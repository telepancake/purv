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
 *                    map[pc>>1], using the SAME fusion decision pass 2 will
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
 * directly); `target` is an absolute pc for branch/jal. `width` is the
 * instruction width (2 or 4; transcode is RV32IM-only, so always 4 here, kept
 * for symmetry with purvs's decoder shape). */
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

/* Output op words for an op: 2 for RISCV_OP_AUIPC_ABS (its baked absolute value
 * spills a second word -- see transcode.h), 1 for everything else, including a
 * fused SPILL2 (it covers 8 bytes of original code, but its consumed-byte count is
 * reported by try_fuse_spill2's return value, separate from this). */
static int op_words(uint8_t op) { return op == RISCV_OP_AUIPC_ABS ? 2 : 1; }

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

/* Callee-saved integer registers (RV32 ilp32): ra, s0, s1, s2..s11. Their canonical
 * save/restore rank (highest frame slot first) is ra, s0, s1, s2..s11 -- rank_of
 * returns that 0..12 index, or -1 for any non-callee-saved register. */
static int rank_of(uint32_t r) {
    if (r == 1) return 0;
    if (r == 8) return 1;
    if (r == 9) return 2;
    if (r >= 18 && r <= 27) return 3 + (int)(r - 18);
    return -1;
}

/* A prologue/epilogue's saved set is canonical iff its offsets are exactly
 * {N-4, N-8, .., N-4-4(cnt-1)} and, as the offset descends, the register rank
 * strictly increases (ra above s0 above s1 ..). Fills *rmask on success. This is
 * what lets the op word store only (frame, mask): each register's slot is derived
 * from its position among the set bits. See transcode.h. */
static int frame_canonical(const uint32_t *regs, const int32_t *offs, uint32_t cnt,
                           uint32_t N, uint32_t *rmask) {
    int prev = -1; uint32_t m = 0;
    for (uint32_t p = 0; p < cnt; p++) {
        int32_t want = (int32_t)N - 4 - 4 * (int32_t)p;
        int found = -1;
        for (uint32_t c = 0; c < cnt; c++) if (offs[c] == want) { found = (int)c; break; }
        if (found < 0) return 0;
        int rk = rank_of(regs[found]);
        if (rk <= prev) return 0;                 /* strictly increasing rank as offset drops */
        prev = rk; m |= 1u << rk;
    }
    *rmask = m;
    return 1;
}

static uint32_t frame_enc(uint32_t rmask, uint32_t N) { return (rmask << 13) | (N & 0x1fffu); }

/* Fusion always replays the exact instruction effects, so a match is never wrong on
 * semantics -- the constraints are only what the compact (frame,mask) encoding needs:
 * a canonical top-of-frame layout, a frame that fits, and no jump landing in the
 * middle of the run being collapsed (its head slot, where returns/entries converge,
 * is kept and still maps to the fused op). */

/* Prologue at `off`: `addi sp,sp,-N` then a contiguous canonical run of callee-saved
 * `sw`s, none of them a jump target. On success fills *out (op=PROLOGUE) and returns
 * the byte width consumed (the addi plus every save, all collapsed to ONE op word);
 * else 0. */
static uint32_t try_fuse_prologue(const uint8_t *code, uint32_t len, uint32_t off,
                                  const Targets *targets, Dec *out) {
    Dec a;
    decode(code, off, &a);
    if (a.op != RISCV_OP_ADDI || a.rd != 2 || a.rs1 != 2 || (int32_t)a.imm >= 0) return 0;
    uint32_t N = (uint32_t)(-(int32_t)a.imm);
    if (N == 0 || N > 0x1fff) return 0;
    uint32_t regs[13]; int32_t offs[13]; uint32_t cnt = 0, o = off + 4;
    while (o + 4 <= len && cnt < 13) {
        if (targets_has(targets, o)) break;               /* a save that is a target keeps its slot */
        Dec s;
        decode(code, o, &s);
        if (s.op != RISCV_OP_SW || s.rs1 != 2 || rank_of(s.rs2) < 0) break;
        regs[cnt] = s.rs2; offs[cnt] = (int32_t)s.imm; cnt++; o += 4;
    }
    uint32_t rmask;
    if (cnt == 0 || !frame_canonical(regs, offs, cnt, N, &rmask)) return 0;
    out->op = RISCV_OP_PROLOGUE; out->imm = frame_enc(rmask, N);
    return 4 * (1 + cnt);
}

/* Epilogue at `off`: a contiguous canonical run of callee-saved `lw`s, then
 * `addi sp,sp,+N`, then `jalr zero,0(ra)`, with the dealloc and ret (and any interior
 * load) not jump targets. On success fills *out (op=EPILOGUE) and returns the byte
 * width consumed (loads + dealloc + ret, collapsed to ONE op word); else 0. */
static uint32_t try_fuse_epilogue(const uint8_t *code, uint32_t len, uint32_t off,
                                  const Targets *targets, Dec *out) {
    uint32_t regs[13]; int32_t offs[13]; uint32_t cnt = 0, o = off;
    while (o + 4 <= len && cnt < 13) {
        if (o != off && targets_has(targets, o)) break;   /* interior load target keeps its slot */
        Dec l;
        decode(code, o, &l);
        if (l.op != RISCV_OP_LW || l.rs1 != 2 || rank_of(l.rd) < 0) break;
        regs[cnt] = l.rd; offs[cnt] = (int32_t)l.imm; cnt++; o += 4;
    }
    if (cnt == 0 || o + 8 > len) return 0;
    if (targets_has(targets, o) || targets_has(targets, o + 4)) return 0;   /* dealloc/ret not targets */
    Dec ad, rt;
    decode(code, o, &ad);
    if (ad.op != RISCV_OP_ADDI || ad.rd != 2 || ad.rs1 != 2 || (int32_t)ad.imm <= 0) return 0;
    uint32_t N = ad.imm;
    if (N > 0x1fff) return 0;
    decode(code, o + 4, &rt);
    if (rt.op != RISCV_OP_JALR || rt.rd != 0 || rt.rs1 != 1 || (int32_t)rt.imm != 0) return 0;
    uint32_t rmask;
    if (!frame_canonical(regs, offs, cnt, N, &rmask)) return 0;
    out->op = RISCV_OP_EPILOGUE; out->imm = frame_enc(rmask, N);
    return (o + 8) - off;
}

/* Decode (or fuse) the unit starting at `off`; returns the byte width consumed and
 * fills *d. Shared by build_map and transcode_ex's emit pass so they make the
 * identical decision -- both are pure functions of (code, len, off, at, targets).
 *
 * `at` is the op-word offset this unit will land at, tracked by the caller. If it
 * still equals off/4, no fusion has happened anywhere earlier in the stream, so
 * op-index == pc/4 holds and an auipc here is exactly as safe as it always was
 * (decode() already produced the ordinary 1-word RISCV_OP_AUIPC). Once at has
 * fallen behind off/4 (an earlier prologue/epilogue collapsed several instructions
 * into one op word), that identity is gone for everything downstream, including this
 * auipc -- so its cursor-based runtime value would be wrong, and step() upgrades it
 * to RISCV_OP_AUIPC_ABS, baking the real absolute value (off + uimm20<<12, using the
 * TRUE pc this function already has, not the drifted cursor the evaluator would have
 * at run time) into a second word (transcode.h has the full argument for why this is
 * safe: rodata is a separate region at its own fixed real address regardless of how
 * code transcoded, so the baked real address needs no further correction). */
static uint32_t step(const uint8_t *code, uint32_t len, uint32_t off, uint32_t at,
                      const Targets *targets, Dec *d) {
    uint32_t fused = try_fuse_prologue(code, len, off, targets, d);
    if (fused) return fused;
    fused = try_fuse_epilogue(code, len, off, targets, d);
    if (fused) return fused;
    decode(code, off, d);
    if (d->op == RISCV_OP_AUIPC && at != off / 4) {
        d->op = RISCV_OP_AUIPC_ABS;
        d->target = off + (d->imm << 12);
    }
    return d->width;
}

/* Resolve a branch/jal target pc to its op-word offset. A target outside the code
 * window or not at an instruction start (shouldn't happen for real code; a
 * defensive fallback) yields `at`, i.e. a zero displacement. */
static uint32_t target_word(const uint32_t *map, uint32_t len, uint32_t target, uint32_t at) {
    if (target >= len) return at;
    uint32_t tw = map[target >> 1];
    return tw == TC_SENTINEL ? at : tw;
}

/* Emit one op at op-word offset `at`; resolve branch/jal targets through `map`
 * (pc>>1 -> op-word offset) to a baked op-relative displacement (scaled x4, so
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
    else if (op == RISCV_OP_PROLOGUE || op == RISCV_OP_EPILOGUE)    /* regmask<<13 | frame */
        ops[at++] = w0 | (d->imm & 0x3ffffffu);
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
    } else if (op == RISCV_OP_AUIPC_ABS) {
        ops[at++] = w0 | rd << 21; ops[at++] = d->target;          /* + baked absolute value */
    } else {                                                       /* nop / traps   */
        ops[at++] = w0;
    }
    return at;
}

/* Pass 1: place every instruction's (or fused pair's) op-word offset in
 * map[pc>>1], using the SAME step() decision pass 2 will make, and report the
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
        wd = step(code, len, off, at, targets, &d);
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
        wd = step(code, len, off, at, &targets, &d);
        at = emit(out->ops, at, &d, map, len);
        off += wd;
    }
    free(map);
    targets_free(&targets);
}

void transcode(const uint8_t *code, uint32_t len, Transcoded *out) {
    transcode_ex(code, len, NULL, out);
}
