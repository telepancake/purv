/*
 * transcode.c - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter.
 *
 * Two passes:
 *
 *   pass 1  walk the instructions; record each one's op-word offset in
 *           map[orig_pc>>1], and total the output size.
 *   pass 2  emit each op, resolving every branch/jal target's original pc through
 *           that map to a baked op-relative displacement.
 *
 * For RV32IM today, op layout IS 1:1 with the instruction stream (one word per
 * 4-byte instruction, op-word offset == pc/4 everywhere), so the map is currently
 * the identity and pass 2's resolution is a formality -- but it's the real
 * mechanism, not a shortcut, because op-index==pc/4 is NOT safe to assume in
 * general: see op_words()'s note on why auipc stays unspilled, which is the
 * concrete reason this can't be "ops[pc>>2]" by convention. The C extension
 * (variable 2/4-byte instructions) and any compacting op fusion both need a real
 * pass-1 layout + pass-2 resolution, which is what this already is.
 *
 * Direct branch/jal targets resolve at transcode time (baked, no runtime map).
 * jalr does not: its target is data-dependent, so it can only be resolved at run
 * time, and ONLY works because op-index==pc/4 holds everywhere right now (see
 * op_words()). That equality is the thing that must not be broken without also
 * giving jalr a real address resolution -- which RV32IM does not need, but C-ext
 * and fusion will.
 */
#include <stdlib.h>

#include "transcode.h"

enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

#define TC_SENTINEL 0xffffffffu          /* map entry for a non-instruction offset */

static int32_t sext(uint32_t v, int bits) { int sh = 32 - bits; return (int32_t)(v << sh) >> sh; }

static uint8_t load_op(uint32_t f3) {
    switch (f3) {
    case 0: return RISCV_OP_LB;  case 1: return RISCV_OP_LH; case 2: return RISCV_OP_LW;
    case 4: return RISCV_OP_LBU; case 5: return RISCV_OP_LHU; default: return RISCV_OP_ILLEGAL;
    }
}
static uint8_t store_op(uint32_t f3) {
    switch (f3) {
    case 0: return RISCV_OP_SB; case 1: return RISCV_OP_SH; case 2: return RISCV_OP_SW;
    default: return RISCV_OP_ILLEGAL;
    }
}
static uint8_t branch_op(uint32_t f3) {
    switch (f3) {
    case 0: return RISCV_OP_BEQ;  case 1: return RISCV_OP_BNE;
    case 4: return RISCV_OP_BLT;  case 5: return RISCV_OP_BGE;
    case 6: return RISCV_OP_BLTU; case 7: return RISCV_OP_BGEU; default: return RISCV_OP_ILLEGAL;
    }
}

/* One decoded instruction. `imm` is the immediate for the ops that keep one inline;
 * `target` is an ABSOLUTE original pc for branch/jal, or the materialized value for
 * auipc/lui. `width` is the original instruction width (2 or 4). */
typedef struct { uint8_t op, rd, rs1, rs2; uint32_t imm, target; uint8_t width; } Dec;

static void decode(const uint8_t *code, uint32_t off, Dec *d) {
    uint32_t w = (uint32_t)code[off] | code[off+1] << 8 | code[off+2] << 16 | (uint32_t)code[off+3] << 24;
    uint32_t f3 = (w >> 12) & 7, f7 = w >> 25;
    d->width = 4;
    d->rd = (w >> 7) & 31; d->rs1 = (w >> 15) & 31; d->rs2 = (w >> 20) & 31;
    d->imm = 0; d->target = 0;
    switch (w & 0x7f) {
    case OPIMM:
        if (f3 == 1) { d->op = RISCV_OP_SLLI; d->imm = (w >> 20) & 31; }
        else if (f3 == 5) { d->op = (f7 == 0x20) ? RISCV_OP_SRAI : RISCV_OP_SRLI; d->imm = (w >> 20) & 31; }
        else { d->op = RISCV_OP_ADDI + f3; d->imm = (uint32_t)((int32_t)w >> 20); }
        break;
    case OP:
        if (f7 == 1) d->op = RISCV_OP_MUL + f3;
        else if (f7 == 0x20) d->op = (f3 == 0) ? RISCV_OP_SUB : RISCV_OP_SRA;
        else d->op = RISCV_OP_ADD + f3;
        break;
    case LOAD:  d->op = load_op(f3);  d->imm = (uint32_t)((int32_t)w >> 20); break;
    case STORE: d->op = store_op(f3); d->imm = (uint32_t)(((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f)); break;
    case BRANCH:
        d->op = branch_op(f3);
        d->target = off + sext((w >> 31 & 1) << 12 | (w >> 7 & 1) << 11 |
                               (w >> 25 & 0x3f) << 5 | (w >> 8 & 0xf) << 1, 13);
        break;
    case JAL:
        d->op = RISCV_OP_JAL;
        d->target = off + sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |
                               (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21);
        break;
    case JALR:  d->op = RISCV_OP_JALR; d->imm = (uint32_t)((int32_t)w >> 20); break;
    case LUI:   d->op = RISCV_OP_LUI;  d->imm = (w >> 12) & 0xfffff; break;
    case AUIPC: d->op = RISCV_OP_AUIPC; d->imm = (w >> 12) & 0xfffff; break;
    case MISCMEM:
        d->op = (d->rd || d->rs1 || (f3 != 0 && f3 != 1)) ? RISCV_OP_ILLEGAL : RISCV_OP_NOP;
        break;
    case SYSTEM: {
        uint32_t imm = (w >> 20) & 0xfff;
        if (f3 == 0 && d->rd == 0 && d->rs1 == 0 && imm == 0x000) d->op = RISCV_OP_ECALL;
        else if (f3 == 0 && d->rd == 0 && d->rs1 == 0 && imm == 0x001) d->op = RISCV_OP_EBREAK;
        else d->op = RISCV_OP_ILLEGAL;
        break;
    }
    default: d->op = RISCV_OP_ILLEGAL; break;
    }
}

/* Output op words for an op. One per instruction for RV32IM: auipc stays a single
 * word (its value is computed at run time from the live cursor, which equals the
 * original pc exactly as long as nothing spills -- see purva.c's h_auipc). Baking
 * auipc's value here would spill a second word, which breaks jalr: a jalr target
 * computed via auipc+arith (the standard PIC indirect-jump idiom) is an ORIGINAL
 * address, but the evaluator resolves jalr by treating its target as a TRANSCODED
 * op-word offset (ops[t>>2]) -- correct only when offset==pc/4 holds everywhere.
 * Confirmed by reproduction: baking auipc made riscv-arch-test I/jal-01 (which
 * contains exactly this `auipc a3,0; addi a3,a3,N; jr a3` idiom) jump into the
 * middle of an unrelated nop run and spin forever. So nothing here may diverge
 * op-index from pc/4 without also giving jalr a real address->op-index resolution,
 * which RV32IM does not need (no spilling) but the C extension and any fusion that
 * compacts the stream will. */
static int op_words(uint8_t op) { (void)op; return 1; }

/* Resolve a branch/jal target's original pc to its op-word offset. A target outside
 * the code window or not at an instruction start (only possible while sweeping the
 * rodata that rides in the code image -- those ops never execute) yields `at`, i.e.
 * a zero displacement. */
static uint32_t target_word(const uint32_t *map, uint32_t len, uint32_t target, uint32_t at) {
    if (target >= len) return at;
    uint32_t tw = map[target >> 1];
    return tw == TC_SENTINEL ? at : tw;
}

/* Emit one op at op-word offset `at`; resolve branch/jal targets through `map`
 * (orig_pc>>1 -> op-word offset) to a baked op-relative displacement (scaled x4, so
 * the evaluator's >>2 recovers the op delta). Returns the next op-word offset. */
static uint32_t emit(uint32_t *ops, uint32_t at, const Dec *d, const uint32_t *map, uint32_t len) {
    uint8_t op = d->op;
    uint32_t w0 = (uint32_t)op << 26, rd = d->rd, rs1 = d->rs1, rs2 = d->rs2;
    if (op <= RISCV_OP_SRA || (op >= RISCV_OP_MUL && op <= RISCV_OP_REMU))
        ops[at++] = w0 | rd << 21 | rs1 << 16 | rs2 << 11;         /* reg-reg       */
    else if (op >= RISCV_OP_ADDI && op <= RISCV_OP_SRAI)
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffff);  /* reg-imm/shift */
    else if (op >= RISCV_OP_LB && op <= RISCV_OP_LHU)
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffff);  /* load          */
    else if (op >= RISCV_OP_SB && op <= RISCV_OP_SW)
        ops[at++] = w0 | rs2 << 21 | rs1 << 16 | (d->imm & 0xffff); /* store         */
    else if (op >= RISCV_OP_BEQ && op <= RISCV_OP_BGEU) {
        int32_t delta = (int32_t)target_word(map, len, d->target, at) - (int32_t)at;
        ops[at++] = w0 | rs1 << 21 | rs2 << 16 | ((uint32_t)(delta * 4) & 0xffff);
    } else if (op == RISCV_OP_JAL) {
        int32_t delta = (int32_t)target_word(map, len, d->target, at) - (int32_t)at;
        ops[at++] = w0 | rd << 21 | ((uint32_t)(delta * 4) & 0x1fffff);
    } else if (op == RISCV_OP_JALR) {
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffff);
    } else if (op == RISCV_OP_LUI || op == RISCV_OP_AUIPC) {
        ops[at++] = w0 | rd << 21 | (d->imm & 0xfffff);            /* lui value / auipc upper */
    } else {                                                       /* nop / traps   */
        ops[at++] = w0;
    }
    return at;
}

void transcode(const uint8_t *code, uint32_t len, Transcoded *out) {
    size_t mapn = (size_t)(len >> 1) + 2;
    uint32_t *map = malloc(mapn * sizeof *map);
    for (size_t i = 0; i < mapn; i++) map[i] = TC_SENTINEL;
    Dec d;

    uint32_t at = 0;                                       /* pass 1: place + size */
    for (uint32_t off = 0; off + 2 <= len; ) {
        uint16_t lo = code[off] | code[off+1] << 8;
        uint32_t wd = ((lo & 3) == 3) ? 4 : 2;
        if (off + wd > len) break;
        decode(code, off, &d);
        map[off >> 1] = at;
        at += op_words(d.op);
        off += d.width;
    }
    out->n_ops = at;
    out->code_len = at * 4;
    out->ops = malloc(((size_t)at + 2) * sizeof *out->ops);

    at = 0;                                                /* pass 2: emit          */
    for (uint32_t off = 0; off + 2 <= len; ) {
        uint16_t lo = code[off] | code[off+1] << 8;
        uint32_t wd = ((lo & 3) == 3) ? 4 : 2;
        if (off + wd > len) break;
        decode(code, off, &d);
        at = emit(out->ops, at, &d, map, len);
        off += d.width;
    }
    free(map);
}
