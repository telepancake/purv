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
#include <string.h>
#ifdef PURVA_PROFILE
#include <stdlib.h>
#endif

#include "purva.h"   /* purv.h + transcode.h (the op vocabulary and field macros) */
#include "../purvmemop.h"   /* the custom-0 bulk mem/str instruction (RISCV_OP_MEMOP) */

/* Little-endian assemble/scatter of n bytes. n is a compile-time constant at every
 * call site, so GCC folds these to a single (unaligned) load/store of that width. */
static inline __attribute__((always_inline)) uint32_t ld_le(const uint8_t *q, uint32_t n) {
    uint32_t v = q[0];
    if (n > 1) v |= (uint32_t)q[1] << 8;
    if (n > 2) v |= (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    return v;
}
static inline __attribute__((always_inline)) void st_le(uint8_t *q, uint32_t n, uint32_t v) {
    q[0] = (uint8_t)v; if (n > 1) q[1] = (uint8_t)(v >> 8);
    if (n > 2) { q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24); }
}

/* Data memory is the two self-describing regions of the state (see purv.h):
 *
 *   s->writable   the stack and heap as ONE contiguous buffer, based at
 *                 RISCV_HALF - stack_len (the stack bottom) and spanning up through
 *                 the heap. Its len is the whole span.
 *   s->readonly   rodata, based at 0 - rodata_len -- it grows down from 0, living at
 *                 small negative addresses. (Instruction fetch is the op cursor, not
 *                 a region -- purva's "code" is packed op words, not data.)
 *
 * mem_w resolves a WRITABLE block to a host pointer (or NULL); mem_r resolves a READ
 * block (writable, then read-only). One base-relative bounded check each: `rel < len
 * && n <= len - rel` is correct for any n -- once rel < len, len - rel can't
 * underflow. The LOAD/STORE macros and the fused prologue/epilogue build on these. */
static inline __attribute__((always_inline))
uint8_t *mem_w(const RiscvEmulatorState_t *s, uint32_t a, uint32_t n) {
    uint32_t rel = a - s->writable.base;
    return (rel < s->writable.len && n <= s->writable.len - rel) ? s->writable.ptr + rel : (uint8_t *)0;
}
static inline __attribute__((always_inline))
uint8_t *mem_r(const RiscvEmulatorState_t *s, uint32_t a, uint32_t n) {
    uint8_t *q = mem_w(s, a, n);
    if (q) return q;
    uint32_t rel = a - s->readonly.base;
    return (rel < s->readonly.len && n <= s->readonly.len - rel) ? s->readonly.ptr + rel : (uint8_t *)0;
}

/* PROLOGUE/EPILOGUE regmask bit -> register number, in canonical save rank order
 * (see transcode.h): rank 0=ra, 1=s0, 2=s1, 3..12=s2..s11. */
static const uint8_t rank2reg[13] = { 1, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };

/* A fused prologue/epilogue only ever touches the callee-saved band -- at most all 13
 * of the set, 52 bytes -- immediately below the frame's TOP (the caller's sp). The op
 * comes from our own transcoder, never hostile input, so we trust the saved set fits the
 * frame and check just that fixed 52-byte band, not the whole (variable) frame: one
 * compare covers ANY frame. frame_top returns the host pointer to `top` (walk the saves
 * DOWN from there) when [top-52, top) is in the writable region, else NULL -> the cold
 * out-of-line slow path (near the stack's very bottom, or an off-stack sp). */
#define FRAME_SAVE_SPAN (13 * 4)
static inline __attribute__((always_inline))
uint8_t *frame_top(const RiscvEmulatorState_t *s, uint32_t top) {
    uint32_t rel = top - s->writable.base;
    return (uint32_t)(rel - FRAME_SAVE_SPAN) <= s->writable.len - FRAME_SAVE_SPAN
           ? s->writable.ptr + rel : (uint8_t *)0;
}

/* Cold fallbacks for frame_top misses: store/load each saved register on its own,
 * routing an out-of-region slot to the callback. Out-of-line (never inlined) so the
 * hot prologue/epilogue handlers stay small. Both return the saved-register count for
 * the instruction budget. */
static uint32_t __attribute__((noinline, cold))
prologue_slow(RiscvEmulatorState_t *s, uint32_t rmask, uint32_t sp0) {
    uint32_t addr = sp0, m = rmask, cnt = 0;
    while (m) { uint32_t r = (uint32_t)__builtin_ctz(m); m &= m - 1; addr -= 4; cnt++;
        uint32_t v = s->x[rank2reg[r]]; uint8_t *qq = mem_w(s, addr, 4);
        if (qq) st_le(qq, 4, v); else s->callback(s, RISCV_MEM_STORE, addr, v); }
    return cnt;
}
static uint32_t __attribute__((noinline, cold))
epilogue_slow(RiscvEmulatorState_t *s, uint32_t rmask, uint32_t top) {
    uint32_t addr = top, m = rmask, cnt = 0;
    while (m) { uint32_t r = (uint32_t)__builtin_ctz(m); m &= m - 1; addr -= 4; cnt++;
        uint8_t *qq = mem_r(s, addr, 4);
        s->x[rank2reg[r]] = qq ? ld_le(qq, 4) : s->callback(s, RISCV_MEM_LOAD, addr, 0); }
    return cnt;
}

static const Transcoded *g_prog;

#ifdef PURVA_PROFILE
/* Profiling build (-DPURVA_PROFILE): one execution counter per op-word index,
 * bumped at every dispatch. The evaluator stays branch-identical; the counters
 * feed profile.py, which joins them with the decoded image to rank hot ops and
 * adjacent-pair patterns -- the evidence fusion decisions are made from. The
 * host dumps purva_prof_counts (n_ops entries) after the run. */
uint64_t *purva_prof_counts;
static uint64_t *prof_alloc(uint32_t n) {
    return (uint64_t *)calloc(n ? n : 1, sizeof(uint64_t));
}
#define PROF(p) (purva_prof_counts[(p) - base]++)
#else
#define PROF(p) ((void)0)
#endif

void RiscvEmulatorSetProgram(const Transcoded *prog) {
    g_prog = prog;
#ifdef PURVA_PROFILE
    free(purva_prof_counts);
    purva_prof_counts = prof_alloc(prog ? prog->n_ops : 0);
#endif
}

__attribute__((optimize("no-gcse", "no-crossjumping")))
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint64_t max) {
    const Transcoded *prog = g_prog;
    if (!prog) return 0;
    uint32_t *base = prog->ops, clen = prog->code_len;
    uint32_t pc = s->pc;
    if (pc >= clen) return 0;                                  /* resume pc outside code */

    /* All 64 values the 6-bit op field (w>>26) can select are populated: the real ops
     * below, then every unused encoding [RISCV_OP_COUNT, 63] filled with h_trap. So a
     * garbage op -- e.g. a jalr/ret landing on a data word -- dispatches to h_trap ->
     * illegal rather than running off the table into a wild `goto *NULL`. */
    static const void *const tbl[64] = {
        [RISCV_OP_COUNT ... 63] = &&h_trap,
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
        [RISCV_OP_PROLOGUE] = &&h_prologue, [RISCV_OP_EPILOGUE] = &&h_epilogue,
        [RISCV_OP_LI_LO] = &&h_li_lo, [RISCV_OP_LI_HI] = &&h_li_hi,
        [RISCV_OP_SHADD] = &&h_shadd, [RISCV_OP_LWX] = &&h_lwx,
        [RISCV_OP_LWLW] = &&h_lwlw, [RISCV_OP_LWJALR] = &&h_lwjalr,
        [RISCV_OP_LW_BZ] = &&h_lw_bz, [RISCV_OP_LBU_BZ] = &&h_lbu_bz,
        [RISCV_OP_LWSW] = &&h_lwsw, [RISCV_OP_VCALL] = &&h_vcall,
        [RISCV_OP_MEMOP] = &&h_memop,
    };
    uint32_t *p = base + (pc >> 2);
    uint32_t w = *p;
    uint64_t k = 0;
    uint32_t a, b;

    /* Straight-line step: one op word forward (the cursor is the pc). */
    #define NEXT() do { k++; w = *++p; PROF(p); goto *tbl[TC_OP(w)]; } while (0)
    /* Jump to op index `idx`: count it, budget-check (resume pc = idx<<2), relocate. */
    #define RELOC(idx) do { uint32_t i_ = (idx); k++; \
        if (k >= max) { s->pc = i_ << 2; return k; } p = base + i_; w = *p; PROF(p); goto *tbl[TC_OP(w)]; } while (0)
    /* One load, one store. addr = x[rs1] + imm. LOAD(T): T is the loaded value's C
     * type -- sizeof(T) picks the byte width, and the cast to (T) then int32_t does
     * the sign/zero extension the width implies (LB/LH signed, LBU/LHU unsigned, LW
     * full word), for both a region hit and a callback miss. A load into x0 is
     * discarded (the only rd==0 case a load reaches; JAL/JALR handle their own) --
     * the transcoder drops every x0-destination ALU/LUI/AUIPC op during translation,
     * so those handlers write s->x[...] with no guard. STORE(n) writes n bytes; a
     * miss (or a store to read-only) goes to the callback. */
    #define LOAD(T) do { uint32_t rd_ = TC_A(w); \
        if (rd_) { uint32_t a_ = s->x[TC_B(w)] + TC_IMM(w); const uint8_t *q_ = mem_r(s, a_, sizeof(T)); \
            s->x[rd_] = (uint32_t)(int32_t)(T)(q_ ? ld_le(q_, sizeof(T)) \
                                                  : s->callback(s, RISCV_MEM_LOAD, a_, 0)); } } while (0)
    #define STORE(n) do { uint32_t a_ = s->x[TC_B(w)] + TC_IMM(w), v_ = s->x[TC_A(w)]; \
        uint8_t *q_ = mem_w(s, a_, (n)); \
        if (q_) st_le(q_, (n), v_); else s->callback(s, RISCV_MEM_STORE, a_, v_); } while (0)

    PROF(p);
    goto *tbl[TC_OP(w)];

    h_add:  s->x[TC_A(w)] = s->x[TC_B(w)] + s->x[TC_C(w)];                          NEXT();
    h_sub:  s->x[TC_A(w)] = s->x[TC_B(w)] - s->x[TC_C(w)];                          NEXT();
    h_sll:  s->x[TC_A(w)] = s->x[TC_B(w)] << (s->x[TC_C(w)] & 31);                  NEXT();
    h_slt:  s->x[TC_A(w)] = (int32_t)s->x[TC_B(w)] < (int32_t)s->x[TC_C(w)];        NEXT();
    h_sltu: s->x[TC_A(w)] = s->x[TC_B(w)] < s->x[TC_C(w)];                          NEXT();
    h_xor:  s->x[TC_A(w)] = s->x[TC_B(w)] ^ s->x[TC_C(w)];                          NEXT();
    h_srl:  s->x[TC_A(w)] = s->x[TC_B(w)] >> (s->x[TC_C(w)] & 31);                  NEXT();
    h_sra:  s->x[TC_A(w)] = (uint32_t)((int32_t)s->x[TC_B(w)] >> (s->x[TC_C(w)] & 31)); NEXT();
    h_or:   s->x[TC_A(w)] = s->x[TC_B(w)] | s->x[TC_C(w)];                          NEXT();
    h_and:  s->x[TC_A(w)] = s->x[TC_B(w)] & s->x[TC_C(w)];                          NEXT();

    h_addi:  s->x[TC_A(w)] = s->x[TC_B(w)] + TC_IMM(w);                             NEXT();
    h_slli:  s->x[TC_A(w)] = s->x[TC_B(w)] << (TC_IMM(w) & 31);                     NEXT();
    h_slti:  s->x[TC_A(w)] = (int32_t)s->x[TC_B(w)] < TC_IMM(w);                    NEXT();
    h_sltiu: s->x[TC_A(w)] = s->x[TC_B(w)] < (uint32_t)TC_IMM(w);                   NEXT();
    h_xori:  s->x[TC_A(w)] = s->x[TC_B(w)] ^ (uint32_t)TC_IMM(w);                   NEXT();
    h_srli:  s->x[TC_A(w)] = s->x[TC_B(w)] >> (TC_IMM(w) & 31);                     NEXT();
    h_ori:   s->x[TC_A(w)] = s->x[TC_B(w)] | (uint32_t)TC_IMM(w);                   NEXT();
    h_andi:  s->x[TC_A(w)] = s->x[TC_B(w)] & (uint32_t)TC_IMM(w);                   NEXT();
    h_srai:  s->x[TC_A(w)] = (uint32_t)((int32_t)s->x[TC_B(w)] >> (TC_IMM(w) & 31)); NEXT();

    h_mul:    s->x[TC_A(w)] = s->x[TC_B(w)] * s->x[TC_C(w)]; NEXT();
    h_mulh:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; s->x[TC_A(w)] = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32); NEXT();
    h_mulhsu: a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; s->x[TC_A(w)] = (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32); NEXT();
    h_mulhu:  a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; s->x[TC_A(w)] = (uint32_t)(((uint64_t)a * b) >> 32); NEXT();
    h_div:    a = s->x[TC_B(w)]; b = s->x[TC_C(w)];
              s->x[TC_A(w)] = b == 0 ? 0xffffffffu : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b); NEXT();
    h_divu:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; s->x[TC_A(w)] = b == 0 ? 0xffffffffu : a / b; NEXT();
    h_rem:    a = s->x[TC_B(w)]; b = s->x[TC_C(w)];
              s->x[TC_A(w)] = b == 0 ? a : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); NEXT();
    h_remu:   a = s->x[TC_B(w)]; b = s->x[TC_C(w)]; s->x[TC_A(w)] = b == 0 ? a : a % b; NEXT();

    h_lb:  LOAD(int8_t);   NEXT();
    h_lh:  LOAD(int16_t);  NEXT();
    h_lw:  LOAD(int32_t);  NEXT();
    h_lbu: LOAD(uint8_t);  NEXT();
    h_lhu: LOAD(uint16_t); NEXT();

    h_sb:  STORE(1); NEXT();
    h_sh:  STORE(2); NEXT();
    h_sw:  STORE(4); NEXT();

    h_beq:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a == b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bne:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a != b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_blt:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if ((int32_t)a <  (int32_t)b) RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bge:  a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if ((int32_t)a >= (int32_t)b) RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bltu: a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a <  b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();
    h_bgeu: a = s->x[TC_A(w)]; b = s->x[TC_B(w)]; if (a >= b)                   RELOC((uint32_t)(p - base) + (uint32_t)(TC_IMM(w) >> 2)); NEXT();

    /* JAL/JALR write the link register, which is x0 for a bare jump (`j`, `ret`) --
     * discard that write. */
    h_jal:  { uint32_t idx = (uint32_t)(p - base), rd = TC_A(w); if (rd) s->x[rd] = (idx << 2) + 4;
              RELOC(idx + (uint32_t)(TC_JOFF(w) >> 2)); }
    h_jalr: { uint32_t t = (s->x[TC_B(w)] + TC_IMM(w)) & ~1u, rd = TC_A(w);
              if (rd) s->x[rd] = ((uint32_t)(p - base) << 2) + 4;
              k++;
              if (t >= clen || k >= max) { s->pc = t; return k; }
              p = base + (t >> 2); w = *p; PROF(p); goto *tbl[TC_OP(w)]; }

    /* Pair-fused ops (transcode.h): each stands in for TWO instructions, so each
     * counts k twice -- the fuel stays the instruction count, as the fused
     * prologue/epilogue already do. The transcoder only emits them where the
     * intermediate register is clobbered by the pair itself, so one rd write
     * (guaranteed nonzero) replays both instructions' architectural effects. */
    h_shadd: s->x[TC_A(w)] = s->x[TC_B(w)] + (s->x[TC_C(w)] << TC_SH(w)); k++; NEXT();
    h_lwx: { uint32_t a_ = s->x[TC_B(w)] + s->x[TC_C(w)] + (uint32_t)TC_OFF11(w);
             const uint8_t *q_ = mem_r(s, a_, 4);
             s->x[TC_A(w)] = q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0);
             k++; NEXT(); }
    h_lwlw: { uint32_t a_ = s->x[TC_B(w)] + (uint32_t)TC_O1(w);
              const uint8_t *q_ = mem_r(s, a_, 4);
              a_ = (q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0)) + (uint32_t)TC_O2(w);
              q_ = mem_r(s, a_, 4);
              s->x[TC_A(w)] = q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0);
              k++; NEXT(); }
    h_lwsw: { uint32_t a_ = s->x[TC_B(w)] + TC_W1(w);
              const uint8_t *q_ = mem_r(s, a_, 4);
              uint32_t v_ = q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0);
              s->x[TC_A(w)] = v_;                     /* T first: the store base may BE T */
              a_ = s->x[TC_C(w)] + TC_W2(w);
              uint8_t *qw_ = mem_w(s, a_, 4);
              if (qw_) st_le(qw_, 4, v_); else s->callback(s, RISCV_MEM_STORE, a_, v_);
              k++; NEXT(); }
    /* The vtable call: load the vptr, load the slot, call it. LWLW's loads, then
     * LWJALR's tail. rd ends up caller-saved-clobbered garbage-by-ABI in compiled
     * code, but write it anyway -- replaying the exact effects costs nothing. */
    h_vcall: { uint32_t a_ = s->x[TC_B(w)] + (uint32_t)TC_O1(w);
               const uint8_t *q_ = mem_r(s, a_, 4);
               a_ = (q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0)) + (uint32_t)TC_O2(w);
               q_ = mem_r(s, a_, 4);
               uint32_t t = (q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0)) & ~1u;
               s->x[TC_A(w)] = t;
               s->x[1] = ((uint32_t)(p - base) << 2) + 4;
               k += 3;
               if (t >= clen || k >= max) { s->pc = t; return k; }
               p = base + (t >> 2); w = *p; PROF(p); goto *tbl[TC_OP(w)]; }
    h_lwjalr: { uint32_t a_ = s->x[TC_B(w)] + TC_IMM(w);
                const uint8_t *q_ = mem_r(s, a_, 4);
                uint32_t t = (q_ ? ld_le(q_, 4) : s->callback(s, RISCV_MEM_LOAD, a_, 0)) & ~1u;
                s->x[TC_A(w)] = t;
                s->x[1] = ((uint32_t)(p - base) << 2) + 4;
                k += 2;
                if (t >= clen || k >= max) { s->pc = t; return k; }
                p = base + (t >> 2); w = *p; PROF(p); goto *tbl[TC_OP(w)]; }
    /* Two-word load+branch-zero: word2 is the baked op-relative displacement
     * (x4, like a branch imm), its bit0 the condition (1 = branch on zero).
     * Fall through skips both words. */
    #define LOADBZ(T) do { uint32_t a_ = s->x[TC_B(w)] + TC_IMM(w); \
        const uint8_t *q_ = mem_r(s, a_, sizeof(T)); \
        uint32_t v_ = (uint32_t)(int32_t)(T)(q_ ? ld_le(q_, sizeof(T)) \
                                                : s->callback(s, RISCV_MEM_LOAD, a_, 0)); \
        s->x[TC_A(w)] = v_; k++; \
        if ((v_ == 0) == (p[1] & 1)) RELOC((uint32_t)(p - base) + ((int32_t)p[1] >> 2)); \
        k++; w = *(p += 2); PROF(p); goto *tbl[TC_OP(w)]; } while (0)
    h_lw_bz:  LOADBZ(int32_t);
    h_lbu_bz: LOADBZ(uint8_t);
    #undef LOADBZ



    h_lui:   s->x[TC_A(w)] = TC_UIMM(w) << 12; NEXT();
    /* Only CODE-address auipc reach here (data auipc are fused to LI at transcode time).
     * The value is cursor-based -- (this op's index)*4 + uimm<<12 -- which is exact
     * whatever the fusion drift, because the cursor IS the op position; tctool re-encoded
     * uimm (and the paired lo12) for the op-space displacement. See transcode.h. */
    h_auipc: s->x[TC_A(w)] = ((uint32_t)(p - base) << 2) + (TC_UIMM(w) << 12); NEXT();
    /* Materialise a data address in one op (transcode.h): a fused `la`, or a lone data
     * auipc. LI_LO is the value straight (rodata just below 0); LI_HI adds RISCV_HALF
     * (globals). Any lo12 load/store/addi that used the auipc's rd still follows. */
    h_li_lo: s->x[TC_A(w)] = (uint32_t)TC_JOFF(w);              NEXT();
    h_li_hi: s->x[TC_A(w)] = (uint32_t)TC_JOFF(w) + RISCV_HALF; NEXT();
    h_nop:   NEXT();

    /* Fused prologue (transcode.h): allocate the frame and save the callee-saved set
     * at the top of it. The p-th set register (rank order: ra,s0,s1,s2..) sits at
     * sp-4-4p, independent of frame size. The whole run is one op word, so advance
     * one word into the body (k counts the addi + every save it stood in for). */
    h_prologue: {
        uint32_t rmask = (w >> 13) & 0x1fffu, frame = w & 0x1fffu, sp0 = s->x[2], cnt = 0;
        /* Saves sit just below the caller's sp (== sp0 here). One fixed-band check; walk
         * the set DOWN from the top, tallying cnt for the k budget. */
        uint8_t *q = frame_top(s, sp0);
        if (q) {
            /* rank order ra,s0,s1,s2..s11 -> descending slots; q steps down only on a set bit. */
            #define SAVE(bit, reg) if (rmask & (1u << (bit))) { q -= 4; st_le(q, 4, s->x[(reg)]); cnt++; }
            SAVE(0,1) SAVE(1,8) SAVE(2,9) SAVE(3,18) SAVE(4,19) SAVE(5,20) SAVE(6,21)
            SAVE(7,22) SAVE(8,23) SAVE(9,24) SAVE(10,25) SAVE(11,26) SAVE(12,27)
            #undef SAVE
        } else cnt = prologue_slow(s, rmask, sp0);
        s->x[2] = sp0 - frame;
        k += cnt + 1; w = *++p; PROF(p); goto *tbl[TC_OP(w)];
    }
    /* Fused epilogue (transcode.h): restore the callee-saved set from the top of the
     * frame (p-th set register at sp+frame-4-4p), deallocate, and return to ra. The
     * whole run is one op word and this op never falls through -- it jumps to ra.
     * Only the ret checks the budget, exactly as the unfused sequence did. */
    h_epilogue: {
        uint32_t rmask = (w >> 13) & 0x1fffu, frame = w & 0x1fffu, sp0 = s->x[2], top = sp0 + frame, cnt = 0;
        /* Mirror of the prologue: the restore band is just below the caller's sp
         * (== sp0 + frame). One fixed-band check; walk the set down from the top. */
        uint8_t *q = frame_top(s, top);
        if (q) {
            #define REST(bit, reg) if (rmask & (1u << (bit))) { q -= 4; s->x[(reg)] = ld_le(q, 4); cnt++; }
            REST(0,1) REST(1,8) REST(2,9) REST(3,18) REST(4,19) REST(5,20) REST(6,21)
            REST(7,22) REST(8,23) REST(9,24) REST(10,25) REST(11,26) REST(12,27)
            #undef REST
        } else cnt = epilogue_slow(s, rmask, top);
        s->x[2] = top;
        uint32_t t = s->x[1] & ~1u; k += cnt + 2;
        if (t >= clen || k >= max) { s->pc = t; return k; }
        p = base + (t >> 2); w = *p; PROF(p); goto *tbl[TC_OP(w)];
    }

    /* The bulk mem/str instruction (transcode.h RISCV_OP_MEMOP; purvmemop.h does
     * the work -- one host memmove/memset/scan over the resolved regions, with
     * bytewise callback routing on a miss). Its return is the fuel charge: what
     * the loop the instruction replaced would have cost, charged in full even
     * though the op is atomic (budget checks happen at the next jump, the same
     * overshoot rule as every straight-line run). */
    h_memop: {
        k += purv_memop(s, w & 7, TC_A(w), TC_B(w), TC_C(w));
        NEXT();
    }

    h_trap: {
        uint32_t pc_ = (uint32_t)(p - base) << 2;
        uint8_t op = TC_OP(w);
        s->pc = pc_; s->inst = w; k++;     /* inst is OUR packed op word, not raw RISC-V */
        int stop = (op == RISCV_OP_ECALL)  ? (s->ecall   ? s->ecall(s)   : 0)
                 : (op == RISCV_OP_EBREAK) ? (s->ebreak  ? s->ebreak(s)  : 1)
                 :                           (s->illegal ? s->illegal(s) : 1);
        if (stop) return k;
        if (k >= max) { s->pc = pc_ + 4; return k; }          /* serviced syscall: resume */
        w = *++p; PROF(p); goto *tbl[TC_OP(w)];
    }
    #undef NEXT
    #undef RELOC
    #undef LOAD
    #undef STORE
}

/* ------------------------------------------------------------------ init */

/* Installed by RiscvEmulatorInit so s->callback is ALWAYS valid; the load/store miss
 * paths invoke it unconditionally rather than null-checking. A miss with no host
 * handler thus reads 0 / drops the write, same as a NULL callback would have. */
static uint32_t default_callback(RiscvEmulatorState_t *s, int op, uint32_t addr, uint32_t value) {
    (void)s; (void)op; (void)addr; (void)value; return 0;
}

void RiscvEmulatorInit(RiscvEmulatorState_t *s,
                       RiscvEmulatorRegion_t readonly, RiscvEmulatorRegion_t writable) {
    memset(s, 0, sizeof *s);
    /* purva's readonly region is rodata alone (code is fetched via the op cursor,
     * installed with RiscvEmulatorSetProgram, and is not data-addressable); the host
     * bases it at 0 - rodata.len. writable is the stack+heap as one span. */
    s->readonly = readonly;
    s->writable = writable;
    s->callback = default_callback;
    s->x[2] = RISCV_HALF;                         /* sp: stack grows down from RISCV_HALF */
    s->pc = 0;
}
