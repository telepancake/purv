#!/usr/bin/env bash
# selfcheck.sh - run the classic riscv-tests ISA suite against purv.
#
# These tests are self-checking: each embeds its expected results and writes
# pass (tohost=1) or fail (tohost=(testnum<<1)|1) to the HTIF tohost word, so no
# reference model is needed. We build them for rv32imc with clang (no cross-gcc)
# and run each on purv, classifying pass/fail by exit code. If `spike` is on
# PATH it is used only to skip tests Spike itself doesn't pass under this config
# (e.g. ma_data), so a purv-vs-spike disagreement is reported as a real gap.
#
#   ./selfcheck.sh                 # rv32ui rv32um rv32uc
#   ./selfcheck.sh rv32ui rv32mi   # pick suites
set -euo pipefail
cd "$(dirname "$0")"
make -s purv
PURV=$PWD/purv
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
SUITES="${*:-rv32ui rv32um rv32uc}"

fetch() { # repo ref dest
  local out="$W/$3.tgz"
  for r in "$2" master main; do
    if curl -fsSL --max-time 90 -o "$out" "https://codeload.github.com/$1/tar.gz/$r" \
       && tar tzf "$out" >/dev/null 2>&1; then tar xzf "$out" -C "$W"; return 0; fi
  done
  echo "fetch failed: $1" >&2; exit 1
}
fetch riscv-software-src/riscv-tests master rvt
fetch riscv/riscv-test-env         master env
RVT=$(echo "$W"/riscv-tests-*); ENV=$(echo "$W"/riscv-test-env-*)

cat > "$W/link.ld" <<'LD'
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text.init : { *(.text.init) }
  . = ALIGN(0x1000); .tohost : { *(.tohost) }
  . = ALIGN(0x1000); .text   : { *(.text) }
  . = ALIGN(0x1000); .data   : { *(.data) } .bss : { *(.bss) }
  _end = .;
}
LD

M="--target=riscv32 -march=rv32imc_zicsr_zifencei -mabi=ilp32 -static -nostdlib -fno-pic"
INC="-I $ENV/p -I $ENV -I $RVT/isa/macros/scalar"
ISA=rv32imc_zicsr_zifencei
SPIKE=$(command -v spike || true)

total=0 passed=0 failed=0 skip_build=0 skip_spike=0
for suite in $SUITES; do
  for s in "$RVT"/isa/"$suite"/*.S; do
    [ -e "$s" ] || continue
    n=$(basename "$s" .S); elf="$W/$suite-$n.elf"; total=$((total+1))
    if ! clang $M $INC -T "$W/link.ld" "$s" -o "$elf" 2>/dev/null; then
      skip_build=$((skip_build+1)); continue
    fi
    if [ -n "$SPIKE" ] && ! "$SPIKE" --isa=$ISA "$elf" >/dev/null 2>&1; then
      skip_spike=$((skip_spike+1)); continue
    fi
    if timeout 30 "$PURV" "$elf" >/dev/null 2>&1; then
      passed=$((passed+1))
    else
      failed=$((failed+1)); echo "  FAIL: $suite/$n"
    fi
  done
done
echo "purv self-check: $passed/$((passed+failed)) runnable passed" \
     "(build-skip $skip_build, spike-skip $skip_spike, total $total)"
[ "$failed" -eq 0 ]
