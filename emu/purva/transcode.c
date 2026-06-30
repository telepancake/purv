/*
 * transcode.c - ahead-of-time RISC-V (RV32IM) -> "our ops" rewriter.
 *
 * Three internal sweeps over the code, each O(n), each a pure function of
 * (code, len, ext):
 *
 *   collect_targets  every direct branch/jal destination, unioned with the
 *                    caller-supplied external (indirectly-reachable) addresses.
 *                    Must finish before any fusion decision -- a target can be set
 *                    by an instruction anywhere in the program, including after the
 *                    candidate pair being considered.
 *   build_map        place every instruction's (or fused pair's) op-word offset in
 *                    map[orig_pc>>1], using the SAME fusion decision pass 2 will
 *                    make (see try_fuse_spill2) so the map and the emitted array
 *                    agree on layout.
 *   emit (in         lower each instruction/pair into its packed op word(s),
 *   transcode_ex)    resolving branch/jal targets through the map.
 *
 * For RV32IM with nothing fused, op layout is 1:1 with the instruction stream
 * (op-index == pc/4 everywhere) -- but SPILL2 (see transcode.h) breaks that
 * identity on purpose where it's provably safe, which is why this stays three real
 * passes instead of a single direct sweep.
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

/* One decoded instruction. `imm` is the immediate for the ops that keep one inline
 * (for STORE specifically, the signed S-immediate -- try_fuse_spill2 reads it
 * directly); `target` is an ABSOLUTE original pc for branch/jal. `width` is the
 * original instruction width (2 or 4; transcode is RV32IM-only, so always 4 here,
 * kept for symmetry with purvs's decoder shape). */
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

/* Output op words for an op. Always 1: a fused SPILL2 covers 8 bytes of original
 * code but is still a single op word (its consumed-byte count is reported by
 * try_fuse_spill2's return value, separate from this). auipc stays a single word
 * too (its value is computed at run time from the live cursor, which equals the
 * original pc exactly as long as nothing before it ran ahead of pc/4 -- see
 * transcode.h's note on why that must hold, and SPILL2's contract for why fusion
 * doesn't break it). */
static int op_words(uint8_t op) { (void)op; return 1; }

/* ---- target set: addresses that must keep their own op slot ---- */

typedef struct { uint8_t *bits; uint32_t n; } Targets;   /* n = 4-byte slots covered */

static Targets targets_new(uint32_t len) {
    uint32_t n = len / 4;
    Targets t = { calloc(1, (size_t)(n + 7) / 8 + 1), n };
    return t;
}
static void targets_mark(Targets *t, uint32_t addr) {
    uint32_t i = addr / 4;
    if (i < t->n) t->bits[i / 8] |= (uint8_t)(1u << (i % 8));
}
static int targets_has(const Targets *t, uint32_t addr) {
    uint32_t i = addr / 4;
    return i < t->n && (t->bits[i / 8] & (1u << (i % 8))) != 0;
}
static void targets_free(Targets *t) { free(t->bits); t->bits = NULL; }

/* Every direct branch/jal destination in `code`, unioned with `ext` (the caller's
 * indirectly-reachable addresses -- see transcode_ex). This is the full set of
 * positions fusion must never hide as a second half. */
static Targets collect_targets(const uint8_t *code, uint32_t len, const TcExternalTargets *ext) {
    Targets t = targets_new(len);
    Dec d;
    for (uint32_t off = 0; off + 4 <= len; off += 4) {
        decode(code, off, &d);
        if (d.op == RISCV_OP_JAL || (d.op >= RISCV_OP_BEQ && d.op <= RISCV_OP_BGEU))
            targets_mark(&t, d.target);
    }
    if (ext) for (uint32_t i = 0; i < ext->n; i++) targets_mark(&t, ext->addr[i]);
    return t;
}

/* Try to fuse the store-pair at `off` into one SPILL2 record (transcode.h has the
 * full safety argument). Two adjacent `sw` to the same base at offsets exactly 4
 * apart, where the second instruction is not in `targets` and the lower offset
 * fits the op word's signed 11-bit field. On success fills *out (op=SPILL2,
 * rs1=base, rd=low-address value, rs2=high-address value, imm=low offset) and
 * returns 8 (bytes consumed); on failure returns 0 and *out is untouched. */
static uint32_t try_fuse_spill2(const uint8_t *code, uint32_t len, uint32_t off,
                                 const Targets *targets, Dec *out) {
    if (off + 8 > len) return 0;
    Dec d1, d2;
    decode(code, off, &d1);
    if (d1.op != RISCV_OP_SW) return 0;
    decode(code, off + 4, &d2);
    if (d2.op != RISCV_OP_SW || d2.rs1 != d1.rs1) return 0;
    if (targets_has(targets, off + 4)) return 0;            /* provably never a target -- required */
    int32_t off1 = (int32_t)d1.imm, off2 = (int32_t)d2.imm;
    if (off2 - off1 != 4 && off1 - off2 != 4) return 0;
    int32_t lo_off = off1 < off2 ? off1 : off2;
    uint8_t lo_val = (uint8_t)(off1 < off2 ? d1.rs2 : d2.rs2);
    uint8_t hi_val = (uint8_t)(off1 < off2 ? d2.rs2 : d1.rs2);
    if (lo_off < -1024 || lo_off > 1023) return 0;           /* must fit the 11-bit field */
    out->op = RISCV_OP_SPILL2;
    out->rs1 = d1.rs1; out->rd = lo_val; out->rs2 = hi_val; out->imm = (uint32_t)lo_off;
    return 8;
}

/* Decode (or fuse) the unit starting at `off`; returns the byte width consumed and
 * fills *d. Shared by build_map and transcode_ex's emit pass so they make the
 * identical decision -- both are pure functions of (code, len, off, targets). */
static uint32_t step(const uint8_t *code, uint32_t len, uint32_t off, const Targets *targets, Dec *d) {
    uint32_t fused = try_fuse_spill2(code, len, off, targets, d);
    if (fused) return fused;
    decode(code, off, d);
    return d->width;
}

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
    else if (op == RISCV_OP_SPILL2)                                 /* base | v1 | v2 | off11 */
        ops[at++] = w0 | rs1 << 21 | rd << 16 | rs2 << 11 | (d->imm & 0x7ffu);
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

/* Pass 1: place every instruction's (or fused pair's) op-word offset in
 * map[orig_pc>>1], using the SAME step() decision pass 2 will make, and report the
 * total op count. */
static void build_map(const uint8_t *code, uint32_t len, const Targets *targets, uint32_t *map, uint32_t *n_ops) {
    size_t mapn = (size_t)(len >> 1) + 2;
    for (size_t i = 0; i < mapn; i++) map[i] = TC_SENTINEL;
    Dec d;
    uint32_t at = 0;
    for (uint32_t off = 0; off + 2 <= len; ) {
        uint16_t lo = code[off] | code[off+1] << 8;
        uint32_t wd = ((lo & 3) == 3) ? 4 : 2;
        if (off + wd > len) break;
        wd = step(code, len, off, targets, &d);
        map[off >> 1] = at;
        at += op_words(d.op);
        off += wd;
    }
    *n_ops = at;
}

uint32_t *transcode_map_ex(const uint8_t *code, uint32_t len, const TcExternalTargets *ext, uint32_t *n_ops_out) {
    Targets targets = collect_targets(code, len, ext);
    uint32_t *map = malloc(((size_t)(len >> 1) + 2) * sizeof *map);
    build_map(code, len, &targets, map, n_ops_out);
    targets_free(&targets);
    return map;
}

uint32_t *transcode_map(const uint8_t *code, uint32_t len, uint32_t *n_ops_out) {
    return transcode_map_ex(code, len, NULL, n_ops_out);
}

void transcode_ex(const uint8_t *code, uint32_t len, const TcExternalTargets *ext, Transcoded *out) {
    Targets targets = collect_targets(code, len, ext);
    uint32_t n_ops;
    uint32_t *map = malloc(((size_t)(len >> 1) + 2) * sizeof *map);
    build_map(code, len, &targets, map, &n_ops);
    out->n_ops = n_ops;
    out->code_len = n_ops * 4;
    out->ops = malloc(((size_t)n_ops + 2) * sizeof *out->ops);

    uint32_t at = 0;                                       /* pass 2: emit */
    Dec d;
    for (uint32_t off = 0; off + 2 <= len; ) {
        uint16_t lo = code[off] | code[off+1] << 8;
        uint32_t wd = ((lo & 3) == 3) ? 4 : 2;
        if (off + wd > len) break;
        wd = step(code, len, off, &targets, &d);
        at = emit(out->ops, at, &d, map, len);
        off += wd;
    }
    free(map);
    targets_free(&targets);
}

void transcode(const uint8_t *code, uint32_t len, Transcoded *out) {
    transcode_ex(code, len, NULL, out);
}
