/*
 * purva.c - AOT evaluator with purv's external shape.
 *
 * It implements purv.h (RiscvEmulatorInit + Loop over the public state) but does NO
 * decoding: it runs a program already transcoded by the standalone tool. The host
 * installs the op array with RiscvEmulatorSetProgram, then RiscvEmulatorLoop threads
 * over it with an indirect computed goto, like purvs -- each handler jumps straight
 * to the next op's handler. There is zero transcode here; this file doesn't even
 * link transcode.c.
 *
 * The op for guest pc is ops[pc>>2] (one word per instruction), so the cursor IS the
 * pc: pc = (cursor - ops) << 2. Nothing else is tracked -- links, the fault pc, and
 * jump targets all fall out of the cursor, and a jump is a pointer add. There is no
 * side map and no per-op pc bookkeeping.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "purva.h"   /* purv.h + transcode.h (the op vocabulary and field macros) */

int g_itrace;              /* TEMP debug: per-instruction register trace */
unsigned long g_icount;    /* TEMP debug: running instruction counter for trace labels */
uint32_t g_watch;          /* TEMP debug: store-address watchpoint, 0 = off */

static inline __attribute__((always_inline))
void trace_dump(RiscvEmulatorState_t *s, uint32_t *p, uint32_t *base) {
    fprintf(stderr, "I%lu op=%u@%u(pc=%u)", g_icount++, TC_OP(*p), (unsigned)(p - base), (unsigned)(p - base) * 4);
    for (int i = 0; i < 32; i++) fprintf(stderr, " x%d=%x", i, s->x[i]);
    fprintf(stderr, "\n");
}
#define TRACE_STEP() do { if (g_itrace) trace_dump(s, p, base); } while (0)

static inline __attribute__((always_inline))
void wr(RiscvEmulatorState_t *s, uint32_t i, uint32_t v) { if (i) s->x[i] = v; }

/* A single 4-byte store, written the same way h_sw's does (GCC reliably folds this
 * exact shape into one unaligned mov). The fused prologue handler calls it once per
 * saved register into the contiguous save block. */
static inline __attribute__((always_inline)) void st32(uint8_t *q, uint32_t v) {
    q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24);
}
static inline __attribute__((always_inline)) uint32_t ld32(const uint8_t *q) {
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

/* Instructions fetch ONLY from region[RISCV_CODE] (via g_prog/the op cursor below,
 * never through here). A data access -- load OR store -- must NEVER reach it:
 * purva's "code" is packed op words, not real RISC-V bytes (unlike purv, whose
 * code stays real bytes forever, so reading it as data is meaningless but
 * harmless there); there is nothing sane to read, and self-modifying code isn't
 * a thing this engine supports. So the lower half only ever resolves into
 * region[RISCV_RODATA] -- read-only, based at RISCV_HALF - its own length,
 * EXACTLY purv.h's documented formula for a region that grows down from a
 * half's top, unchanged from the original. No extra state or setter needed:
 * purva.ld places rodata to grow down from RISCV_HALF for exactly this reason
 * (a genuinely separate region from code, anchored the same way the engine
 * already computes it, not derived from region[RISCV_CODE].len or anything
 * about how code transcoded). The upper half is unchanged from purv.h's
 * documented model: region[RISCV_HEAP] grows up from RISCV_HALF,
 * region[RISCV_STACK] grows down from 2^32. */
static inline __attribute__((always_inline))
uint8_t *mem_xlate(const RiscvEmulatorState_t *s, uint32_t addr, uint32_t n, int write) {
    if (addr < RISCV_HALF) {
        if (write) return (uint8_t *)0;
        const RiscvEmulatorRegion_t *rodata = &s->region[RISCV_RODATA];
        uint32_t down = RISCV_HALF - rodata->len;
        return (addr >= down && addr + n <= RISCV_HALF) ? rodata->ptr + (addr - down) : (uint8_t *)0;
    }
    const RiscvEmulatorRegion_t *heap = &s->region[RISCV_HEAP];
    uint32_t lo = addr & (RISCV_HALF - 1);
    if (lo + n <= heap->len) return heap->ptr + lo;
    const RiscvEmulatorRegion_t *stack = &s->region[RISCV_STACK];
    uint32_t down = RISCV_HALF - stack->len;
    if (lo >= down && lo + n <= RISCV_HALF) return stack->ptr + (lo - down);
    return (uint8_t *)0;
}

static int32_t sext(uint32_t v, int bits) { int sh = 32 - bits; return (int32_t)(v << sh) >> sh; }

/* PROLOGUE/EPILOGUE regmask bit -> register number, in canonical save rank order
 * (see transcode.h): rank 0=ra, 1=s0, 2=s1, 3..12=s2..s11. */
static const uint8_t rank2reg[13] = { 1, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };

static const Transcoded *g_prog;

void RiscvEmulatorSetProgram(const Transcoded *prog) { g_prog = prog; }

__attribute__((optimize("no-gcse", "no-crossjumping")))
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s_in, uint64_t max) {
    const Transcoded *prog = g_prog;
    if (!prog) return 0;
    uint32_t *base = prog->ops, clen = prog->code_len;
    uint32_t pc = s_in->pc;
    if (pc >= clen) return 0;                                  /* resume pc outside code */

    /* Non-escaping local copy of the whole state for the hot loop. Because &st never
     * reaches an opaque call (mem_xlate/wr/trace_dump are always_inline; ecall and the
     * OOB callbacks are handed s_in, never &st), the compiler can prove that guest
     * memory stores -- uint8_t writes, which legally alias every type -- cannot touch
     * st, so it keeps the register file and region bounds alias-free instead of
     * reloading them across every guest store. st is flushed to *s_in before every
     * return and before any host call, and reloaded after a serviced ecall (which
     * mutates guest registers). */
    RiscvEmulatorState_t st = *s_in;
    RiscvEmulatorState_t *const s = &st;
    #define FLUSH() (*s_in = st)

    static const void *const tbl[RISCV_OP_COUNT] = {
        [RISCV_OP_ADD] = &&h_add, [RISCV_OP_SLL] = &&h_sll, [RISCV_OP_SLT] = &&h_slt,
        [RISCV_OP_SLTU] = &&h_sltu, [RISCV_OP_XOR] = &&h_xor, [RISCV_OP_SRL] = &&h_srl,
        [RISCV_OP_OR] = &&h_or, [RISCV_OP_AND] = &&h_and, [RISCV_OP_SUB] = &&h_sub, [RISCV_OP_SRA] = &&h_sra,
        [RISCV_OP_ADDI] = &&h_addi, [RISCV_OP_SLLI] = &&h_slli, [RISCV_OP_SLTI] = &&h_slti,
        [RISCV_OP_SLTIU] = &&h_sltiu, [RISCV_OP_XORI] = &&h_xori, [RISCV_OP_SRLI] = &&h_srli,
        [RISCV_OP_ORI] = &&h_ori, [RISCV_OP_ANDI] = &&h_andi, [RISCV_OP_SRAI] = &&h_srai,
        [RISCV_OP_MUL] = &&h_mul, [RISCV_OP_MULH] = &&h_mulh, [RISCV_OP_MULHSU] = &&h_mulhsu,
        [RISCV_OP_MULHU] = &&h_mulhu, [RISCV_OP_DIV] = &&h_div, [RISCV_OP_DIVU] = &&h_divu,
        [RISCV_OP_REM] = &&h_rem, [RISCV_OP_REMU] = &&h_remu,
        [RISCV_OP_LB] = &&h_lb, [RISCV_OP_LH] = &&h_lh, [RISCV_OP_LW] = &&h_lw,
        [RISCV_OP_LBU] = &&h_lbu, [RISCV_OP_LHU] = &&h_lhu,
        [RISCV_OP_SB] = &&h_sb, [RISCV_OP_SH] = &&h_sh, [RISCV_OP_SW] = &&h_sw,
        [RISCV_OP_BEQ] = &&h_beq, [RISCV_OP_BNE] = &&h_bne, [RISCV_OP_BLT] = &&h_blt,
        [RISCV_OP_BGE] = &&h_bge, [RISCV_OP_BLTU] = &&h_bltu, [RISCV_OP_BGEU] = &&h_bgeu,
        [RISCV_OP_JAL] = &&h_jal, [RISCV_OP_JALR] = &&h_jalr,
        [RISCV_OP_LUI] = &&h_lui, [RISCV_OP_AUIPC] = &&h_auipc, [RISCV_OP_NOP] = &&h_nop,
        [RISCV_OP_ECALL] = &&h_trap, [RISCV_OP_EBREAK] = &&h_trap, [RISCV_OP_ILLEGAL] = &&h_trap,
        [RISCV_OP_AUIPC_ABS] = &&h_auipc_abs,
        [RISCV_OP_PROLOGUE] = &&h_prologue, [RISCV_OP_EPILOGUE] = &&h_epilogue,
    };
    uint32_t *p = base + (pc >> 2);
    uint32_t w = *p;
    uint64_t k = 0;
    uint32_t a, b;

    /* Straight-line step: one op word forward (the cursor is the pc). */
    #define NEXT() do { k++; w = *++p; TRACE_STEP(); goto *tbl[TC_OP(w)]; } while (0)
    /* Jump to op index `idx`: count it, budget-check (resume pc = idx<<2), relocate. */
    #define RELOC(idx) do { uint32_t i_ = (idx); k++; \
        if (k >= max) { s->pc = i_ << 2; FLUSH(); return k; } p = base + i_; w = *p; TRACE_STEP(); goto *tbl[TC_OP(w)]; } while (0)

    TRACE_STEP();
    goto *tbl[TC_OP(w)];

    h_add:  wr(s, TC_A(w), s->x[TC_B(w)] + s->x[TC_C(w)]);                          NEXT();
    h_sub:  wr(s, TC_A(w), s->x[TC_B(w)] - s->x[TC_C(w)]);                          NEXT();
    h_sll:  wr(s, TC_A(w), s->x[TC_B(w)] << (s->x[TC_C(w)] & 31));                  NEXT();
    h_slt:  wr(s, TC_A(w), (int32_t)s->x[TC_B(w)] < (int32_t)s->x[TC_C(w)]);        NEXT();
    h_sltu: wr(s, TC_A(w), s->x[TC_B(w)] < s->x[TC_C(w)]);                          NEXT();
    h_xor:  wr(s, TC_A(w), s->x[TC_B(w)] ^ s->x[TC_C(w)]);                          NEXT();
    h_srl:  wr(s, TC_A(w), s->x[TC_B(w)] >> (s->x[TC_C(w)] & 31));                  NEXT();
    h_sra:  wr(s, TC_A(w), (uint32_t)((int32_t)s->x[TC_B(w)] >> (s->x[TC_C(w)] & 31))); NEXT();
    h_or:   wr(s, TC_A(w), s->x[TC_B(w)] | s->x[TC_C(w)]);                          NEXT();
    h_and:  wr(s, TC_A(w), s->x[TC_B(w)] & s->x[TC_C(w)]);                          NEXT();

    h_addi:  wr(s, TC_A(w), s->x[TC_B(w)] + TC_IMM(w));                             NEXT();
    h_slli:  wr(s, TC_A(w), s->x[TC_B(w)] << (TC_IMM(w) & 31));                     NEXT();
    h_slti:  wr(s, TC_A(w), (int32_t)s->x[TC_B(w)] < TC_IMM(w));                    NEXT();
    h_sltiu: wr(s, TC_A(w), s->x[TC_B(w)] < (uint32_t)TC_IMM(w));                   NEXT();
    h_xori:  wr(s, TC_A(w), s->x[TC_B(w)] ^ (uint32_t)TC_IMM(w));                   NEXT();
    h_srli:  wr(s, TC_A(w), s->x[TC_B(w)] >> (TC_IMM(w) & 31));                     NEXT();
    h_ori:   wr(s, TC_A(w), s->x[TC_B(w)] | (uint32_t)TC_IMM(w));                   NEXT();
    h_andi:  wr(s, TC_A(w), s->x[TC_B(w)] & (uint32_t)TC_IMM(w));                   NEXT();
    h_srai:  wr(s, TC_A(w), (uint32_t)((int32_t)s->x[TC_B(w)] >> (TC_IMM(w) & 31))); NEXT();

    h_mul:    wr(s, TC_A(w), s->x[TC_B(w)] * s->x[TC_C(w)]); NEXT();
    h_mulh:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; wr(s, TC_A(w), (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32)); NEXT();
    h_mulhsu: a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; wr(s, TC_A(w), (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32)); NEXT();
    h_mulhu:  a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; wr(s, TC_A(w), (uint32_t)(((uint64_t)a * b) >> 32)); NEXT();
    h_div:    a = s->x[TC_B(w)]; b = s->x[TC_C(w)];
              wr(s, TC_A(w), b == 0 ? 0xffffffffu : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b)); NEXT();
    h_divu:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; wr(s, TC_A(w), b == 0 ? 0xffffffffu : a / b); NEXT();
    h_rem:    a = s->x[TC_B(w)]; b = s->x[TC_C(w)];
              wr(s, TC_A(w), b == 0 ? a : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b)); NEXT();
    h_remu:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; wr(s, TC_A(w), b == 0 ? a : a % b); NEXT();

    h_lb: { uint32_t rd = TC_A(w), ad = s->x[TC_B(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 1, 0);
                wr(s, rd, q ? (uint32_t)(int32_t)(int8_t)q[0]
                            : (uint32_t)sext((FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, ad, 0)), 8)); } }
          NEXT();
    h_lh: { uint32_t rd = TC_A(w), ad = s->x[TC_B(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 2, 0);
                wr(s, rd, q ? (uint32_t)(int32_t)(int16_t)(q[0] | q[1] << 8)
                            : (uint32_t)sext((FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, ad, 0)), 16)); } }
          NEXT();
    h_lw: { uint32_t rd = TC_A(w), ad = s->x[TC_B(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 4, 0);
                wr(s, rd, q ? ((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24)
                            : ((FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, ad, 0)))); } }
          NEXT();
    h_lbu: { uint32_t rd = TC_A(w), ad = s->x[TC_B(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 1, 0);
                wr(s, rd, q ? q[0] : (((FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, ad, 0))) & 0xff)); } }
          NEXT();
    h_lhu: { uint32_t rd = TC_A(w), ad = s->x[TC_B(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 2, 0);
                wr(s, rd, q ? ((uint32_t)q[0] | (uint32_t)q[1] << 8)
                            : (((FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, ad, 0))) & 0xffff)); } }
          NEXT();

    h_sb: { uint32_t ad = s->x[TC_B(w)] + TC_IMM(w); b = s->x[TC_A(w)]; uint8_t *q = mem_xlate(s, ad, 1, 1);
            if (q) q[0] = (uint8_t)b; else (FLUSH(), s_in->callback(s_in, RISCV_MEM_STORE, ad, b)); }
          NEXT();
    h_sh: { uint32_t ad = s->x[TC_B(w)] + TC_IMM(w); b = s->x[TC_A(w)]; uint8_t *q = mem_xlate(s, ad, 2, 1);
            if (q) { q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8); } else (FLUSH(), s_in->callback(s_in, RISCV_MEM_STORE, ad, b)); }
          NEXT();
    h_sw: { uint32_t ad = s->x[TC_B(w)] + TC_IMM(w); b = s->x[TC_A(w)]; uint8_t *q = mem_xlate(s, ad, 4, 1);
            extern uint32_t g_watch;
            if (g_watch && ad == g_watch)
                fprintf(stderr, "WATCH sw ad=0x%x val=0x%x at op-idx=%u (pc=%u)\n", ad, b, (unsigned)(p - base), (unsigned)(p - base) * 4);
            if (q) { q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8); q[2] = (uint8_t)(b >> 16); q[3] = (uint8_t)(b >> 24); }
            else (FLUSH(), s_in->callback(s_in, RISCV_MEM_STORE, ad, b)); }
          NEXT();

    h_beq:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a == b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bne:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a != b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_blt:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if ((int32_t)a <  (int32_t)b) RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bge:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if ((int32_t)a >= (int32_t)b) RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bltu: a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a <  b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bgeu: a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a >= b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();

    h_jal:  { uint32_t idx = (uint32_t)(p - base); wr(s, TC_A(w), (idx << 2) + 4);
              RELOC(idx + (uint32_t)(TC_JOFF(w) >> 2)); }
    h_jalr: { uint32_t t = (s->x[TC_B(w)] + TC_IMM(w)) & ~1u; wr(s, TC_A(w), ((uint32_t)(p - base) << 2) + 4); k++;
              if (t >= clen || k >= max) { s->pc = t; FLUSH(); return k; }
              p = base + (t >> 2); w = *p; TRACE_STEP(); goto *tbl[TC_OP(w)]; }

    h_lui:   wr(s, TC_A(w), TC_UIMM(w) << 12); NEXT();
    /* Cursor-based: correct only when nothing before this op has fused (op-index ==
     * pc/4 still holds). transcode_ex only emits this form when it verified that --
     * see transcode.h's RISCV_OP_AUIPC_ABS note. */
    h_auipc: wr(s, TC_A(w), ((uint32_t)(p - base) << 2) + (TC_UIMM(w) << 12)); NEXT();
    /* Drift has occurred upstream: the transcoder baked the real absolute value
     * (computed from the TRUE pc, not a drifted cursor) into the next word. */
    h_auipc_abs: wr(s, TC_A(w), p[1]); k++; p += 2; w = *p; TRACE_STEP(); goto *tbl[TC_OP(w)];
    h_nop:   NEXT();

    /* Fused prologue (transcode.h): allocate the frame and save the callee-saved set
     * at the top of it. The p-th set register (rank order: ra,s0,s1,s2..) sits at
     * sp-4-4p, independent of frame size. The whole run is one op word, so advance
     * one word into the body (k counts the addi + every save it stood in for). */
    h_prologue: {
        uint32_t rmask = (w >> 13) & 0x1fffu, frame = w & 0x1fffu;
        uint32_t cnt = (uint32_t)__builtin_popcount(rmask), sp0 = s->x[2];
        uint8_t *q = mem_xlate(s, sp0 - 4 * cnt, 4 * cnt, 1);      /* one xlate for the whole block */
        if (q) {
            q += 4 * cnt;                                          /* one past the top slot */
            /* rank order ra,s0,s1,s2..s11 -> descending slots; q steps down only on a set bit. */
            #define SAVE(bit, reg) if (rmask & (1u << (bit))) { q -= 4; st32(q, s->x[(reg)]); }
            SAVE(0,1) SAVE(1,8) SAVE(2,9) SAVE(3,18) SAVE(4,19) SAVE(5,20) SAVE(6,21)
            SAVE(7,22) SAVE(8,23) SAVE(9,24) SAVE(10,25) SAVE(11,26) SAVE(12,27)
            #undef SAVE
        } else {                                                  /* off-stack fallback: per-reg */
            uint32_t addr = sp0, m = rmask;
            while (m) { uint32_t r = (uint32_t)__builtin_ctz(m); m &= m - 1; addr -= 4;
                uint32_t v = s->x[rank2reg[r]]; uint8_t *qq = mem_xlate(s, addr, 4, 1);
                if (qq) st32(qq, v); else (FLUSH(), s_in->callback(s_in, RISCV_MEM_STORE, addr, v)); }
        }
        s->x[2] = sp0 - frame;
        k += cnt + 1; w = *++p; TRACE_STEP(); goto *tbl[TC_OP(w)];
    }
    /* Fused epilogue (transcode.h): restore the callee-saved set from the top of the
     * frame (p-th set register at sp+frame-4-4p), deallocate, and return to ra. The
     * whole run is one op word and this op never falls through -- it jumps to ra.
     * Only the ret checks the budget, exactly as the unfused sequence did. */
    h_epilogue: {
        uint32_t rmask = (w >> 13) & 0x1fffu, frame = w & 0x1fffu;
        uint32_t cnt = (uint32_t)__builtin_popcount(rmask), sp0 = s->x[2];
        uint8_t *q = mem_xlate(s, sp0 + frame - 4 * cnt, 4 * cnt, 0);   /* one xlate for the whole block */
        if (q) {
            q += 4 * cnt;
            #define REST(bit, reg) if (rmask & (1u << (bit))) { q -= 4; s->x[(reg)] = ld32(q); }
            REST(0,1) REST(1,8) REST(2,9) REST(3,18) REST(4,19) REST(5,20) REST(6,21)
            REST(7,22) REST(8,23) REST(9,24) REST(10,25) REST(11,26) REST(12,27)
            #undef REST
        } else {                                                       /* off-stack fallback: per-reg */
            uint32_t addr = sp0 + frame, m = rmask;
            while (m) { uint32_t r = (uint32_t)__builtin_ctz(m); m &= m - 1; addr -= 4;
                uint8_t *qq = mem_xlate(s, addr, 4, 0);
                s->x[rank2reg[r]] = qq ? ld32(qq) : (FLUSH(), s_in->callback(s_in, RISCV_MEM_LOAD, addr, 0)); }
        }
        s->x[2] = sp0 + frame;
        uint32_t t = s->x[1] & ~1u; k += cnt + 2;
        if (t >= clen || k >= max) { s->pc = t; FLUSH(); return k; }
        p = base + (t >> 2); w = *p; TRACE_STEP(); goto *tbl[TC_OP(w)];
    }

    h_trap: {
        uint32_t pc_ = (uint32_t)(p - base) << 2;
        uint8_t op = TC_OP(w);
        s->pc = pc_; s->inst = w; k++;     /* inst is OUR packed op word, not raw RISC-V */
        FLUSH();                           /* host handler works on *s_in, never &st */
        int stop = (op == RISCV_OP_ECALL)  ? (s_in->ecall   ? s_in->ecall(s_in)   : 0)
                 : (op == RISCV_OP_EBREAK) ? (s_in->ebreak  ? s_in->ebreak(s_in)  : 1)
                 :                           (s_in->illegal ? s_in->illegal(s_in) : 1);
        st = *s_in;                        /* reload: a serviced ecall mutates guest regs */
        if (stop) return k;                /* *s_in already holds the final state */
        if (k >= max) { s->pc = pc_ + 4; FLUSH(); return k; } /* serviced syscall: resume */
        w = *++p; TRACE_STEP(); goto *tbl[TC_OP(w)];
    }
    #undef NEXT
    #undef RELOC
    #undef FLUSH
}

/* ------------------------------------------------------------------ init */

/* Installed by RiscvEmulatorInit so s->callback is ALWAYS valid; the load/store miss
 * paths invoke it unconditionally rather than null-checking. A miss with no host
 * handler thus reads 0 / drops the write, same as a NULL callback would have. */
static uint32_t default_callback(RiscvEmulatorState_t *s, int op, uint32_t addr, uint32_t value) {
    (void)s; (void)op; (void)addr; (void)value; return 0;
}

void RiscvEmulatorInit(RiscvEmulatorState_t *s,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t rodata,
                       RiscvEmulatorRegion_t heap, RiscvEmulatorRegion_t stack) {
    memset(s, 0, sizeof *s);
    s->region[RISCV_CODE]   = code;
    s->region[RISCV_RODATA] = rodata;
    s->region[RISCV_HEAP]   = heap;
    s->region[RISCV_STACK]  = stack;
    s->callback = default_callback;
    s->x[2] = 0;
    s->pc = 0;
}
