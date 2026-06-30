/*
 * purva.c - AOT evaluator with purv's external shape.
 *
 * It implements purv.h (RiscvEmulatorInit + Loop over the public state struct), but
 * does NO decoding of its own: it runs a program that was already transcoded by the
 * standalone tool (transcode.c). The host installs that program with
 * RiscvEmulatorSetProgram, then RiscvEmulatorLoop threads over the packed op stream
 * with an indirect computed goto, like purvs -- each handler jumps straight to the
 * next op's handler, so the branch predictor can specialise per op. There is zero
 * transcode in here; this file does not even link transcode.c.
 *
 * A running guest pc is kept beside the op cursor (pc += 4 per op, RV32IM), so links
 * (jal/jalr) and the fault pc are free, and a slice boundary or an indirect jump
 * resumes cleanly. Control transfers locate their destination op through the
 * program's pc->op map; a target outside the image (e.g. a return sentinel) ends the
 * slice with pc left there -- the same short-return contract purv's Loop has.
 */
#include <stdint.h>
#include <string.h>

#include "purva.h"   /* purv.h + transcode.h (the op vocabulary and packed format) */

/* ---- register write (x0 stays zero) and one address translation ---- */
static void wr(RiscvEmulatorState_t *s, uint32_t i, uint32_t v) { if (i) s->x[i] = v; }

static inline __attribute__((always_inline))
uint8_t *mem_xlate(const RiscvEmulatorState_t *s, uint32_t addr, uint32_t n, int write) {
    if (write && addr < RISCV_HALF) return (uint8_t *)0;     /* lower half is read-only */
    const RiscvEmulatorRegion_t *r = &s->region[(addr >> 31) << 1];
    uint32_t lo = addr & (RISCV_HALF - 1);
    if (lo + n <= r[0].len) return r[0].ptr + lo;
    uint32_t down = RISCV_HALF - r[1].len;
    if (lo >= down && lo + n <= RISCV_HALF) return r[1].ptr + (lo - down);
    return (uint8_t *)0;
}

static int32_t sext(uint32_t v, int bits) { int sh = 32 - bits; return (int32_t)(v << sh) >> sh; }

/* The transcoded program the host installed (see RiscvEmulatorSetProgram). */
static const Transcoded *g_prog;

void RiscvEmulatorSetProgram(const Transcoded *prog) { g_prog = prog; }

uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint64_t max) {
    const Transcoded *prog = g_prog;
    if (!prog) return 0;                                       /* no program installed */
    uint32_t *base = prog->ops, *map = prog->map, clen = prog->code_len;
    uint32_t pc = s->pc;
    if (pc >= clen || map[pc >> 1] == TC_SENTINEL) return 0;   /* resume pc outside image */

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
        [RISCV_OP_LUI] = &&h_lui, [RISCV_OP_NOP] = &&h_nop,
        [RISCV_OP_ECALL] = &&h_trap, [RISCV_OP_EBREAK] = &&h_trap, [RISCV_OP_ILLEGAL] = &&h_trap,
    };
    uint32_t *p = base + map[pc >> 1];
    uint32_t w = *p;
    uint64_t k = 0;
    uint32_t a, b;

    /* Straight-line step: advance one (or two) op words and dispatch on the next op. */
    #define NEXT(sz) do { k++; pc += 4; p += (sz); w = *p; goto *tbl[TC_OP(w)]; } while (0)
    /* Control transfer to guest pc T: count it, bound/budget-check, relocate via map. */
    #define JUMP(T) do { uint32_t t_ = (T); k++; pc = t_; \
        if (t_ >= clen || map[t_ >> 1] == TC_SENTINEL || k >= max) { s->pc = t_; return k; } \
        p = base + map[t_ >> 1]; w = *p; goto *tbl[TC_OP(w)]; } while (0)
    /* Conditional branch: taken -> target pc (word 1); not-taken -> fall through. */
    #define BR(cond) do { uint32_t tgt = p[1]; if (cond) JUMP(tgt); \
        k++; pc += 4; if (k >= max) { s->pc = pc; return k; } p += 2; w = *p; goto *tbl[TC_OP(w)]; } while (0)

    goto *tbl[TC_OP(w)];

    h_add:  wr(s, TC_RD(w), s->x[TC_RS1(w)] + s->x[TC_RS2L(w)]);                          NEXT(1);
    h_sub:  wr(s, TC_RD(w), s->x[TC_RS1(w)] - s->x[TC_RS2L(w)]);                          NEXT(1);
    h_sll:  wr(s, TC_RD(w), s->x[TC_RS1(w)] << (s->x[TC_RS2L(w)] & 31));                  NEXT(1);
    h_slt:  wr(s, TC_RD(w), (int32_t)s->x[TC_RS1(w)] < (int32_t)s->x[TC_RS2L(w)]);        NEXT(1);
    h_sltu: wr(s, TC_RD(w), s->x[TC_RS1(w)] < s->x[TC_RS2L(w)]);                          NEXT(1);
    h_xor:  wr(s, TC_RD(w), s->x[TC_RS1(w)] ^ s->x[TC_RS2L(w)]);                          NEXT(1);
    h_srl:  wr(s, TC_RD(w), s->x[TC_RS1(w)] >> (s->x[TC_RS2L(w)] & 31));                  NEXT(1);
    h_sra:  wr(s, TC_RD(w), (uint32_t)((int32_t)s->x[TC_RS1(w)] >> (s->x[TC_RS2L(w)] & 31))); NEXT(1);
    h_or:   wr(s, TC_RD(w), s->x[TC_RS1(w)] | s->x[TC_RS2L(w)]);                          NEXT(1);
    h_and:  wr(s, TC_RD(w), s->x[TC_RS1(w)] & s->x[TC_RS2L(w)]);                          NEXT(1);

    h_addi:  wr(s, TC_RD(w), s->x[TC_RS1(w)] + TC_IMM(w));                                NEXT(1);
    h_slli:  wr(s, TC_RD(w), s->x[TC_RS1(w)] << (TC_IMM(w) & 31));                        NEXT(1);
    h_slti:  wr(s, TC_RD(w), (int32_t)s->x[TC_RS1(w)] < TC_IMM(w));                       NEXT(1);
    h_sltiu: wr(s, TC_RD(w), s->x[TC_RS1(w)] < (uint32_t)TC_IMM(w));                      NEXT(1);
    h_xori:  wr(s, TC_RD(w), s->x[TC_RS1(w)] ^ (uint32_t)TC_IMM(w));                      NEXT(1);
    h_srli:  wr(s, TC_RD(w), s->x[TC_RS1(w)] >> (TC_IMM(w) & 31));                        NEXT(1);
    h_ori:   wr(s, TC_RD(w), s->x[TC_RS1(w)] | (uint32_t)TC_IMM(w));                      NEXT(1);
    h_andi:  wr(s, TC_RD(w), s->x[TC_RS1(w)] & (uint32_t)TC_IMM(w));                      NEXT(1);
    h_srai:  wr(s, TC_RD(w), (uint32_t)((int32_t)s->x[TC_RS1(w)] >> (TC_IMM(w) & 31)));   NEXT(1);

    h_mul:    wr(s, TC_RD(w), s->x[TC_RS1(w)] * s->x[TC_RS2L(w)]); NEXT(1);
    h_mulh:   a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)]; wr(s, TC_RD(w), (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32)); NEXT(1);
    h_mulhsu: a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)]; wr(s, TC_RD(w), (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32)); NEXT(1);
    h_mulhu:  a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)]; wr(s, TC_RD(w), (uint32_t)(((uint64_t)a * b) >> 32)); NEXT(1);
    h_div:    a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)];
              wr(s, TC_RD(w), b == 0 ? 0xffffffffu : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b)); NEXT(1);
    h_divu:   a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)]; wr(s, TC_RD(w), b == 0 ? 0xffffffffu : a / b); NEXT(1);
    h_rem:    a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)];
              wr(s, TC_RD(w), b == 0 ? a : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b)); NEXT(1);
    h_remu:   a = s->x[TC_RS1(w)]; b = s->x[TC_RS2L(w)]; wr(s, TC_RD(w), b == 0 ? a : a % b); NEXT(1);

    h_lb: { uint32_t rd = TC_RD(w), ad = s->x[TC_RS1(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 1, 0);
                wr(s, rd, q ? (uint32_t)(int32_t)(int8_t)q[0]
                            : (uint32_t)sext(s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0, 8)); } }
          NEXT(1);
    h_lh: { uint32_t rd = TC_RD(w), ad = s->x[TC_RS1(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 2, 0);
                wr(s, rd, q ? (uint32_t)(int32_t)(int16_t)(q[0] | q[1] << 8)
                            : (uint32_t)sext(s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0, 16)); } }
          NEXT(1);
    h_lw: { uint32_t rd = TC_RD(w), ad = s->x[TC_RS1(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 4, 0);
                wr(s, rd, q ? ((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24)
                            : (s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0)); } }
          NEXT(1);
    h_lbu: { uint32_t rd = TC_RD(w), ad = s->x[TC_RS1(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 1, 0);
                wr(s, rd, q ? q[0] : ((s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0) & 0xff)); } }
          NEXT(1);
    h_lhu: { uint32_t rd = TC_RD(w), ad = s->x[TC_RS1(w)] + TC_IMM(w);
            if (rd) { uint8_t *q = mem_xlate(s, ad, 2, 0);
                wr(s, rd, q ? ((uint32_t)q[0] | (uint32_t)q[1] << 8)
                            : ((s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0) & 0xffff)); } }
          NEXT(1);

    h_sb: { uint32_t ad = s->x[TC_RS1(w)] + TC_IMM(w); b = s->x[TC_RS2H(w)]; uint8_t *q = mem_xlate(s, ad, 1, 1);
            if (q) q[0] = (uint8_t)b; else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT(1);
    h_sh: { uint32_t ad = s->x[TC_RS1(w)] + TC_IMM(w); b = s->x[TC_RS2H(w)]; uint8_t *q = mem_xlate(s, ad, 2, 1);
            if (q) { q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8); } else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT(1);
    h_sw: { uint32_t ad = s->x[TC_RS1(w)] + TC_IMM(w); b = s->x[TC_RS2H(w)]; uint8_t *q = mem_xlate(s, ad, 4, 1);
            if (q) { q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8); q[2] = (uint8_t)(b >> 16); q[3] = (uint8_t)(b >> 24); }
            else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT(1);

    h_beq:  a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR(a == b);
    h_bne:  a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR(a != b);
    h_blt:  a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR((int32_t)a <  (int32_t)b);
    h_bge:  a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR((int32_t)a >= (int32_t)b);
    h_bltu: a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR(a <  b);
    h_bgeu: a = s->x[TC_RS1(w)]; b = s->x[TC_RS2H(w)]; BR(a >= b);

    h_jal:  { uint32_t tgt = p[1]; wr(s, TC_RD(w), pc + 4); JUMP(tgt); }
    h_jalr: { uint32_t t = (s->x[TC_RS1(w)] + TC_IMM(w)) & ~1u; wr(s, TC_RD(w), pc + 4); JUMP(t); }

    h_lui:  wr(s, TC_RD(w), p[1]); NEXT(2);
    h_nop:  NEXT(1);

    h_trap: {
        uint8_t op = TC_OP(w);
        s->pc = pc; s->inst = p[1]; k++;
        int stop = (op == RISCV_OP_ECALL)  ? (s->ecall   ? s->ecall(s)   : 0)
                 : (op == RISCV_OP_EBREAK) ? (s->ebreak  ? s->ebreak(s)  : 1)
                 :                           (s->illegal ? s->illegal(s) : 1);
        if (stop) { s->pc = pc; return k; }
        pc += 4;                                   /* serviced syscall: resume after it */
        if (k >= max || pc >= clen || map[pc >> 1] == TC_SENTINEL) { s->pc = pc; return k; }
        p = base + map[pc >> 1]; w = *p; goto *tbl[TC_OP(w)];
    }
    #undef NEXT
    #undef JUMP
    #undef BR
}

/* ------------------------------------------------------------------ init */

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
