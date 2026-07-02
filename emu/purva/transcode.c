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
    /* A pure register write to x0 is architecturally a nop -- fold it into NOP so
     * translation drops it entirely (see op_words/emit: NOP emits zero op words).
     * The evaluator then never sees an x0-destination ALU/LUI/AUIPC op, so those
     * handlers write s->x[rd] with no x0 guard. (Loads and JAL/JALR still reach the
     * evaluator with rd==0 -- a load into x0 probes memory, a bare jump discards its
     * link -- so they keep their own discard.) */
    if (d->rd == 0 && (d->op <= RISCV_OP_REMU || d->op == RISCV_OP_LUI || d->op == RISCV_OP_AUIPC))
        d->op = RISCV_OP_NOP;
}

/* Output op words for an op: 0 for RISCV_OP_NOP (dropped during translation -- it
 * occupies no op slot), 2 for the fused load+branch pairs (word2 is the baked
 * branch displacement), 1 for everything else (a fused prologue/epilogue/la covers
 * several bytes of original code but still emits exactly one op word; its consumed-
 * byte count is the fuse helper's return value, separate from this). */
static int op_words(uint8_t op) {
    if (op == RISCV_OP_NOP) return 0;
    if (op >= RISCV_OP_LW_BEQZ && op <= RISCV_OP_LBU_BNEZ) return 2;
    return 1;
}

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

/* `la` at `off`: `auipc rd, hi` then `addi rd, rd, lo` -- the medany pcrel idiom that
 * materialises a DATA address into rd. The code is non-relocatable, so that address is
 * a compile-time constant: final = off + (hi<<12) + sext(lo), using the TRUE pc (off),
 * exactly the value the two instructions would leave in rd. If final lands in a cluster
 * -- small around 0 (rodata just below 0, code) or small around RISCV_HALF (globals) --
 * it collapses to ONE load-immediate op (LI_LO / LI_HI), dropping the addi. A CODE
 * target (final < code_len) is left alone: those are rewritten to op-indices by the
 * pcrel-code fixups, so their register value is an op-index, not this address. Returns
 * the byte width consumed (8) on a match, else 0. */
static uint32_t try_fuse_la(const uint8_t *code, uint32_t len, uint32_t off,
                            const Targets *targets, Dec *out) {
    if (off + 8 > len) return 0;
    if (targets_has(targets, off + 4)) return 0;          /* the addi keeps its slot if a jump lands on it */
    Dec a, b;
    decode(code, off, &a);
    if (a.op != RISCV_OP_AUIPC) return 0;
    decode(code, off + 4, &b);
    if (b.op != RISCV_OP_ADDI || b.rd != a.rd || b.rs1 != a.rd) return 0;  /* addi rd,rd,lo consumes it in place */
    uint32_t final = off + (a.imm << 12) + (uint32_t)(int32_t)b.imm;
    if (final < len) return 0;                            /* code address: leave to the pcrel-code fixups */
    int32_t lo = (int32_t)final, hi = (int32_t)(final - 0x80000000u);   /* RISCV_HALF */
    if (lo >= -(1 << 20) && lo < (1 << 20))       { out->op = RISCV_OP_LI_LO; out->imm = final & 0x1fffffu; }
    else if (hi >= -(1 << 20) && hi < (1 << 20))  { out->op = RISCV_OP_LI_HI; out->imm = (final - 0x80000000u) & 0x1fffffu; }
    else return 0;                                        /* too far from either anchor */
    out->rd = a.rd;
    return 8;
}

/* Adjacent-pair fusion: collapse two instructions into ONE op when the packed
 * word can replay BOTH exactly. The rule that makes every match sound without
 * liveness analysis: the second instruction's destination must CLOBBER the
 * first's (so the only intermediate value dies inside the op), except LWJALR
 * where the intermediate is the op's own explicit rd. The patterns (and the
 * 8/11-bit offset limits) come from measured pair frequency on real C and C++
 * code -- see profile.py and the enum notes in transcode.h. Returns 8 (both
 * instructions consumed) on a match, else 0. */
static uint32_t try_fuse_pair(const uint8_t *code, uint32_t len, uint32_t off,
                              const Targets *targets, Dec *out) {
    if (off + 8 > len) return 0;
    if (targets_has(targets, off + 4)) return 0;   /* second half must not be a jump target */
    Dec a, b;
    decode(code, off, &a);
    decode(code, off + 4, &b);

    /* slli T,X,k ; add T,B,T (either operand order)  ->  SHADD T = B + (X<<k) */
    if (a.op == RISCV_OP_SLLI && b.op == RISCV_OP_ADD && b.rd == a.rd) {
        uint32_t other = b.rs1 == a.rd ? b.rs2 : b.rs2 == a.rd ? b.rs1 : 32;
        if (other < 32 && other != a.rd) {
            out->op = RISCV_OP_SHADD; out->rd = a.rd; out->rs1 = other;
            out->rs2 = a.rs1; out->imm = a.imm;
            return 8;
        }
    }
    /* add T,a,b ; lw T,off(T)  ->  LWX T = M32[a + b + off11] */
    if (a.op == RISCV_OP_ADD && b.op == RISCV_OP_LW && a.rd != 0 &&
        b.rd == a.rd && b.rs1 == a.rd &&
        (int32_t)b.imm >= -1024 && (int32_t)b.imm < 1024) {
        out->op = RISCV_OP_LWX; out->rd = a.rd; out->rs1 = a.rs1; out->rs2 = a.rs2;
        out->imm = b.imm;
        return 8;
    }
    if (a.op == RISCV_OP_LW && a.rd != 0) {
        /* lw T,o1(a) ; lw T,o2(T)  ->  LWLW (the double indirection) */
        if (b.op == RISCV_OP_LW && b.rd == a.rd && b.rs1 == a.rd &&
            (int32_t)a.imm >= -128 && (int32_t)a.imm < 128 &&
            (int32_t)b.imm >= -128 && (int32_t)b.imm < 128) {
            out->op = RISCV_OP_LWLW; out->rd = a.rd; out->rs1 = a.rs1;
            out->imm = (a.imm & 0xffu) << 8 | (b.imm & 0xffu);
            return 8;
        }
        /* lw T,off(a) ; jalr ra,0(T)  ->  LWJALR (the virtual call) */
        if (b.op == RISCV_OP_JALR && b.rd == 1 && b.rs1 == a.rd && (int32_t)b.imm == 0) {
            out->op = RISCV_OP_LWJALR; out->rd = a.rd; out->rs1 = a.rs1; out->imm = a.imm;
            return 8;
        }
    }
    /* lw/lbu T,off(a) ; beq/bne T,x0,target  ->  two-word load+branch-zero */
    if ((a.op == RISCV_OP_LW || a.op == RISCV_OP_LBU) && a.rd != 0 &&
        (b.op == RISCV_OP_BEQ || b.op == RISCV_OP_BNE) &&
        ((b.rs1 == a.rd && b.rs2 == 0) || (b.rs2 == a.rd && b.rs1 == 0))) {
        int ne = b.op == RISCV_OP_BNE;
        out->op = a.op == RISCV_OP_LW ? (ne ? RISCV_OP_LW_BNEZ  : RISCV_OP_LW_BEQZ)
                                      : (ne ? RISCV_OP_LBU_BNEZ : RISCV_OP_LBU_BEQZ);
        out->rd = a.rd; out->rs1 = a.rs1; out->imm = a.imm; out->target = b.target;
        return 8;
    }
    return 0;
}

/* Decode (or fuse) the unit starting at `off`; returns the byte width consumed and
 * fills *d. Shared by build_map and transcode_ex's emit pass so they make the
 * identical decision -- both are pure functions of (code, len, off, targets). Every op
 * it emits is exactly one op word wide (prologue/epilogue/la fusions collapse several
 * instructions INTO one; a lone data auipc becomes one LI), so op layout never depends
 * on anything but this decision -- there is no drift-dependent sizing to keep the two
 * passes in lockstep on. */
static uint32_t step(const uint8_t *code, uint32_t len, uint32_t off,
                      const Targets *targets, Dec *d) {
    uint32_t fused = try_fuse_prologue(code, len, off, targets, d);
    if (fused) return fused;
    fused = try_fuse_epilogue(code, len, off, targets, d);
    if (fused) return fused;
    fused = try_fuse_la(code, len, off, targets, d);
    if (fused) return fused;
    fused = try_fuse_pair(code, len, off, targets, d);
    if (fused) return fused;
    decode(code, off, d);
    if (d->op == RISCV_OP_AUIPC) {
        /* A lone auipc materialises a constant into rd: val = off + (uimm20<<12), the
         * value at the TRUE pc (code is non-relocatable). When that constant lands in a
         * DATA cluster it is an absolute address that no fusion ever moves, so load it
         * straight -- LI_LO/LI_HI, one word, exactly like the fused `la` above -- and the
         * following lo12 load/store/addi adds its own offset on top. The clusters are
         * disjoint from code: a CODE auipc's value is within +/-2KB of a code target in
         * [0, code_len), so a value <= -2049 (rodata, just below 0) or within 2^20 of
         * RISCV_HALF (globals) cannot be code. Everything else stays a plain 1-word auipc:
         * that is a CODE reference (or the top ~2KB of rodata a code value could alias,
         * which tctool resolves too). tctool re-encodes those against the op-index map --
         * a code address is a CONSTANT (the target's op index * 4), not pc-relative, but
         * that constant isn't known until the map is built, so it's filled in a second
         * pass; the runtime auipc computes from the op CURSOR, so once tctool re-encodes
         * the offset in op space it is correct regardless of fusion drift (no baked
         * absolute needed -- see transcode.h and tctool.c's apply_pcrel_code_fixups). */
        uint32_t val = off + (d->imm << 12);
        int32_t s0 = (int32_t)val, sh = (int32_t)(val - 0x80000000u);   /* dist from 0 / RISCV_HALF */
        if (s0 <= -2049 && s0 >= -(1 << 20))         { d->op = RISCV_OP_LI_LO; d->imm = val & 0x1fffffu; }
        else if (sh >= -(1 << 20) && sh < (1 << 20)) { d->op = RISCV_OP_LI_HI; d->imm = (val - 0x80000000u) & 0x1fffffu; }
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
    else if (op == RISCV_OP_LI_LO || op == RISCV_OP_LI_HI)          /* rd | 21-bit imm (JAL shape) */
        ops[at++] = w0 | rd << 21 | (d->imm & 0x1fffffu);
    else if (op == RISCV_OP_SHADD)                                  /* rd, rs1, rs2, sh[10:6] */
        ops[at++] = w0 | rd << 21 | rs1 << 16 | rs2 << 11 | (d->imm & 31) << 6;
    else if (op == RISCV_OP_LWX)                                    /* rd, rs1, rs2, off[10:0] */
        ops[at++] = w0 | rd << 21 | rs1 << 16 | rs2 << 11 | (d->imm & 0x7ffu);
    else if (op == RISCV_OP_LWLW || op == RISCV_OP_LWJALR)          /* rd, rs1, packed offs / off16 */
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffffu);
    else if (op >= RISCV_OP_LW_BEQZ && op <= RISCV_OP_LBU_BNEZ) {   /* word1: load; word2: disp */
        int32_t delta = (int32_t)target_word(map, len, d->target, at) - (int32_t)at;
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffffu);
        ops[at++] = (uint32_t)(delta * 4);
    } else if (op >= RISCV_OP_BEQ && op <= RISCV_OP_BGEU) {
        int32_t delta = (int32_t)target_word(map, len, d->target, at) - (int32_t)at;
        ops[at++] = w0 | rs1 << 21 | rs2 << 16 | ((uint32_t)(delta * 4) & 0xffff);
    } else if (op == RISCV_OP_JAL) {
        int32_t delta = (int32_t)target_word(map, len, d->target, at) - (int32_t)at;
        ops[at++] = w0 | rd << 21 | ((uint32_t)(delta * 4) & 0x1fffff);
    } else if (op == RISCV_OP_JALR) {
        ops[at++] = w0 | rd << 21 | rs1 << 16 | (d->imm & 0xffff);
    } else if (op == RISCV_OP_LUI || op == RISCV_OP_AUIPC) {
        ops[at++] = w0 | rd << 21 | (d->imm & 0xfffff);            /* lui value / auipc upper */
    } else if (op == RISCV_OP_NOP) {
        /* dropped during translation -- emits zero op words (op_words == 0) */
    } else {                                                       /* traps (ecall/ebreak/illegal) */
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
