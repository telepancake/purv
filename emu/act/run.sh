#!/usr/bin/env bash
#
# run.sh - RISC-V Architecture Compatibility Test (ACT) for the purv engine.
#
# Userspace conformance for RV32IMC + Zifencei, checked against golden
# signatures cooked from an INDEPENDENT reference model (Spike, the official
# riscv-isa-sim) and vendored under golden/. Verifying needs only a RISC-V
# cross-assembler and purv; regenerating the goldens needs Spike (REGEN below).
#
#   ./run.sh                 verify purv against the vendored goldens
#   REGEN=1 SPIKE=/path/to/spike ./run.sh
#                            re-cook the goldens from Spike (independent ref)
#
# Knobs (env): CC=riscv64-unknown-elf-gcc  ACT_TAG=3.9.1  PURV=../purv
#              ACTDIR=<arch-test checkout>  (skips the fetch if set)
#
# The tests halt by storing to the `tohost` symbol and looping; purv's host
# (main.c) watches that store and dumps [begin_signature,end_signature). No
# machine mode is involved -- tohost is a plain memory store -- so this is a
# faithful test of a userspace-only engine.
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$(cd "$HERE/.." && pwd)"
CC="${CC:-riscv64-unknown-elf-gcc}"
PURV="${PURV:-$EMU/purv}"
SPIKE="${SPIKE:-spike}"
ACT_TAG="${ACT_TAG:-3.9.1}"
WORK="${WORK:-$HERE/.work}"
GOLDEN="$HERE/golden"

die(){ echo "act: $*" >&2; exit 2; }
command -v "$CC" >/dev/null 2>&1 || die "no cross-compiler '$CC' (set CC=)"
[ -x "$PURV" ] || die "purv not built at $PURV (run: make -C $EMU purv)"

# ---- locate the arch-test sources (env headers + the .S test bodies) ----
ACTDIR="${ACTDIR:-}"
if [ -z "$ACTDIR" ]; then
  ACTDIR="$WORK/riscv-arch-test-$ACT_TAG"
  if [ ! -d "$ACTDIR" ]; then
    mkdir -p "$WORK"
    echo "act: fetching riscv-arch-test $ACT_TAG ..."
    curl -fsSL -o "$WORK/act.tgz" \
      "https://codeload.github.com/riscv-non-isa/riscv-arch-test/tar.gz/refs/tags/$ACT_TAG" \
      || die "fetch failed"
    tar -xzf "$WORK/act.tgz" -C "$WORK" || die "extract failed"
  fi
fi
ENV="$ACTDIR/riscv-test-suite/env"
SUITE="$ACTDIR/riscv-test-suite/rv32i_m"
[ -d "$SUITE" ] || die "no rv32i_m suite under $ACTDIR"

GCC_OPTS=(-mabi=ilp32 -static -mcmodel=medany -fvisibility=hidden
          -nostdlib -nostartfiles -DXLEN=32 -DTEST_CASE_1=True
          -I"$ENV" -I"$HERE" -T"$HERE/link.ld")
declare -A MARCH=([I]=rv32i [M]=rv32im [C]=rv32ic [Zifencei]=rv32i_zifencei)
declare -A SISA=([I]=rv32i [M]=rv32im [C]=rv32ic [Zifencei]=rv32i_zifencei)

mkdir -p "$WORK/build"
pass=0; fail=0; skip=0; failed=""
for ext in I M C Zifencei; do
  for f in "$SUITE/$ext/src"/*.S; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .S)"; tag="$ext/$n"
    # Out of scope 1: cebreak needs a machine-mode trap routine (Zicsr/mtvec/
    # mepc/mret) -- exactly the privileged machinery purv omits by design.
    if [ "$tag" = "C/cebreak-01" ]; then
      echo "SKIP  $tag (needs M-mode trap routine)"; skip=$((skip+1)); continue; fi
    # Out of scope 2: an extension purv does not implement (it is RV32IMC). Zcb
    # (c.mul/c.lbu/c.sext.b/...) and the Zbb it pulls in are a distinct ISA.
    if grep -qE 'Zcb|Zbb|Zba|Zbs|Zbc' "$f"; then
      echo "SKIP  $tag ($(grep -oE 'RV32[A-Za-z_]*' "$f" | head -1) not implemented)"
      skip=$((skip+1)); continue; fi

    elf="$WORK/build/$ext-$n.elf"
    if ! "$CC" -march=${MARCH[$ext]} "${GCC_OPTS[@]}" "$f" -o "$elf" 2>"$WORK/build/$ext-$n.cc.log"; then
      echo "CCFAIL $tag"; fail=$((fail+1)); failed="$failed $tag(cc)"; continue; fi

    gold="$GOLDEN/$ext/$n.sig"
    if [ -n "${REGEN:-}" ]; then
      command -v "$SPIKE" >/dev/null 2>&1 || die "REGEN needs Spike (set SPIKE=)"
      mkdir -p "$(dirname "$gold")"
      timeout 30 "$SPIKE" --isa=${SISA[$ext]} +signature="$gold" +signature-granularity=4 "$elf" \
        >/dev/null 2>&1 || { echo "SPIKE  $tag"; fail=$((fail+1)); continue; }
      echo "COOK  $tag"; pass=$((pass+1)); continue
    fi

    [ -f "$gold" ] || { echo "NOGOLD $tag"; fail=$((fail+1)); failed="$failed $tag(nogold)"; continue; }
    dut="$WORK/build/$ext-$n.dut"
    if ! timeout 30 "$PURV" --signature="$dut" "$elf" >"$WORK/build/$ext-$n.purv.log" 2>&1; then
      echo "RUNERR $tag"; fail=$((fail+1)); failed="$failed $tag(run)"; continue; fi
    if diff -q "$gold" "$dut" >/dev/null 2>&1; then
      pass=$((pass+1))
    else
      echo "FAIL  $tag (signature mismatch vs Spike golden)"; fail=$((fail+1)); failed="$failed $tag"
    fi
  done
done
echo "==================================================="
if [ -n "${REGEN:-}" ]; then echo "cooked $pass golden(s), $fail error(s)"
else echo "ACT RV32IMC userspace: PASS=$pass FAIL=$fail SKIP=$skip"; fi
[ -n "$failed" ] && echo "failed:$failed"
exit $((fail>0))
