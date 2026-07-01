/*
 * transcode.h - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter (interface).
 *
 * Lowers each instruction into a packed 32-bit op word. For plain RV32IM that is
 * 1:1 with the instruction stream (op-index == pc/4 everywhere), but it is NOT a
 * general guarantee: SPILL2 (below) fuses a safe pair of adjacent stores into ONE
 * op word covering 8 bytes of original code, so op-index can run ahead of pc/4 once
 * a fusion has happened earlier in the stream. Every jump target is still resolved
 * correctly because of how it's computed -- see SPILL2's contract -- but "ops[pc>>2]
 * is always right" is no longer literally true; only the transcode-time map and
 * baked op-relative displacements are.
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
 *   spill2 (fused)   op[31:26] base[25:21] v1[20:16] v2[15:11] off[10:0]
 *
 * (auipc is its own op -- it needs the live pc, which the evaluator has -- so unlike
 * a baked absolute it fits one word. lui keeps only the 20-bit upper immediate.)
 *
 * SPILL2: a peephole fusion over two adjacent `sw` to the same base register at
 * consecutive word offsets (off, off+4) -- the spill/restore and struct-copy idiom,
 * ~8.6% of static instructions in a real corpus (see the conversation this came
 * from). One op does both stores; the evaluator counts it as 2 instructions and
 * advances its cursor by 1 op word instead of 2, so it is strictly fewer dispatches
 * and one merged 8-byte memory access instead of two 4-byte ones.
 *
 * Stores write no register, so there is no data hazard between the pair -- the only
 * thing fusion can break is CONTROL FLOW: hiding the SECOND instruction's op slot
 * is only safe if nothing can ever land there. Sequential scanning makes this
 * provable, not assumed: a candidate pair's FIRST instruction always keeps its own
 * op slot (fusion only ever hides the second), and an `sw` can never itself be a
 * jal/jalr/branch, so the instruction immediately after any jal/jalr/branch is
 * always visited fresh by the scan and can only ever be a kept first-half or a
 * standalone -- meaning every RETURN ADDRESS (computed purely from the call site's
 * own op-index, not from pc) is automatically safe with no bookkeeping. The
 * remaining risk is a position becoming unreachable as a DIRECT branch/jal target,
 * or as an INDIRECT (jalr) target reached via a code address taken somewhere in the
 * program (a function pointer, a vtable slot -- exactly the addresses --emit-relocs
 * surfaces in tctool.c, the same mechanism that patches them). transcode_ex's
 * `ext` parameter lets a caller that has that information (tctool.c does; a bare
 * transcode() call does not, and so fuses nothing across an unknown external
 * target) supply it; the transcoder unions it with its own direct-target scan and
 * refuses to fuse across anything in the result.
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
    RISCV_OP_SPILL2,        /* fused store-pair (peephole-only; decode() never emits it directly) */
    RISCV_OP_AUIPC_ABS,     /* auipc with a baked absolute value (see below; also peephole-only) */
    RISCV_OP_COUNT
};

/* RISCV_OP_AUIPC's runtime value (cursor*4 + uimm20<<12) is only correct as long as
 * op-index == pc/4 EVERYWHERE BEFORE this instruction -- the same invariant SPILL2
 * fusion deliberately breaks once it's fired earlier in the stream (fusion runs the
 * op-index 1 behind pc/4 for everything downstream of it). transcode_ex tracks
 * whether that drift has actually started by the time it reaches an auipc; if so,
 * it bakes the absolute value (computed from `off`, which the transcoder has and
 * the evaluator's cursor no longer reliably tracks) into a second word as
 * RISCV_OP_AUIPC_ABS rather than emitting the ordinary 1-word cursor-based
 * RISCV_OP_AUIPC.
 *
 * That baked value is safe unmodified for a data (rodata/rwdata) target because
 * neither region ever moves relative to code: they're separate regions (purv.h's
 * documented four-region layout -- code at [0, code.len), rodata growing down
 * from RISCV_HALF; see purva.ld and purva.c's mem_xlate), anchored to their own
 * fixed architectural boundaries rather than to "wherever code happens to end",
 * so fusion changing code's size can't move them.
 *
 * A CODE target is different: two positions in code can shift relative to each
 * other as fusion changes what's between them, so a value computed against one
 * position (the auipc's) isn't valid at another (the target's) -- an auipc
 * materializing a function pointer to store as data (`obj->method = someFunc;`,
 * as opposed to a static initializer, which is a link-time relocation and
 * already handled by patch_rela) needs the actual pc-to-pc displacement between
 * the two, not its own local value. tctool.c's collect_pcrel_code_fixups /
 * apply_pcrel_code_fixups do that: find the auipc+addi (or +load/store) pair via
 * the PCREL_HI20/LO12 relocations --emit-relocs keeps, and once the map exists,
 * re-encode both instructions for the real displacement. */

/* Packed-op field accessors. A/B/C are the three register slots; which register each
 * holds depends on the class (see the layout above), so handlers pick by name.
 * SPILL2 reuses A/B/C as base/v1/v2 and adds its own 11-bit signed offset field. */
#define TC_OP(w)    ((uint32_t)(w) >> 26)
#define TC_A(w)     (((w) >> 21) & 31)   /* rd, or rs1 (branch), or rs2 (store), or SPILL2 base */
#define TC_B(w)     (((w) >> 16) & 31)   /* rs1, or rs2 (branch), or SPILL2 v1                  */
#define TC_C(w)     (((w) >> 11) & 31)   /* rs2 (reg-reg), or SPILL2 v2                         */
#define TC_IMM(w)   ((int32_t)(int16_t)(w))                       /* 16-bit signed */
#define TC_JOFF(w)  ((int32_t)(((w) & 0x1fffffu) << 11) >> 11)    /* 21-bit signed */
#define TC_UIMM(w)  ((w) & 0xfffffu)                              /* 20-bit upper  */
#define TC_OFF11(w) ((int32_t)(((w) & 0x7ffu) << 21) >> 21)       /* 11-bit signed (SPILL2) */

/* A transcoded program: just the op array and how far the code runs. ops[pc>>2] is
 * a SHORTCUT that only holds when nothing has fused -- see the header note above.
 * The evaluator never indexes that way; it always walks via its own cursor. */
typedef struct {
    uint32_t *ops;
    uint32_t  n_ops;       /* number of op words (NOT == code_len/4 once anything fuses) */
    uint32_t  code_len;    /* op-word extent in bytes (== n_ops*4)         */
} Transcoded;

/* A caller-supplied set of additional addresses that may be reached indirectly
 * (e.g. via jalr through a function pointer) -- code addresses found by relocations
 * the transcoder itself never sees (it reads only code bytes, never an ELF). NULL
 * means "none known"; the caller must supply every such address for fusion to stay
 * sound -- see SPILL2's contract above. */
typedef struct { const uint32_t *addr; uint32_t n; } TcExternalTargets;

/* Transcode RV32IM code [0, len) into *out (allocates out->ops), folding in `ext`
 * (may be NULL) as additional fusion-unsafe addresses. Reads only the code bytes.
 * Free out->ops yourself. */
void transcode_ex(const uint8_t *code, uint32_t len, const TcExternalTargets *ext, Transcoded *out);
void transcode(const uint8_t *code, uint32_t len, Transcoded *out);   /* = transcode_ex(..., NULL, ...) */

#define TC_SENTINEL 0xffffffffu     /* map entry for a non-instruction offset */

/* Build ONLY the pc>>1 -> op-word-offset map (pass 1; no op emission), and
 * report the resulting op count -- using the SAME fusion decisions a transcode_ex
 * call with the same (code, len, ext) would make, so the two stay consistent. For a
 * tool that needs to resolve a CODE ADDRESS to its op index outside the normal
 * transcode call -- e.g. patching a code pointer found by a relocation -- not
 * needed by transcode()/transcode_ex() callers that just run code. Caller frees the
 * returned array (sized (len>>1)+2). */
uint32_t *transcode_map_ex(const uint8_t *code, uint32_t len, const TcExternalTargets *ext, uint32_t *n_ops_out);
uint32_t *transcode_map(const uint8_t *code, uint32_t len, uint32_t *n_ops_out);  /* = ..._ex(..., NULL, ...) */

#endif /* TRANSCODE_H_ */
