/*
 * transcode.h - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter (interface).
 *
 * Lowers each instruction into a packed 32-bit op word. For plain RV32IM that is
 * 1:1 with the instruction stream (op-index == pc/4 everywhere), but it is NOT a
 * general guarantee: PROLOGUE/EPILOGUE fusion (below) collapses a whole run of
 * instructions -- a frame setup or teardown -- into ONE op word, so op-index falls
 * behind pc/4 once a fusion has happened earlier in the stream. Every jump target is
 * still resolved correctly because of how it's computed -- see the fusion contract --
 * but "ops[pc>>2] is always right" is no longer literally true; only the
 * transcode-time map and baked op-relative displacements are.
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
 *   li_lo / li_hi    op[31:26] rd[25:21]  imm[20:0]              (21-bit signed value)
 *   nop / ecall / ebreak / illegal   op[31:26]
 *   prologue / epilogue (fused)      op[31:26] regmask[25:13] frame[12:0]
 *   shadd (fused)                    op[31:26] rd[25:21] rs1[20:16] rs2[15:11] sh[10:6]
 *   lwx (fused)                      op[31:26] rd[25:21] rs1[20:16] rs2[15:11] off[10:0]
 *   lwlw (fused)                     op[31:26] rd[25:21] rs1[20:16] o1[15:8] o2[7:0]
 *   lwjalr (fused)                   op[31:26] rd[25:21] rs1[20:16] off[15:0]
 *   load+beqz/bnez (fused, 2 words)  op[31:26] rd[25:21] rs1[20:16] off[15:0] | disp32
 *   lwsw (fused)                     op[31:26] rd[25:21] rs1[20:16] rs2[15:11] o1w[10:6] o2w[5:0]
 *
 * (auipc is its own op -- it computes from the evaluator's live op cursor, so it fits
 * one word; it survives only for CODE addresses, tctool re-encoding its uimm in op space.
 * A DATA address is a constant and becomes a li_lo/li_hi load-immediate instead. lui
 * keeps only the 20-bit upper immediate.)
 *
 * PROLOGUE/EPILOGUE fusion (see the encoding note below) collapses a function's frame
 * setup (`addi sp` + callee-saved stores) or teardown (callee-saved loads + `addi sp`
 * + `ret`) into ONE op word. It happens INSIDE the two passes (step(), shared by
 * build_map and emit), so the collapsed instructions are genuinely removed: the map
 * assigns the run a single op-word and the evaluator advances one op word for the
 * whole thing. Each fused op replays the exact instruction effects, so a match is
 * never wrong on semantics.
 *
 * The only thing fusion can break is CONTROL FLOW: collapsing a run is safe only if
 * nothing jumps into its interior. The run's HEAD keeps an op-word (the map points
 * the frame entry / the epilogue's shared return there), so branches and return
 * addresses that target the head still resolve. The interior is the risk -- a
 * position becoming a DIRECT branch/jal target, or an INDIRECT (jalr) target reached
 * via a code address taken somewhere in the program (a function pointer, a vtable
 * slot -- exactly the addresses --emit-relocs surfaces in tctool.c, the same
 * mechanism that patches them). transcode_ex's `ext` parameter lets a caller that has
 * that information (tctool.c does; a bare transcode() call does not, and so fuses
 * nothing across an unknown external target) supply it; the transcoder unions it with
 * its own direct-target scan and refuses to collapse across anything in the result.
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
    RISCV_OP_PROLOGUE,      /* fused frame-alloc + callee-saved register saves (peephole-only) */
    RISCV_OP_EPILOGUE,      /* fused callee-saved restores + frame-dealloc + ret (peephole-only) */
    RISCV_OP_LI_LO,         /* rd = sext21(imm)              -- data auipc/`la` near 0 (peephole-only) */
    RISCV_OP_LI_HI,         /* rd = sext21(imm) + RISCV_HALF -- data auipc/`la` near RISCV_HALF (peephole-only) */
    /* Pair fusions (peephole-only; see try_fuse_pair). Each replays BOTH
     * instructions' exact effects, so it is only matched when the second one's
     * register writes coincide with the first's (the intermediate is clobbered)
     * -- no liveness guessing. All chosen from measured pair frequency on real
     * C and C++ code (profile.py); each earns its evaluator handler. */
    RISCV_OP_SHADD,         /* slli T,X,k;  add T,B,T   ->  rd = rs1 + (rs2 << sh5)      */
    RISCV_OP_LWX,           /* add  T,a,b;  lw  T,o(T)  ->  rd = M32[rs1 + rs2 + off11]  */
    RISCV_OP_LWLW,          /* lw T,o1(a);  lw T,o2(T)  ->  rd = M32[M32[rs1+o1_8]+o2_8] */
    RISCV_OP_LWJALR,        /* lw T,o(a);   jalr ra,0(T) -> virtual call: rd=t, ra=link, jump t */
    RISCV_OP_LW_BEQZ,       /* lw  T,o(a);  beq T,x0    -- TWO op words (word2 = branch disp) */
    RISCV_OP_LW_BNEZ,       /* lw  T,o(a);  bne T,x0                                          */
    RISCV_OP_LBU_BEQZ,      /* lbu T,o(a);  beq T,x0                                          */
    RISCV_OP_LBU_BNEZ,      /* lbu T,o(a);  bne T,x0                                          */
    RISCV_OP_LWSW,          /* lw T,o1(a); sw T,o2(b) -> the word copy; o1/o2 word-scaled     */
    RISCV_OP_VCALL,         /* lw T,o1(a); lw T,o2(T); jalr ra,0(T) -> the vtable call
                             * (LWLW field layout; link register implied ra). THREE insns.    */
    RISCV_OP_MEMOP,         /* guest custom-0 (.insn r 0x0b): one mem/str libc call as ONE
                             * instruction (the sqlite rt.c bodies). funct3 rides in the low
                             * 3 bits and selects the routine; ../purvmemop.h is the whole
                             * story (encoding, semantics, fuel). This takes the LAST opcode
                             * slot: the dispatch table is now full. */
    RISCV_OP_COUNT
};

/* PROLOGUE / EPILOGUE encoding:  op[31:26] | regmask[25:13] | frame[12:0]
 *
 * A compiler prologue is a frame allocation followed by the callee-saved registers
 * stored at the TOP of the new frame, and the epilogue is its mirror:
 *     addi sp,sp,-N   |   lw   ra,N-4(sp)          (ra highest, then s0,s1,s2..)
 *     sw   ra,N-4(sp) |   lw   s0,N-8(sp)
 *     sw   s0,N-8(sp) |   ...
 *     ...             |   addi sp,sp,N
 *                     |   jalr zero,0(ra)          (ret)
 * The saves/restores always occupy contiguous 4-byte slots from N-4 downward in the
 * canonical order ra, s0, s1, s2..s11, so each register's offset is fully determined
 * by (frame, its position among the set registers) -- no offset list is stored.
 *
 * regmask: 13 bits, one per callee-saved register in rank order
 *          rank 0=ra, 1=s0, 2=s1, 3..12=s2..s11. The p-th SET bit (rank order) lives
 *          at frame-4-4p; PROLOGUE stores it, EPILOGUE loads it.
 * frame:   frame size in bytes (the addi sp amount; <= 2047 in practice, 13 bits here).
 *
 * The whole run is ONE op word -- the collapsed instructions are removed during the
 * two passes, not left behind, so op-index falls behind pc/4 downstream. That drift
 * is invisible to the evaluator (it walks by op cursor, never by pc) and every op is
 * still exactly one word wide, so nothing needs a drift-dependent form.
 * PROLOGUE: sp -= frame, do the stores, advance one op word into the body.
 * EPILOGUE: do the loads, sp += frame, return to ra (it never falls through). */

/* An auipc materialises `pc + uimm20<<12` into rd. Since code is non-relocatable that
 * is always a compile-time CONSTANT (nothing here is truly pc-relative at run time):
 *
 *   DATA constant (the common case: the address half of a pc-relative load/store of a
 *   global or rodata symbol) lands in a fixed data cluster -- just below 0 (rodata) or
 *   near RISCV_HALF (globals) -- that no fusion ever moves. step() emits it as a one-
 *   word LI_LO/LI_HI load-immediate, NOT an auipc; the lo12 load/store/addi that follows
 *   adds its own offset on top. (The data lives in .rodata/.data; the auipc only forms a
 *   POINTER to it -- "pc-relative" is how the linker encoded that pointer, not where the
 *   data sits.)
 *
 *   CODE constant (an auipc materialising a code address -- a function pointer stored as
 *   data, or a far call target) is the target's op-index * 4. That is also a constant,
 *   but it isn't known until the instruction->op map is built, so step() leaves a plain
 *   1-word RISCV_OP_AUIPC and tctool fills the constant in a second pass
 *   (apply_pcrel_code_fixups) once the map exists. The runtime handler computes from the
 *   op CURSOR (its own op position, always exact), so tctool re-encoding the auipc's
 *   uimm + the paired lo12 for the OP-space displacement makes it correct regardless of
 *   how much fusion drifted the stream -- no baked-absolute form is needed. tctool finds
 *   the auipc+addi (or +load/store/jalr) pair via the PCREL_HI20/LO12 relocations
 *   --emit-relocs keeps. A function pointer in a static initializer is instead a link-
 *   time R_RISCV_32 relocation, resolved through the map by patch_rela. */

/* Packed-op field accessors. A/B/C are the three register slots; which register each
 * holds depends on the class (see the layout above), so handlers pick by name.
 * PROLOGUE/EPILOGUE instead pack (regmask, frame) into the low 26 bits. */
#define TC_OP(w)    ((uint32_t)(w) >> 26)
#define TC_A(w)     (((w) >> 21) & 31)   /* rd, or rs1 (branch), or rs2 (store) */
#define TC_B(w)     (((w) >> 16) & 31)   /* rs1, or rs2 (branch)                */
#define TC_C(w)     (((w) >> 11) & 31)   /* rs2 (reg-reg)                       */
#define TC_IMM(w)   ((int32_t)(int16_t)(w))                       /* 16-bit signed */
#define TC_JOFF(w)  ((int32_t)(((w) & 0x1fffffu) << 11) >> 11)    /* 21-bit signed */
#define TC_UIMM(w)  ((w) & 0xfffffu)                              /* 20-bit upper  */
#define TC_SH(w)    (((w) >> 6) & 31)                             /* shadd shift   */
#define TC_OFF11(w) ((int32_t)((w) << 21) >> 21)                  /* lwx 11-bit signed */
#define TC_O1(w)    ((int32_t)(int8_t)((w) >> 8))                 /* lwlw offsets  */
#define TC_O2(w)    ((int32_t)(int8_t)(w))
#define TC_W1(w)    ((((w) >> 6) & 31u) << 2)                     /* lwsw offsets  */
#define TC_W2(w)    (((w) & 63u) << 2)                            /*  (word-scaled, unsigned) */

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
