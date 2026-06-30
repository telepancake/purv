/*
 * transcode.c - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter.
 *
 * Standalone: it decodes raw instructions and emits one packed op word each. It
 * touches no engine state -- no registers, no memory, no symbols. One pass: the
 * output is 1:1 with the instruction stream (op for pc is ops[pc>>2]), so there is
 * nothing to place or back-patch. See transcode.h for the word layout.
 */
#include <stdlib.h>

#include "transcode.h"

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

/* Lower the instruction at code[off] into one packed op word. `disp`/`imm`/`uimm`
 * payloads are relative or value-only (never an absolute address): the evaluator has
 * the live pc, so branch/jal carry a displacement and auipc carries just the upper
 * immediate. An unhandled or reserved encoding becomes ILLEGAL (op only). */
static uint32_t encode(const uint8_t *code, uint32_t off) {
    uint32_t w = (uint32_t)code[off] | code[off+1] << 8 | code[off+2] << 16 | (uint32_t)code[off+3] << 24;
    uint32_t rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31;
    uint32_t f3 = (w >> 12) & 7, f7 = w >> 25;
    uint8_t op;
    switch (w & 0x7f) {
    case OPIMM:
        if (f3 == 1) op = RISCV_OP_SLLI, rs2 = 0;
        else if (f3 == 5) op = (f7 == 0x20) ? RISCV_OP_SRAI : RISCV_OP_SRLI, rs2 = 0;
        else op = RISCV_OP_ADDI + f3;
        if (f3 == 1 || f3 == 5) return (uint32_t)op << 26 | rd << 21 | rs1 << 16 | ((w >> 20) & 31);
        return (uint32_t)op << 26 | rd << 21 | rs1 << 16 | ((uint32_t)((int32_t)w >> 20) & 0xffff);
    case OP:
        if (f7 == 1) op = RISCV_OP_MUL + f3;
        else if (f7 == 0x20) op = (f3 == 0) ? RISCV_OP_SUB : RISCV_OP_SRA;
        else op = RISCV_OP_ADD + f3;
        return (uint32_t)op << 26 | rd << 21 | rs1 << 16 | rs2 << 11;
    case LOAD:
        op = load_op(f3);
        return (uint32_t)op << 26 | rd << 21 | rs1 << 16 | ((uint32_t)((int32_t)w >> 20) & 0xffff);
    case STORE: {
        op = store_op(f3);
        uint32_t imm = (uint32_t)(((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f));
        return (uint32_t)op << 26 | rs2 << 21 | rs1 << 16 | (imm & 0xffff);
    }
    case BRANCH: {
        op = branch_op(f3);
        int32_t disp = sext((w >> 31 & 1) << 12 | (w >> 7 & 1) << 11 |
                            (w >> 25 & 0x3f) << 5 | (w >> 8 & 0xf) << 1, 13);
        return (uint32_t)op << 26 | rs1 << 21 | rs2 << 16 | ((uint32_t)disp & 0xffff);
    }
    case JAL: {
        int32_t disp = sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |
                            (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21);
        return (uint32_t)RISCV_OP_JAL << 26 | rd << 21 | ((uint32_t)disp & 0x1fffff);
    }
    case JALR:
        return (uint32_t)RISCV_OP_JALR << 26 | rd << 21 | rs1 << 16 | ((uint32_t)((int32_t)w >> 20) & 0xffff);
    case LUI:
        return (uint32_t)RISCV_OP_LUI   << 26 | rd << 21 | ((w >> 12) & 0xfffff);
    case AUIPC:
        return (uint32_t)RISCV_OP_AUIPC << 26 | rd << 21 | ((w >> 12) & 0xfffff);
    case MISCMEM:
        return (uint32_t)((rd || rs1 || (f3 != 0 && f3 != 1)) ? RISCV_OP_ILLEGAL : RISCV_OP_NOP) << 26;
    case SYSTEM: {
        uint32_t imm = (w >> 20) & 0xfff;
        if (f3 == 0 && rd == 0 && rs1 == 0 && imm == 0x000) op = RISCV_OP_ECALL;
        else if (f3 == 0 && rd == 0 && rs1 == 0 && imm == 0x001) op = RISCV_OP_EBREAK;
        else op = RISCV_OP_ILLEGAL;
        return (uint32_t)op << 26;
    }
    default:
        return (uint32_t)RISCV_OP_ILLEGAL << 26;
    }
}

void transcode(const uint8_t *code, uint32_t len, Transcoded *out) {
    uint32_t n = len / 4;
    out->n_ops = n;
    out->code_len = n * 4;
    out->ops = malloc(((size_t)n + 1) * sizeof *out->ops);
    for (uint32_t i = 0; i < n; i++) out->ops[i] = encode(code, i * 4);
}
