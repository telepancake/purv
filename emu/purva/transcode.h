/*
 * transcode.h - ahead-of-time RISC-V -> "our ops" rewriter (interface).
 *
 * The transcoder is the decode pass lifted out of the run loop and run ONCE over a
 * whole RV32IM code image, instead of per straight-line run. It lowers each 4-byte
 * instruction into a packed 32-bit op word (a couple of ops spill one extra word),
 * and builds a pc->word-offset map so a control transfer can find its destination.
 * It is standalone: it reads code bytes and writes op words, and depends on nothing
 * but this header -- no engine state, no registers, no memory map.
 *
 * The packed op word: our leaf op in the top 6 bits (so the evaluator dispatches
 * with op = w >> 26, no mask), then the register indices and a 16-bit immediate:
 *
 *   reg-reg          op[31:26] rd[25:21] rs1[20:16] .......... rs2[4:0]
 *   reg-imm / load   op[31:26] rd[25:21] rs1[20:16] imm[15:0]
 *   store            op[31:26] rs2[25:21] rs1[20:16] imm[15:0]
 *   branch           op[31:26] rs2[25:21] rs1[20:16] ;  word1 = target pc
 *   jal              op[31:26] rd[25:21]            ;  word1 = target pc
 *   jalr             op[31:26] rd[25:21] rs1[20:16] imm[15:0]
 *   lui              op[31:26] rd[25:21]            ;  word1 = 32-bit value
 *   nop              op[31:26]
 *   ecall/ebreak/illegal  op[31:26]                ;  word1 = raw instruction word
 *
 * Branch/jal carry their absolute target pc, mapped to an op at run time; this keeps
 * the rewriter simple (a tighter build would bake the output offset and drop the
 * word). lui/trap spill because their payload exceeds 16 bits.
 */
#ifndef TRANSCODE_H_
#define TRANSCODE_H_

#include <stdint.h>

/* Our flat leaf ops -- one per evaluator handler. Decode folds the 32-bit forms,
 * funct3/funct7, and the operand source into this one byte (ADDI distinct from ADD).
 * The order is fixed: the evaluator's jump table and the imm/terminator range tests
 * depend on it. (RV32IM only -- no compressed; the transcoder asserts 4-byte steps.) */
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
    RISCV_OP_LUI, RISCV_OP_NOP,
    RISCV_OP_ECALL, RISCV_OP_EBREAK, RISCV_OP_ILLEGAL,
    RISCV_OP_COUNT
};

/* Packed-op field accessors (shared by the transcoder's emit and the evaluator). */
#define TC_OP(w)   ((uint32_t)(w) >> 26)
#define TC_RD(w)   (((w) >> 21) & 31)
#define TC_RS1(w)  (((w) >> 16) & 31)
#define TC_RS2L(w) ((w) & 31)            /* reg-reg: rs2 in the low 5 bits          */
#define TC_RS2H(w) (((w) >> 21) & 31)    /* store/branch: rs2 in the rd slot        */
#define TC_IMM(w)  ((int32_t)(int16_t)(w))   /* sign-extended 16-bit immediate      */

/* map entry for an address that is not an instruction start (or out of the image). */
#define TC_SENTINEL 0xffffffffu

/* A transcoded program: the packed op stream, and map[pc>>1] -> word offset in it
 * (TC_SENTINEL for a non-instruction pc). code_len is the image extent swept. */
typedef struct {
    uint32_t *ops;
    uint32_t  n_ops;
    uint32_t *map;
    uint32_t  code_len;
} Transcoded;

/* Transcode RV32IM code [0, len) into *out (allocates ops + map). Reads only the
 * code bytes. Free with transcode_free. */
void transcode(const uint8_t *code, uint32_t len, Transcoded *out);
void transcode_free(Transcoded *out);

#endif /* TRANSCODE_H_ */
