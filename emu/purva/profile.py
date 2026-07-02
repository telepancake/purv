#!/usr/bin/env python3
"""profile.py - rank hot ops and fusable adjacent pairs in a purva run.

Joins a .img (the transcoded op array) with the counter dump a PURVA_PROFILE
engine writes (make -C ../sqlite host-purva-prof; one little-endian uint64
execution count per op-word index) and reports where the dynamic op budget
actually goes: per-op-kind totals, the hottest straight-line ADJACENT PAIRS
with their register-chaining relation, and tallies for specific fusion
candidates. This is the evidence for adding (or rejecting) fused ops in
transcode.c -- the transcoder can afford any analysis; the evaluator pays for
every new handler, so only patterns that dominate here earn one.

usage: profile.py prog.img prog.prof [--pairs=N]
"""
import struct
import sys
from collections import Counter

OPS = [
    "ADD", "SLL", "SLT", "SLTU", "XOR", "SRL", "OR", "AND", "SUB", "SRA",
    "ADDI", "SLLI", "SLTI", "SLTIU", "XORI", "SRLI", "ORI", "ANDI", "SRAI",
    "MUL", "MULH", "MULHSU", "MULHU", "DIV", "DIVU", "REM", "REMU",
    "LB", "LH", "LW", "LBU", "LHU", "SB", "SH", "SW",
    "BEQ", "BNE", "BLT", "BGE", "BLTU", "BGEU", "JAL", "JALR",
    "LUI", "AUIPC", "NOP", "ECALL", "EBREAK", "ILLEGAL",
    "PROLOGUE", "EPILOGUE", "LI_LO", "LI_HI",
    "SHADD", "LWX", "LWLW", "LW_BZ", "LBU_BZ", "LWSW", "VCALL", "MEMOP",
]
OPS += [f"OP{i}" for i in range(len(OPS), 64)]
REG_REG = set(range(0, 10)) | set(range(19, 27))       # ADD..SRA, MUL..REMU
REG_IMM = set(range(10, 19))                           # ADDI..SRAI
LOADS = set(range(27, 32))
STORES = set(range(32, 35))
BRANCH = set(range(35, 41))
LI = {51, 52}


def dec(w):
    """(op, A, B, C, imm16, joff21) -- the transcode.h field split."""
    op = w >> 26
    a, b, c = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm = w & 0xFFFF
    if imm >= 0x8000:
        imm -= 0x10000
    joff = w & 0x1FFFFF
    if joff >= 0x100000:
        joff -= 0x200000
    return op, a, b, c, imm, joff


def txt(w):
    op, a, b, c, imm, joff = dec(w)
    n = OPS[op]
    if op in REG_REG:
        return f"{n} x{a}, x{b}, x{c}"
    if op in REG_IMM or op == 42:                      # JALR
        return f"{n} x{a}, x{b}, {imm}"
    if op in LOADS:
        return f"{n} x{a}, {imm}(x{b})"
    if op in STORES:
        return f"{n} x{a} -> {imm}(x{b})"
    if op in BRANCH:
        return f"{n} x{a}, x{b}, .{imm:+}"
    if op == 41:                                       # JAL
        return f"{n} x{a}, .{joff:+}"
    if op in (43, 44):                                 # LUI/AUIPC
        return f"{n} x{a}, 0x{w & 0xFFFFF:05x}"
    if op in LI:
        return f"{n} x{a}, {joff}"
    if op in (49, 50):                                 # PROLOGUE/EPILOGUE
        return f"{n} mask=0x{(w >> 13) & 0x1FFF:04x} frame={w & 0x1FFF}"
    return n


def relation(w1, w2):
    """How the second op consumes the first one's result (rough but telling)."""
    op1, a1, b1, c1, _, _ = dec(w1)
    op2, a2, b2, c2, _, _ = dec(w2)
    if op1 in (49, 50) or op2 in (49, 50):
        return ""
    d1 = a1 if (op1 in REG_REG or op1 in REG_IMM or op1 in LOADS or op1 in (43, 44) or op1 in LI) else None
    if d1 is None:
        return ""
    uses = []
    if op2 in REG_REG:
        srcs = (b2, c2)
    elif op2 in REG_IMM or op2 in LOADS or op2 == 42:
        srcs = (b2,)
    elif op2 in STORES:
        srcs = (b2, a2)                                # a2 is the stored value
    elif op2 in BRANCH:
        srcs = (a2, b2)
    else:
        srcs = ()
    if d1 in srcs:
        uses.append("feeds")
    d2 = a2 if (op2 in REG_REG or op2 in REG_IMM or op2 in LOADS or op2 in (43, 44) or op2 in LI) else None
    if d2 is not None and d2 == d1:
        uses.append("dest-reused" if uses else "dest-clobbered")
    return ",".join(uses)


def main():
    pairs_n = 30
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for a in sys.argv[1:]:
        if a.startswith("--pairs="):
            pairs_n = int(a.split("=")[1])
    if len(args) != 2:
        sys.exit(__doc__)
    img, prof = args

    with open(img, "rb") as f:
        code_size = struct.unpack("<5I", f.read(20))[0]
        ops = list(struct.unpack(f"<{code_size // 4}I", f.read(code_size)))
    with open(prof, "rb") as f:
        raw = f.read()
    counts = list(struct.unpack(f"<{len(raw) // 8}Q", raw))
    assert len(counts) == len(ops), f"{len(counts)} counters vs {len(ops)} ops"
    total = sum(counts)
    print(f"{len(ops)} ops, {total:,} dispatches\n")

    by_kind = Counter()
    for w, n in zip(ops, counts):
        by_kind[OPS[w >> 26]] += n
    print("== dynamic op mix ==")
    for name, n in by_kind.most_common(20):
        print(f"  {name:9s} {n:14,}  {100 * n / total:5.1f}%")

    # Adjacent pairs: op i falls through into i+1 unless i is a taken jump.
    # min(count) is exact for pure straight-line neighbours and an upper bound
    # around branches -- good enough to rank fusion candidates.
    pair_w = Counter()
    pattern = Counter()
    for i in range(len(ops) - 1):
        n = min(counts[i], counts[i + 1])
        if n == 0:
            continue
        w1, w2 = ops[i], ops[i + 1]
        op1, op2 = w1 >> 26, w2 >> 26
        if op1 in BRANCH or op1 in (41, 42, 50):       # taken-jump ends the pair
            continue
        rel = relation(w1, w2)
        key = (OPS[op1], OPS[op2], rel)
        pair_w[key] += n

        d = dec(w1), dec(w2)
        (o1, a1, b1, c1, i1, j1), (o2, a2, b2, c2, i2, j2) = d
        if o1 in LI and o2 == 29 and a1 == b2 and a1 == a2:
            pattern["LI+LW  same rd  (abs load, 1-op)"] += n
        elif o1 in LI and o2 in LOADS and a1 == b2:
            pattern["LI+load        (abs load)"] += n
        elif o1 in LI and o2 in STORES and a1 == b2:
            pattern["LI+store       (abs store)"] += n
        if o1 == 43 and o2 == 10 and a1 == b2 and a1 == a2:
            pattern["LUI+ADDI same rd (const32)"] += n
        if o1 == 11 and o2 == 0 and a1 == a2 and a1 in (b2, c2):
            pattern["SLLI+ADD -> shNadd (same rd)"] += n
        if o1 == 0 and o2 == 29 and a1 == b2 and a1 == a2:
            pattern["ADD+LW   same rd (indexed load)"] += n
        if o1 == 29 and o2 == 29 and a1 == b2 and a1 == a2:
            pattern["LW+LW    same rd (double indirection)"] += n
        if o1 == 29 and o2 == 29 and a1 == b2 and a1 != a2:
            pattern["LW+LW    chain, differing rd"] += n
        if o1 == 10 and o2 == 29 and a1 == b2 and a1 == a2:
            pattern["ADDI+LW  same rd"] += n
        if (o1 in REG_IMM or o1 in REG_REG) and o2 in BRANCH and a1 in (a2, b2):
            pattern["ALU+branch feeds"] += n
        if o1 in LOADS and o2 in BRANCH and a1 in (a2, b2):
            pattern["load+branch feeds"] += n
            if o2 in (35, 36) and (a2 == 0 or b2 == 0):
                pattern["load+beq/bne-vs-zero"] += n
        if o1 in REG_REG and o2 in (35, 36) and a1 in (a2, b2) and (a2 == 0 or b2 == 0):
            pattern["ALU+beq/bne-vs-zero"] += n
        if o1 == 29 and o2 == 42 and a1 == b2:
            pattern["LW+JALR feeds (indirect call via mem)"] += n
        if o1 == 29 and o2 == 34 and a1 == a2 and b1 == b2:
            pattern["LW+SW same base (member copy)"] += n
        if o1 == 29 and o2 == 29 and a1 == b2 and -128 <= i1 < 128 and -128 <= i2 < 128:
            pattern["LW+LW chain, both offsets fit 8 bits"] += n

    print("\n== hottest adjacent pairs (fall-through, min-count weight) ==")
    for (n1, n2, rel), n in pair_w.most_common(pairs_n):
        r = f"  [{rel}]" if rel else ""
        print(f"  {n:14,}  {n1:8s} -> {n2:8s}{r}")

    print("\n== fusion-candidate patterns ==")
    for name, n in pattern.most_common():
        print(f"  {n:14,}  {100 * n / total:5.2f}%  {name}")


if __name__ == "__main__":
    main()
