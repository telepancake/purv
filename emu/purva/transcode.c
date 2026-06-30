/*
 * transcode.c - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter.
 *
 * Standalone: it decodes raw instructions and emits packed op words; it touches no
 * engine state. Two passes (measure, then emit) so the output can be sized exactly
 * and every branch/jal target maps to a placed op. See transcode.h for the layout.
 */
#include <stdlib.h>

#include "transcode.h"

/* Base opcodes (instruction[6:0]) -- RV32IM subset, no compressed. */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

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

/* One decoded instruction: our leaf op, the register indices, and `pay` -- the
 * per-op payload the emitter needs. For reg-imm/load/store/jalr it is the immediate;
 * for branch/jal the ABSOLUTE target pc (off + offset); for lui/auipc the value to
 * write (auipc folded to its absolute value); for the traps the raw word. */
typedef struct { uint8_t op, rd, rs1, rs2; uint32_t pay; } Dec;

static void decode(const uint8_t *code, uint32_t off, Dec *d) {
    uint32_t w = (uint32_t)code[off] | code[off+1] << 8 | code[off+2] << 16 | (uint32_t)code[off+3] << 24;
    uint32_t f3 = (w >> 12) & 7, f7 = w >> 25;
    d->rd = (w >> 7) & 31; d->rs1 = (w >> 15) & 31; d->rs2 = (w >> 20) & 31; d->pay = 0;
    switch (w & 0x7f) {
    case OPIMM:
        if (f3 == 1) { d->op = RISCV_OP_SLLI; d->pay = (w >> 20) & 31; }
        else if (f3 == 5) { d->op = (f7 == 0x20) ? RISCV_OP_SRAI : RISCV_OP_SRLI; d->pay = (w >> 20) & 31; }
        else { d->op = RISCV_OP_ADDI + f3; d->pay = (uint32_t)((int32_t)w >> 20); }
        break;
    case OP:
        if (f7 == 1) d->op = RISCV_OP_MUL + f3;
        else if (f7 == 0x20) d->op = (f3 == 0) ? RISCV_OP_SUB : RISCV_OP_SRA;
        else d->op = RISCV_OP_ADD + f3;
        break;
    case LOAD:  d->op = load_op(f3);  d->pay = (uint32_t)((int32_t)w >> 20); break;
    case STORE: d->op = store_op(f3); d->pay = (uint32_t)(((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f)); break;
    case BRANCH:
        d->op = branch_op(f3);
        d->pay = off + sext((w >> 31 & 1) << 12 | (w >> 7 & 1) << 11 |
                            (w >> 25 & 0x3f) << 5 | (w >> 8 & 0xf) << 1, 13);   /* target pc */
        break;
    case JAL:
        d->op = RISCV_OP_JAL;
        d->pay = off + sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |
                            (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21);  /* target pc */
        break;
    case JALR:  d->op = RISCV_OP_JALR; d->pay = (uint32_t)((int32_t)w >> 20); break;
    case LUI:   d->op = RISCV_OP_LUI;  d->pay = w & 0xfffff000; break;
    case AUIPC: d->op = RISCV_OP_LUI;  d->pay = off + (w & 0xfffff000); break;    /* fold to LUI */
    case MISCMEM:
        d->op = (d->rd || d->rs1 || (f3 != 0 && f3 != 1)) ? RISCV_OP_ILLEGAL : RISCV_OP_NOP;
        if (d->op == RISCV_OP_ILLEGAL) d->pay = w;
        break;
    case SYSTEM: {
        uint32_t imm = (w >> 20) & 0xfff;
        if (f3 == 0 && d->rd == 0 && d->rs1 == 0 && imm == 0x000) d->op = RISCV_OP_ECALL;
        else if (f3 == 0 && d->rd == 0 && d->rs1 == 0 && imm == 0x001) d->op = RISCV_OP_EBREAK;
        else d->op = RISCV_OP_ILLEGAL;
        d->pay = w;
        break;
    }
    default: d->op = RISCV_OP_ILLEGAL; d->pay = w; break;
    }
}

/* Output words this op occupies. */
static int rec_words(uint8_t op) {
    if (op >= RISCV_OP_BEQ   && op <= RISCV_OP_BGEU)    return 2;   /* + target pc */
    if (op == RISCV_OP_JAL)                             return 2;   /* + target pc */
    if (op == RISCV_OP_LUI)                             return 2;   /* + value     */
    if (op >= RISCV_OP_ECALL && op <= RISCV_OP_ILLEGAL) return 2;   /* + raw word   */
    return 1;
}

/* Pack one decoded instruction into out[at..]; return the new cursor. */
static uint32_t emit(uint32_t *out, uint32_t at, const Dec *d) {
    uint8_t op = d->op;
    uint32_t w0 = (uint32_t)op << 26, rd = d->rd, rs1 = d->rs1, rs2 = d->rs2;
    if (op <= RISCV_OP_SRA || (op >= RISCV_OP_MUL && op <= RISCV_OP_REMU))
        out[at++] = w0 | rd << 21 | rs1 << 16 | rs2;                /* reg-reg       */
    else if (op >= RISCV_OP_ADDI && op <= RISCV_OP_SRAI)
        out[at++] = w0 | rd << 21 | rs1 << 16 | (d->pay & 0xffff);  /* reg-imm/shift */
    else if (op >= RISCV_OP_LB && op <= RISCV_OP_LHU)
        out[at++] = w0 | rd << 21 | rs1 << 16 | (d->pay & 0xffff);  /* load          */
    else if (op >= RISCV_OP_SB && op <= RISCV_OP_SW)
        out[at++] = w0 | rs2 << 21 | rs1 << 16 | (d->pay & 0xffff); /* store         */
    else if (op >= RISCV_OP_BEQ && op <= RISCV_OP_BGEU) {
        out[at++] = w0 | rs2 << 21 | rs1 << 16; out[at++] = d->pay; /* + target pc   */
    } else if (op == RISCV_OP_JAL) {
        out[at++] = w0 | rd << 21;              out[at++] = d->pay; /* + target pc   */
    } else if (op == RISCV_OP_JALR) {
        out[at++] = w0 | rd << 21 | rs1 << 16 | (d->pay & 0xffff);  /* indirect      */
    } else if (op == RISCV_OP_LUI) {
        out[at++] = w0 | rd << 21;              out[at++] = d->pay; /* + value       */
    } else if (op == RISCV_OP_NOP) {
        out[at++] = w0;
    } else {                                                       /* ecall/ebreak/illegal */
        out[at++] = w0;                         out[at++] = d->pay; /* + raw word    */
    }
    return at;
}

void transcode(const uint8_t *code, uint32_t len, Transcoded *out) {
    out->code_len = len;
    size_t mapn = (size_t)(len >> 1) + 2;
    out->map = malloc(mapn * sizeof *out->map);
    for (size_t i = 0; i < mapn; i++) out->map[i] = TC_SENTINEL;

    Dec d;
    uint32_t words = 0;
    for (uint32_t off = 0; off + 4 <= len; off += 4) {          /* pass 1: place + size */
        decode(code, off, &d);
        out->map[off >> 1] = words;
        words += rec_words(d.op);
    }
    out->n_ops = words;
    out->ops = malloc(((size_t)words + 4) * sizeof *out->ops);
    uint32_t at = 0;
    for (uint32_t off = 0; off + 4 <= len; off += 4) {          /* pass 2: emit          */
        decode(code, off, &d);
        at = emit(out->ops, at, &d);
    }
}

void transcode_free(Transcoded *out) {
    free(out->ops); free(out->map);
    out->ops = out->map = NULL; out->n_ops = out->code_len = 0;
}
