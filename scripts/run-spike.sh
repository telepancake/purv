#!/usr/bin/env bash
# Generate ACT4 self-checking ELFs (expected results computed by Sail 0.12) and
# run them on the locally-built Spike. This is the reference cross-check:
# Sail generates the golden expectations, Spike executes + self-checks.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

# Toolchain on PATH: mise (uv/ruby), our Spike, prebuilt Sail 0.12, riscv gcc.
export PATH="$HOME/.local/bin:$PATH"
eval "$(mise activate bash)" 2>/dev/null || true
# tools/riscv-gcc must come first: ACT4 requires GCC 15+, newer than the apt
# gcc-riscv64-unknown-elf (13.2.0) on Ubuntu 24.04.
export PATH="$ROOT/tools/riscv-gcc/bin:$ROOT/third_party/riscv-isa-sim/build:$ROOT/tools/sail-0.12/bin:$PATH"

command -v spike          >/dev/null || { echo "spike not on PATH"; exit 1; }
command -v sail_riscv_sim >/dev/null || { echo "sail_riscv_sim not on PATH"; exit 1; }
command -v riscv64-unknown-elf-gcc >/dev/null || { echo "riscv gcc not on PATH"; exit 1; }

CONFIG="${1:-spike-rv32-max}"
cd "$ROOT/third_party/riscv-arch-test"
echo "==> make ${CONFIG} (generate ELFs via Sail 0.12 + run on Spike)"
mise exec -- make "${CONFIG}"
