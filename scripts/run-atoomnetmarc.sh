#!/usr/bin/env bash
# Generate ACT4 self-checking ELFs (rv32imc, expected results from Sail 0.12) and
# run them on atoomnetmarc/RISC-V-emulator via our ACT4 runner.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
DUT="$ROOT/conformance/dut/atoomnetmarc"

# Build the runner (embeds the atoomnetmarc library, RV32IMC enabled).
mkdir -p tools/bin
gcc -O2 -w -Wno-packed-bitfield-compat \
  -I "$DUT" -I "$ROOT/third_party/atoomnetmarc-rv/include" \
  -DRVE_E_M=1 -DRVE_E_C=1 -DRVE_E_ZICSR=1 -DRVE_E_ZIFENCEI=1 \
  -DRVE_E_A=0 -DRVE_E_ZBA=0 -DRVE_E_ZBB=0 -DRVE_E_ZBC=0 -DRVE_E_ZBS=0 -DRVE_E_HOOK=0 \
  -o tools/bin/atoomnetmarc-runner "$DUT/runner.c"

export PATH="$HOME/.local/bin:$PATH"
eval "$(mise activate bash)" 2>/dev/null || true
export PATH="$ROOT/tools/riscv-gcc/bin:$ROOT/tools/sail-0.12/bin:$ROOT/tools/bin:$PATH"

cd "$ROOT/third_party/riscv-arch-test"
echo "==> generate rv32imc ELFs (Sail 0.12 reference)"
mise exec -- make elfs CONFIG_FILES="$DUT/test_config.yaml"
echo "==> run on atoomnetmarc"
mise exec -- ./run_tests.py "atoomnetmarc-runner" work/atoomnetmarc/elfs
