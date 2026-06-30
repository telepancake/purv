/*
 * transcode.h - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter (interface).
 *
 * Pure code transformation: it lowers each 4-byte instruction into exactly ONE
 * packed 32-bit op word -- one to one with the instruction stream -- so the op for
 * guest pc is simply ops[pc>>2]. No side table, no memory image, no symbols: those
 * are the host's (upper-layer) job, exactly as for purv. The tool's whole output is
 * the op array.
 *
 * Packed op word: our leaf op in the top 6 bits (dispatch is w>>26, no mask), then
 * the operands. The immediate is a displacement/value, NOT an absolute address --
 * the evaluator derives pc from its op cursor, so jumps stay pc-relative and fit:
 *
 *   reg-reg          op[31:26] rd[25:21] rs1[20:16] rs2[15:11]
 *   reg-imm / load   op[31:26] rd[25:21] rs1[20:16] imm[15:0]
 *   store            op[31:26] rs2[25:21] rs1[20:16] imm[15:0]
 *   branch           op[31:26] rs1[25:21] rs2[20:16] disp[15:0]   (byte displacement)
 *   jal              op[31:26] rd[25:21]  disp[20:0]              (byte displacement)
 *   jalr             op[31:26] rd[25:21] rs1[20:16] imm[15:0]
 *   lui / auipc      op[31:26] rd[25:21]  uimm20[19:0]
 *   nop / ecall / ebreak / illegal   op[31:26]
 *
 * (auipc is its own op -- it needs the live pc, which the evaluator has -- so unlike
 * a baked absolute it fits one word. lui keeps only the 20-bit upper immediate.)
 */
#ifndef TRANSCODE_H_
#define TRANSCODE_H_

#include <stdint.h>

/* Our flat leaf ops -- one per evaluator handler. Order is fixed: the jump table and
 * the range tests in emit/eval depend on it. RV32IM only (no compressed). */
enum {
    RISCV_OP_ADD, RISCV_OP_SLL, RISCV_OP_SLT, RISCV_OP_SLTU,
    RISCV_OP_XOR, RISCV_OP_SRL, RISCV_OP_OR,  RISCV_OP_AND,
    RISCV_OP_SUB, RISCV_OP_SRA,
    RISCV_OP_ADDI, RISCV_OP_SLLI, RISCV_OP_SLTI, RISCV_OP_SLTIU,
    RISCV_OP_XORI, RISCV_OP_SRLI, RISCV_OP_ORI,  RISCV_OP_ANDI, RISCV_OP_SRAI,
    RISCV_OP_MUL, RISCV_OP_MULH, RISCV_OP_MULHSU, RISCV_OP_MULHU,
    RISCV_OP_DIV, RISCV_OP_DIVU, RISCV_OP_REM, RISCV_OP_REMU,
    RISCV_OP_LB, RISCV_OP_LH, RISCV_OP_LW, RISCV_OP_LBU, RISCV_OP_LHU,
    RISCV_OP_SB, RISCV_OP_SH, RISCV_OP_SW,
    RISCV_OP_BEQ, RISCV_OP_BNE, RISCV_OP_BLT, RISCV_OP_BGE, RISCV_OP_BLTU, RISCV_OP_BGEU,
    RISCV_OP_JAL, RISCV_OP_JALR,
    RISCV_OP_LUI, RISCV_OP_AUIPC, RISCV_OP_NOP,
    RISCV_OP_ECALL, RISCV_OP_EBREAK, RISCV_OP_ILLEGAL,
    RISCV_OP_COUNT
};

/* Packed-op field accessors. A/B/C are the three register slots; which register each
 * holds depends on the class (see the layout above), so handlers pick by name. */
#define TC_OP(w)    ((uint32_t)(w) >> 26)
#define TC_A(w)     (((w) >> 21) & 31)   /* rd, or rs1 (branch), or rs2 (store)   */
#define TC_B(w)     (((w) >> 16) & 31)   /* rs1, or rs2 (branch)                  */
#define TC_C(w)     (((w) >> 11) & 31)   /* rs2 (reg-reg)                         */
#define TC_IMM(w)   ((int32_t)(int16_t)(w))                       /* 16-bit signed */
#define TC_JOFF(w)  ((int32_t)(((w) & 0x1fffffu) << 11) >> 11)    /* 21-bit signed */
#define TC_UIMM(w)  ((w) & 0xfffffu)                              /* 20-bit upper  */

/* A transcoded program: just the op array (ops[pc>>2]) and how far the code runs. */
typedef struct {
    uint32_t *ops;
    uint32_t  n_ops;       /* == code_len/4                     */
    uint32_t  code_len;    /* image extent in bytes             */
} Transcoded;

/* Transcode RV32IM code [0, len) into *out (allocates out->ops). Reads only the
 * code bytes; produces one op word per 4-byte instruction. Free out->ops yourself. */
void transcode(const uint8_t *code, uint32_t len, Transcoded *out);

#define TC_SENTINEL 0xffffffffu     /* map entry for a non-instruction offset */

/* Build ONLY the orig_pc>>1 -> op-word-offset map (pass 1; no op emission), and
 * report the resulting op count. For a tool that needs to resolve a CODE ADDRESS to
 * its op index outside the normal transcode call -- e.g. patching a code pointer
 * found by a relocation -- not needed by transcode() callers that just run code.
 * Caller frees the returned array (sized (len>>1)+2). */
uint32_t *transcode_map(const uint8_t *code, uint32_t len, uint32_t *n_ops_out);

#endif /* TRANSCODE_H_ */
