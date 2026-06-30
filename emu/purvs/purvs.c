/*
 * purvs.c - RISC-V (RV32IMC + Zifencei) monadic emulator: implementation.
 *
 * purvs is purv (the hand-written engine) restructured into a decode/execute
 * split. purv's inner loop fused fetch, decode, and execute; here it is cut in
 * two, with a deliberately lean intermediate form between them:
 *
 *   1. decode-ahead (decode_one, driven from RiscvEmulatorLoop): from pc, lower
 *      each raw instruction -- 32-bit or compressed -- into an 8-byte
 *      RiscvEmulatorDecoded_t { imm, op, rd, rs1, rs2 } in an internal buffer.
 *      It traces THROUGH unconditional jumps -- a jal/j target is static, so the
 *      run continues at the target (the jump is replaced by just its link write,
 *      or nothing for a plain j) -- and stops only at a conditional branch, an
 *      indirect jump (jalr), a trap, a full buffer, or the code window's end.
 *      Decode reads only the code bytes; it never reads register values, so a
 *      whole run is decoded before any of it runs.
 *
 *   2. direct-threaded execution (RiscvEmulatorDefaultEval): a computed-goto jump
 *      table indexed by the leaf op, where each handler ends by jumping STRAIGHT
 *      to the next op's handler -- so every op carries its own indirect dispatch
 *      and the branch predictor can specialise per op.
 *
 * The intermediate form carries only what cannot be recovered for free at run
 * time. The instruction's pc and width are NOT stored: the loop tracks the running
 * pc and presets state->pc to the run's fall-through before eval, so a sequential
 * op never touches pc and jal/jalr take their link straight from state->pc. Every
 * pc-relative result is baked into `imm` at decode (AUIPC becomes a LUI of pc+imm;
 * a branch/JAL target is the absolute pc+imm), which is the only reason `imm`
 * needs 32 bits. Operand source is folded into the op (ADDI is distinct from ADD).
 *
 * Execution is the pluggable half: state->eval (default RiscvEmulatorDefaultEval)
 * is the one place value semantics live, so an alternate interpretation (tag
 * tracking, taint, tracing, ...) is a drop-in over the same decode and run loop.
 * Code is in the read-only lower half, so a decoded run can never be invalidated
 * by a store -- decoding ahead is always safe.
 */
#include <stdint.h>
#include <string.h>

#include "purvs.h"

/* Base opcodes (instruction[6:0]). */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

/* How many decoded instructions one decode-ahead pass buffers (plus room for the
 * END sentinel). A longer straight-line run just decodes in successive batches. */
enum { RISCV_BLOCK_MAX = 64 };

/* ---- memory: one address translation, used by every load and store ---- */
/* mem_xlate maps [addr, addr+n) to host storage, or NULL on a miss (and on a
 * write to the read-only lower half). The half (addr >> 31) selects a pair of
 * adjacent regions that grow toward each other: region[half*2] up from the half's
 * base, region[half*2+1] down from RISCV_HALF. The load/store sites read/write the
 * bytes explicitly per width (little-endian, so the byte-or / byte-store idioms
 * compile to a single unaligned sized access). */
static inline __attribute__((always_inline))
uint8_t *mem_xlate(const RiscvEmulatorState_t *s, uint32_t addr, uint32_t n, int write) {
    if (write && addr < RISCV_HALF) return (uint8_t *)0;     /* lower half is read-only */
    const RiscvEmulatorRegion_t *r = &s->region[(addr >> 31) << 1];
    uint32_t lo = addr & (RISCV_HALF - 1);
    if (lo + n <= r[0].len) return r[0].ptr + lo;            /* grows up from the base */
    uint32_t down = RISCV_HALF - r[1].len;                   /* grows down from RISCV_HALF */
    if (lo >= down && lo + n <= RISCV_HALF) return r[1].ptr + (lo - down);
    return (uint8_t *)0;
}

static void wr(RiscvEmulatorState_t *s, uint32_t i, uint32_t v) { if (i) s->x[i] = v; }

/* Sign-extend the low `bits` of v. */
static int32_t sext(uint32_t v, int bits) {
    int sh = 32 - bits;
    return (int32_t)(v << sh) >> sh;
}

/* The control transfers and traps that end a decoded run: a branch/jump (the next
 * pc is the target, not pc+width) or a trap (control leaves the engine). They are
 * a contiguous range of leaf ops, so the test is two range checks. */
static int is_terminator(uint8_t op) {
    return (op >= RISCV_OP_BEQ   && op <= RISCV_OP_JALR) ||
           (op >= RISCV_OP_ECALL && op <= RISCV_OP_ILLEGAL);
}

/* ----------------------------------------------------------------- decode */

/* funct3 -> leaf op for the encodings whose width/sign/condition lives in funct3.
 * An out-of-range funct3 is an illegal encoding (returns RISCV_OP_ILLEGAL). */
static uint8_t load_op(uint32_t f3) {
    switch (f3) {
    case 0: return RISCV_OP_LB;  case 1: return RISCV_OP_LH; case 2: return RISCV_OP_LW;
    case 4: return RISCV_OP_LBU; case 5: return RISCV_OP_LHU;
    default: return RISCV_OP_ILLEGAL;
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
    case 6: return RISCV_OP_BLTU; case 7: return RISCV_OP_BGEU;
    default: return RISCV_OP_ILLEGAL;
    }
}

/* Lower the instruction at `code[off]` into *d. Reads only the code bytes. Sets the
 * leaf op, register indices, and the per-op `imm` payload -- baking pc-relative
 * targets (AUIPC value, branch/JAL target) into `imm` using `off`, so nothing
 * downstream needs the instruction's own pc. The case structure mirrors purv's
 * fused decode/execute switch; each former `goto <action>` becomes a leaf op plus
 * the operand fields that action consumed. A trailing fixup stores the raw word in
 * `imm` for trap ops, which the run loop hands to the host as state->inst. This is
 * the decode half of the hot loop and its only caller (the cold pc fixup uses the
 * lean step_one, not this), so inline it -- always_inline here only firms up codegen
 * the single-caller heuristic already chooses, with no cold-copy cost. */
static inline __attribute__((always_inline))
uint32_t decode_one(RiscvEmulatorDecoded_t *d, const uint8_t *code, uint32_t off) {
    uint16_t lo = (uint16_t)(code[off] | code[off + 1] << 8);
    uint32_t raw, width;
    /* Set only the fields the op's handler reads (see the payload table in purvs.h).
     * A field a handler never touches is left as-is -- the buffer is reused, but a
     * stale value in an unread field is harmless. The only ops whose handler reads a
     * field their case does not write are the compare-against-x0 compressed branches
     * (rs2) and C.JR/C.JALR (imm); those zero it explicitly. Decode returns the
     * instruction's width, so the caller needs no second test of the 16/32-bit bit. */

    if ((lo & 3) == 3) {                                  /* ---- 32-bit ---- */
        width = 4;
        uint32_t w = lo | (uint32_t)(code[off + 2] | code[off + 3] << 8) << 16;
        uint32_t rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, f3 = (w >> 12) & 7;
        raw = w;
        d->rd = rd; d->rs1 = rs1;
        switch (w & 0x7f) {
        case OPIMM:                                       /* reg-immediate */
            if (f3 == 1 || f3 == 5) {                     /* shifts: shamt + funct7 */
                d->imm = (w >> 20) & 31;
                d->op = (f3 == 5 && (w >> 25) == 0x20) ? RISCV_OP_SRAI : (uint8_t)(RISCV_OP_ADDI + f3);
            } else {
                d->imm = (uint32_t)((int32_t)w >> 20);    /* I-immediate */
                d->op = RISCV_OP_ADDI + f3;
            }
            break;
        case OP:                                          /* reg-register */
            d->rs2 = (w >> 20) & 31;
            if ((w >> 25) == 1) d->op = RISCV_OP_MUL + f3;            /* RV32M */
            else if ((w >> 25) == 0x20) d->op = (f3 == 0) ? RISCV_OP_SUB : RISCV_OP_SRA;
            else d->op = RISCV_OP_ADD + f3;
            break;
        case LOAD:
            d->op = load_op(f3); d->imm = (uint32_t)((int32_t)w >> 20); break;
        case STORE:
            d->op = store_op(f3); d->rs2 = (w >> 20) & 31;
            d->imm = (uint32_t)(((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f)); break;
        case BRANCH:
            d->op = branch_op(f3); d->rs2 = (w >> 20) & 31;
            d->imm = off + sext((w >> 31 & 1) << 12 | (w >> 7 & 1) << 11 |       /* baked target */
                                (w >> 25 & 0x3f) << 5 | (w >> 8 & 0xf) << 1, 13);
            break;
        case JAL:
            d->op = RISCV_OP_JAL;
            d->imm = off + sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |   /* baked target */
                                (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21);
            break;
        case JALR:
            d->op = RISCV_OP_JALR; d->imm = (uint32_t)((int32_t)w >> 20); break; /* dynamic offset */
        case LUI:   d->op = RISCV_OP_LUI; d->imm = w & 0xfffff000; break;
        case AUIPC: d->op = RISCV_OP_LUI; d->imm = off + (w & 0xfffff000); break; /* fold into LUI */
        case MISCMEM:                                     /* FENCE / FENCE.I -> nop */
            d->op = (rd || rs1 || (f3 != 0 && f3 != 1)) ? RISCV_OP_ILLEGAL : RISCV_OP_NOP;
            break;
        case SYSTEM: {
            uint32_t imm = (w >> 20) & 0xfff;
            if (f3 == 0 && rd == 0 && rs1 == 0 && imm == 0x000) d->op = RISCV_OP_ECALL;
            else if (f3 == 0 && rd == 0 && rs1 == 0 && imm == 0x001) d->op = RISCV_OP_EBREAK;
            else d->op = RISCV_OP_ILLEGAL;                /* CSR / MRET / WFI: not userspace */
            break;
        }
        default: d->op = RISCV_OP_ILLEGAL; break;
        }
    } else {                                              /* ---- compressed ---- */
        width = 2;
        uint16_t c = lo;
        uint32_t rdp = ((c >> 2) & 7) + 8, rsp = ((c >> 7) & 7) + 8;  /* x8..x15 fields */
        uint32_t shamt = ((c >> 2) & 0x1f) | ((c >> 12) & 1) << 5;
        int32_t  ci = sext(shamt, 6);                                 /* CI 6-bit imm */
        uint32_t rd = (c >> 7) & 31;
        raw = lo;
        d->rd = rd;
        switch (((c >> 13) & 7) << 2 | (c & 3)) {
        case 0 << 2 | 0: {                       /* C.ADDI4SPN -> addi rd', sp, uimm */
            uint32_t imm = ((c >> 11) & 3) << 4 | ((c >> 7) & 0xf) << 6 |
                           ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 3;
            d->op = imm ? RISCV_OP_ADDI : RISCV_OP_ILLEGAL;          /* imm 0 reserved */
            d->rd = rdp; d->rs1 = 2; d->imm = imm; break;
        }
        case 2 << 2 | 0:                          /* C.LW -> lw rd', uimm(rs1') */
            d->op = RISCV_OP_LW; d->rd = rdp; d->rs1 = rsp;
            d->imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6; break;
        case 6 << 2 | 0:                          /* C.SW -> sw rs2', uimm(rs1') */
            d->op = RISCV_OP_SW; d->rs1 = rsp; d->rs2 = rdp;
            d->imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6; break;
        case 0 << 2 | 1:                          /* C.ADDI / C.NOP -> addi rd, rd, imm */
            d->op = RISCV_OP_ADDI; d->rs1 = rd; d->imm = (uint32_t)ci; break;
        case 1 << 2 | 1: d->rd = 1; goto cj;      /* C.JAL -> jal ra, off */
        case 5 << 2 | 1: d->rd = 0;               /* C.J -> jal x0, off */
        cj: d->op = RISCV_OP_JAL;
            d->imm = off + sext(((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 | ((c >> 9) & 3) << 8 |
                                ((c >> 8) & 1) << 10 | ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                                ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5, 12);   /* baked target */
            break;
        case 2 << 2 | 1:                          /* C.LI -> addi rd, x0, imm */
            d->op = RISCV_OP_ADDI; d->rs1 = 0; d->imm = (uint32_t)ci; break;
        case 3 << 2 | 1:                          /* C.LUI / C.ADDI16SP */
            if (rd == 2) {                        /* addi sp, sp, nzimm */
                uint32_t imm = sext(((c >> 6) & 1) << 4 | ((c >> 2) & 1) << 5 | ((c >> 5) & 1) << 6 |
                                    ((c >> 3) & 3) << 7 | ((c >> 12) & 1) << 9, 10);
                d->op = imm ? RISCV_OP_ADDI : RISCV_OP_ILLEGAL;
                d->rd = 2; d->rs1 = 2; d->imm = imm; break;
            }
            {                                     /* lui rd, nzimm */
                uint32_t imm = sext(((c >> 2) & 0x1f) << 12 | ((c >> 12) & 1) << 17, 18);
                d->op = imm ? RISCV_OP_LUI : RISCV_OP_ILLEGAL; d->imm = imm; break;
            }
        case 4 << 2 | 1:                          /* C.MISC-ALU */
            d->rd = rsp; d->rs1 = rsp;
            switch ((c >> 10) & 3) {
            case 0: d->op = RISCV_OP_SRLI; d->imm = shamt; break;         /* C.SRLI */
            case 1: d->op = RISCV_OP_SRAI; d->imm = shamt; break;         /* C.SRAI */
            case 2: d->op = RISCV_OP_ANDI; d->imm = (uint32_t)ci; break;  /* C.ANDI */
            default:
                if ((c >> 12) & 1) { d->op = RISCV_OP_ILLEGAL; break; }   /* RV64 only */
                d->rs2 = rdp;
                switch ((c >> 5) & 3) {
                case 0: d->op = RISCV_OP_SUB; break;     /* C.SUB */
                case 1: d->op = RISCV_OP_XOR; break;     /* C.XOR */
                case 2: d->op = RISCV_OP_OR;  break;     /* C.OR  */
                default: d->op = RISCV_OP_AND; break;    /* C.AND */
                }
            }
            break;
        case 6 << 2 | 1: d->op = RISCV_OP_BEQ; d->rs1 = rsp; goto cb;   /* C.BEQZ */
        case 7 << 2 | 1: d->op = RISCV_OP_BNE; d->rs1 = rsp;            /* C.BNEZ */
        cb: d->rs2 = 0;                                                 /* compare against x0 */
            d->imm = off + sext(((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 | ((c >> 5) & 3) << 6 |
                                ((c >> 3) & 3) << 1 | ((c >> 2) & 1) << 5, 9);   /* baked target */
            break;
        case 0 << 2 | 2:                          /* C.SLLI -> slli rd, rd, shamt */
            d->op = RISCV_OP_SLLI; d->rs1 = rd; d->imm = shamt; break;
        case 2 << 2 | 2:                          /* C.LWSP -> lw rd, uimm(sp) */
            d->op = RISCV_OP_LW; d->rs1 = 2;
            d->imm = ((c >> 4) & 7) << 2 | ((c >> 2) & 3) << 6 | ((c >> 12) & 1) << 5; break;
        case 4 << 2 | 2: {                        /* C.JR / C.JALR / C.MV / C.ADD / C.EBREAK */
            uint32_t rs2 = (c >> 2) & 31;
            if (!((c >> 12) & 1)) {
                if (rs2 == 0) { d->op = RISCV_OP_JALR; d->rs1 = rd; d->rd = 0; d->imm = 0; }  /* C.JR */
                else { d->op = RISCV_OP_ADD; d->rd = (c >> 7) & 31; d->rs1 = 0; d->rs2 = rs2; }  /* C.MV */
            } else if (rd == 0 && rs2 == 0) d->op = RISCV_OP_EBREAK;                  /* C.EBREAK */
            else if (rs2 == 0) { d->op = RISCV_OP_JALR; d->rs1 = rd; d->rd = 1; d->imm = 0; }  /* C.JALR */
            else { d->op = RISCV_OP_ADD; d->rs1 = rd; d->rs2 = rs2; }                 /* C.ADD */
            break;
        }
        case 6 << 2 | 2:                          /* C.SWSP -> sw rs2, uimm(sp) */
            d->op = RISCV_OP_SW; d->rs1 = 2; d->rs2 = (c >> 2) & 31;
            d->imm = ((c >> 9) & 0xf) << 2 | ((c >> 7) & 3) << 6; break;
        default: d->op = RISCV_OP_ILLEGAL; break;
        }
    }
    /* Trap ops carry the raw word so the loop can hand it to the host as state->inst. */
    if (d->op >= RISCV_OP_ECALL && d->op <= RISCV_OP_ILLEGAL) d->imm = raw;
    return width;
}

/* ------------------------------------------------------------- default eval */

/* Keep the direct threading intact in this ONE function without nerfing the rest
 * of the translation unit (or the host built alongside it). At -O2 a compiler will
 * merge the 48 identical per-op `goto *tbl[...]` dispatches back into a handful of
 * shared indirect branches, defeating the per-op branch prediction the threading
 * is for. GCC exposes the two responsible passes as a per-function optimize
 * attribute, which turns them off here only (verified: 4 -> 48 distinct dispatch
 * sites). Clang has no equivalent per-function knob -- its `optimize`/`nomerge`
 * attributes are ignored on a computed goto, and `optnone` would disable ALL
 * optimization of the evaluator (far worse than the merge) -- so under Clang the
 * dispatches stay merged. That is acceptable: the default build is GCC (see the
 * Makefile), and the merge is perf-neutral on this engine anyway (the cost is
 * dominated by re-decoding each run, not by dispatch prediction). A Clang user who
 * wants the threading can compile this file with -mllvm -enable-tail-merge=false. */
#if defined(__GNUC__) && !defined(__clang__)
#define PURVS_THREADED __attribute__((optimize("no-crossjumping", "no-gcse")))
#else
#define PURVS_THREADED   /* no per-function equivalent on Clang; see above */
#endif

/* The default value semantics: a DIRECT-THREADED interpreter over the decoded run.
 * Each op handler ends with NEXT, which fetches the next record and jumps straight
 * to its handler through the jump table -- so every op has its own indirect
 * dispatch, no shared dispatch tail. The run is terminated by the RISCV_OP_END
 * sentinel the loop appends at block[count], so NEXT needs no bound check.
 *
 * state->pc was preset by the loop to the run's fall-through, so sequential ops
 * leave it alone, a not-taken branch leaves it alone, and jal/jalr read their link
 * from it. Traps are not run here: a trap op stops eval (the loop sets pc/inst and
 * calls the host handler). Returns the number of real records executed. */
PURVS_THREADED
uint32_t RiscvEmulatorDefaultEval(RiscvEmulatorState_t *s, const RiscvEmulatorDecoded_t *block) {
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
        [RISCV_OP_END] = &&h_end,
    };
    const RiscvEmulatorDecoded_t *d = block;
    uint32_t a, b;

    /* Walk the run by advancing d; NEXT steps to the next record and dispatches. */
    #define NEXT() do { goto *tbl[(++d)->op]; } while (0)
    goto *tbl[d->op];                         /* initial dispatch on block[0] */

    /* ---- ALU reg-reg (rd = x[rs1] <op> x[rs2]) ---- */
    h_add:  wr(s, d->rd, s->x[d->rs1] + s->x[d->rs2]);                          NEXT();
    h_sub:  wr(s, d->rd, s->x[d->rs1] - s->x[d->rs2]);                          NEXT();
    h_sll:  wr(s, d->rd, s->x[d->rs1] << (s->x[d->rs2] & 31));                  NEXT();
    h_slt:  wr(s, d->rd, (int32_t)s->x[d->rs1] < (int32_t)s->x[d->rs2]);        NEXT();
    h_sltu: wr(s, d->rd, s->x[d->rs1] < s->x[d->rs2]);                          NEXT();
    h_xor:  wr(s, d->rd, s->x[d->rs1] ^ s->x[d->rs2]);                          NEXT();
    h_srl:  wr(s, d->rd, s->x[d->rs1] >> (s->x[d->rs2] & 31));                  NEXT();
    h_sra:  wr(s, d->rd, (uint32_t)((int32_t)s->x[d->rs1] >> (s->x[d->rs2] & 31))); NEXT();
    h_or:   wr(s, d->rd, s->x[d->rs1] | s->x[d->rs2]);                          NEXT();
    h_and:  wr(s, d->rd, s->x[d->rs1] & s->x[d->rs2]);                          NEXT();

    /* ---- ALU reg-imm (rd = x[rs1] <op> imm) ---- */
    h_addi:  wr(s, d->rd, s->x[d->rs1] + d->imm);                               NEXT();
    h_slli:  wr(s, d->rd, s->x[d->rs1] << (d->imm & 31));                       NEXT();
    h_slti:  wr(s, d->rd, (int32_t)s->x[d->rs1] < (int32_t)d->imm);             NEXT();
    h_sltiu: wr(s, d->rd, s->x[d->rs1] < d->imm);                              NEXT();
    h_xori:  wr(s, d->rd, s->x[d->rs1] ^ d->imm);                              NEXT();
    h_srli:  wr(s, d->rd, s->x[d->rs1] >> (d->imm & 31));                       NEXT();
    h_ori:   wr(s, d->rd, s->x[d->rs1] | d->imm);                              NEXT();
    h_andi:  wr(s, d->rd, s->x[d->rs1] & d->imm);                              NEXT();
    h_srai:  wr(s, d->rd, (uint32_t)((int32_t)s->x[d->rs1] >> (d->imm & 31)));  NEXT();

    /* ---- RV32M (a = x[rs1]; b = x[rs2]) ---- */
    h_mul:    wr(s, d->rd, s->x[d->rs1] * s->x[d->rs2]); NEXT();
    h_mulh:   a = s->x[d->rs1]; b = s->x[d->rs2]; wr(s, d->rd, (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32)); NEXT();
    h_mulhsu: a = s->x[d->rs1]; b = s->x[d->rs2]; wr(s, d->rd, (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32)); NEXT();
    h_mulhu:  a = s->x[d->rs1]; b = s->x[d->rs2]; wr(s, d->rd, (uint32_t)(((uint64_t)a * b) >> 32)); NEXT();
    h_div:    a = s->x[d->rs1]; b = s->x[d->rs2];
              wr(s, d->rd, b == 0 ? 0xffffffffu : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b)); NEXT();
    h_divu:   a = s->x[d->rs1]; b = s->x[d->rs2]; wr(s, d->rd, b == 0 ? 0xffffffffu : a / b); NEXT();
    h_rem:    a = s->x[d->rs1]; b = s->x[d->rs2];
              wr(s, d->rd, b == 0 ? a : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b)); NEXT();
    h_remu:   a = s->x[d->rs1]; b = s->x[d->rs2]; wr(s, d->rd, b == 0 ? a : a % b); NEXT();

    /* ---- loads (rd = mem[x[rs1] + imm]); miss -> the memory callback ---- */
    h_lb: if (d->rd) { uint32_t ad = s->x[d->rs1] + d->imm; uint8_t *p = mem_xlate(s, ad, 1, 0);
            wr(s, d->rd, p ? (uint32_t)(int32_t)(int8_t)p[0]
                           : (uint32_t)sext(s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0, 8)); }
          NEXT();
    h_lh: if (d->rd) { uint32_t ad = s->x[d->rs1] + d->imm; uint8_t *p = mem_xlate(s, ad, 2, 0);
            wr(s, d->rd, p ? (uint32_t)(int32_t)(int16_t)(p[0] | p[1] << 8)
                           : (uint32_t)sext(s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0, 16)); }
          NEXT();
    h_lw: if (d->rd) { uint32_t ad = s->x[d->rs1] + d->imm; uint8_t *p = mem_xlate(s, ad, 4, 0);
            wr(s, d->rd, p ? ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)
                           : (s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0)); }
          NEXT();
    h_lbu: if (d->rd) { uint32_t ad = s->x[d->rs1] + d->imm; uint8_t *p = mem_xlate(s, ad, 1, 0);
            wr(s, d->rd, p ? p[0] : ((s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0) & 0xff)); }
          NEXT();
    h_lhu: if (d->rd) { uint32_t ad = s->x[d->rs1] + d->imm; uint8_t *p = mem_xlate(s, ad, 2, 0);
            wr(s, d->rd, p ? ((uint32_t)p[0] | (uint32_t)p[1] << 8)
                           : ((s->callback ? s->callback(s, RISCV_MEM_LOAD, ad, 0) : 0) & 0xffff)); }
          NEXT();

    /* ---- stores (mem[x[rs1] + imm] = x[rs2]); miss -> the memory callback ---- */
    h_sb: { uint32_t ad = s->x[d->rs1] + d->imm; b = s->x[d->rs2]; uint8_t *p = mem_xlate(s, ad, 1, 1);
            if (p) p[0] = (uint8_t)b; else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT();
    h_sh: { uint32_t ad = s->x[d->rs1] + d->imm; b = s->x[d->rs2]; uint8_t *p = mem_xlate(s, ad, 2, 1);
            if (p) { p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8); } else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT();
    h_sw: { uint32_t ad = s->x[d->rs1] + d->imm; b = s->x[d->rs2]; uint8_t *p = mem_xlate(s, ad, 4, 1);
            if (p) { p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8); p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24); }
            else if (s->callback) s->callback(s, RISCV_MEM_STORE, ad, b); }
          NEXT();

    /* ---- branches: taken -> the baked absolute target; not taken -> leave pc
     *      (it was preset to the fall-through). Always the run's last real record. ---- */
    h_beq:  if (s->x[d->rs1] == s->x[d->rs2])                   s->pc = d->imm; NEXT();
    h_bne:  if (s->x[d->rs1] != s->x[d->rs2])                   s->pc = d->imm; NEXT();
    h_blt:  if ((int32_t)s->x[d->rs1] <  (int32_t)s->x[d->rs2]) s->pc = d->imm; NEXT();
    h_bge:  if ((int32_t)s->x[d->rs1] >= (int32_t)s->x[d->rs2]) s->pc = d->imm; NEXT();
    h_bltu: if (s->x[d->rs1] <  s->x[d->rs2])                   s->pc = d->imm; NEXT();
    h_bgeu: if (s->x[d->rs1] >= s->x[d->rs2])                   s->pc = d->imm; NEXT();

    /* ---- jumps: link = the preset fall-through in state->pc, then jump ---- */
    h_jal:  wr(s, d->rd, s->pc); s->pc = d->imm; NEXT();
    h_jalr: { uint32_t t = (s->x[d->rs1] + d->imm) & ~1u; wr(s, d->rd, s->pc); s->pc = t; } NEXT();

    /* ---- upper-immediate (LUI, and AUIPC folded in) / nop ---- */
    h_lui:  wr(s, d->rd, d->imm); NEXT();
    h_nop:  NEXT();

    /* ---- traps: stop; the run loop sets pc/inst and calls the host handler ---- */
    h_trap: return (uint32_t)(d - block + 1); /* d points at the trap, which counts */
    #undef NEXT
    h_end:  return (uint32_t)(d - block);     /* d points at the END sentinel (not real) */
}

/* ------------------------------------------------------- early-exit pc fixup */

/* Lean step for the cold fixup walk: just enough to advance pc. Returns the
 * instruction width; for an unconditional jump (the only control flow the walk can
 * meet -- see fixup_pc) sets *jump and, then, the absolute *target and whether it
 * writes a link (jal vs plain j). It does none of decode_one's operand/op work. */
static uint32_t step_one(const uint8_t *code, uint32_t pc, int *jump, int *link, uint32_t *target) {
    uint16_t lo = (uint16_t)(code[pc] | code[pc + 1] << 8);
    *jump = 0;
    if ((lo & 3) == 3) {                                   /* 32-bit */
        uint32_t w = lo | (uint32_t)(code[pc + 2] | code[pc + 3] << 8) << 16;
        if ((w & 0x7f) == JAL) {
            *jump = 1; *link = ((w >> 7) & 31) != 0;
            *target = pc + sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |
                                (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21);
        }
        return 4;
    }
    uint32_t sel = ((lo >> 13) & 7) << 2 | (lo & 3);       /* compressed: C.JAL / C.J */
    if (sel == (1 << 2 | 1) || sel == (5 << 2 | 1)) {
        *jump = 1; *link = (sel == (1 << 2 | 1));          /* C.JAL links ra; C.J does not */
        *target = pc + sext(((lo >> 12) & 1) << 11 | ((lo >> 11) & 1) << 4 | ((lo >> 9) & 3) << 8 |
                            ((lo >> 8) & 1) << 10 | ((lo >> 7) & 1) << 6 | ((lo >> 6) & 1) << 7 |
                            ((lo >> 3) & 7) << 1 | ((lo >> 2) & 1) << 5, 12);
    }
    return 2;
}

/* Recover the resume pc when an evaluator stops before the run's end. Because the
 * decode pass omits plain unconditional jumps from the record stream (their only
 * effect is the pc redirect, which the trace already followed), the loop can't map
 * "records executed" back to a pc by record count alone. So on a short return we
 * re-walk the code from the run's start, advancing through `records` emitted
 * records -- stepping sequential ops by their width and FOLLOWING the absorbed
 * jumps -- and land on the resume pc. The first `records` of a run are never
 * terminators (those are always the run's last record), so the walk only meets
 * sequential ops and unconditional jumps -- exactly what step_one decodes.
 * *insns receives the guest instructions covered (records plus jumps traversed).
 * Cold path: only a custom eval that stops mid-run reaches it. */
static uint32_t fixup_pc(const uint8_t *code, uint32_t pc, uint32_t records, uint32_t *insns) {
    uint32_t recs = 0, cnt = 0, target;
    int jump, link;
    while (recs < records) {
        uint32_t width = step_one(code, pc, &jump, &link, &target);
        cnt++;
        if (jump) {                     /* omitted plain j, or a jal kept as a LUI link */
            if (link) recs++;           /* jal left a record; plain j did not */
            pc = target;
            continue;
        }
        recs++;
        pc += width;
    }
    *insns = cnt;
    return pc;
}

/* ----------------------------------------------------------------- the VM */

uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint64_t max) {
    RiscvEmulatorDecoded_t block[RISCV_BLOCK_MAX + 1];  /* + 1 for the END sentinel */
    const uint8_t *code = s->region[RISCV_CODE].ptr;    /* instruction-fetch window... */
    uint32_t code_len = s->region[RISCV_CODE].len;      /* ...based at 0 */
    RiscvEmulatorEvalFn eval = s->eval ? s->eval : RiscvEmulatorDefaultEval;
    uint64_t k = 0;

    /* `win` is the last pc at which a full 4-byte fetch is in bounds; while pc stays
     * <= win (the overwhelmingly common case, a well-predicted branch) a fetch needs
     * no bounds check. Only the window tail -- or a traced jump landing near/past the
     * end -- re-checks carefully. */
    uint32_t win = code_len >= 4 ? code_len - 4 : 0;

    /* Each iteration: decode a straight-line run from pc, then thread-execute it.
     * `n` counts records emitted for eval; `insns` counts guest instructions -- they
     * differ because plain unconditional jumps are traced across and OMITTED from the
     * record stream (only their pc redirect matters, and the trace follows it), so k
     * (the returned instruction count) advances by `insns`, not by record count. */
    while (k < max) {
        uint64_t room = max - k;
        /* One bound for both the buffer and the budget: records <= guest instructions,
         * so capping guest instructions (insns) at min(room, BLOCK_MAX) keeps n <=
         * BLOCK_MAX implicitly -- a single compare in the hot loop, not two. */
        uint32_t lim = room < RISCV_BLOCK_MAX ? (uint32_t)room : RISCV_BLOCK_MAX;
        uint32_t pc = s->pc, run_pc = pc, n = 0, insns = 0, last_pc = pc;

        /* loop 1: decode ahead, tracing THROUGH unconditional jumps (static targets),
         * until a conditional branch / indirect jump / trap (the run's last record),
         * the buffer/budget cap, or the code window's end. Code is read-only, so
         * nothing eval does can invalidate what we decode. */
        if (code) while (insns < lim) {
            if (pc > win) {                               /* tail / out-of-window: check */
                if (pc >= code_len || code_len - pc < 2) break;
                if (((code[pc] | code[pc + 1] << 8) & 3) == 3 && code_len - pc < 4) break;
            }
            uint32_t width = decode_one(&block[n], code, pc);
            insns++;
            if (block[n].op == RISCV_OP_JAL) {
                /* Statically-known target: trace across it. A jal's lasting effect is
                 * its link write, so keep that as a LUI record; a plain j has no
                 * effect at all, so OMIT it (don't emit a record) -- execution just
                 * flows into the target, the next record. Either way, decode on at
                 * the target. (insns already counted the jump; the fixup re-walk and
                 * decode agree on this, so an early exit recovers pc correctly.) */
                uint32_t target = block[n].imm;           /* the baked absolute target */
                if (block[n].rd) {                        /* jal: keep the link write */
                    block[n].op = RISCV_OP_LUI; block[n].imm = pc + width;
                    last_pc = pc; n++;
                }                                         /* plain j: omitted */
                pc = target;
                continue;
            }
            last_pc = pc;
            pc += width;
            if (is_terminator(block[n++].op)) break;
        }
        if (insns == 0) break;                /* pc outside the code window: nothing to run */

        uint32_t end_pc = pc;                 /* fall-through / link / continue address */
        s->pc = end_pc;                       /* preset: sequential ops and not-taken branches land here */
        if (n == 0) { k += insns; continue; } /* the run was only omitted jumps */

        block[n].op = RISCV_OP_END;           /* sentinel terminates the threaded dispatch */

        /* loop 2: threaded execution of the decoded run (the pluggable half). */
        uint32_t ran = eval(s, block);
        if (ran < n) {                        /* eval stopped early: re-walk to recover pc */
            uint32_t done;
            s->pc = fixup_pc(code, run_pc, ran, &done);
            k += done;
            continue;
        }
        k += insns;                           /* full run: every record executed */

        /* A trap terminates the run without executing in eval: set the faulting pc
         * and raw word, then call the host handler (it may stop or resume). */
        uint8_t term = block[n - 1].op;
        if (term >= RISCV_OP_ECALL && term <= RISCV_OP_ILLEGAL) {
            s->pc = last_pc; s->inst = block[n - 1].imm;
            int stop = (term == RISCV_OP_ECALL)  ? (s->ecall   ? s->ecall(s)   : 0)
                     : (term == RISCV_OP_EBREAK) ? (s->ebreak  ? s->ebreak(s)  : 1)
                     :                             (s->illegal ? s->illegal(s) : 1);
            if (stop) break;                  /* terminal trap (exit, breakpoint, illegal) */
            s->pc = end_pc;                   /* serviced (e.g. a syscall): resume after it */
        }
    }
    return k;
}

/* ------------------------------------------------------------------ init */

/* The default memory callback: a no-op that delivers zero to a missed load. */
static uint32_t default_callback(RiscvEmulatorState_t *s, int op, uint32_t addr, uint32_t value) {
    (void)s; (void)op; (void)addr; (void)value;
    return 0;
}

void RiscvEmulatorInit(RiscvEmulatorState_t *s,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t rodata,
                       RiscvEmulatorRegion_t heap, RiscvEmulatorRegion_t stack) {
    memset(s, 0, sizeof *s);              /* clears registers, regions, handlers */
    s->region[RISCV_CODE]   = code;       /* [0, len): instruction fetch + read-only data */
    s->region[RISCV_RODATA] = rodata;     /* read-only data, just below RISCV_HALF */
    s->region[RISCV_HEAP]   = heap;       /* read/write, from RISCV_HALF up */
    s->region[RISCV_STACK]  = stack;      /* read/write, grows down, ends at the last address */
    s->callback = default_callback;       /* no-op until the host installs its own */
    s->eval     = RiscvEmulatorDefaultEval;  /* threaded RV32 semantics until swapped */
    s->x[2] = 0;                          /* sp = 0 == 2^32, one past the top of the stack */
    s->pc = 0;                            /* code is based at 0 */
}
