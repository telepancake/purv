#!/usr/bin/env bash
# Generate ACT4 self-checking ELFs (rv32im, expected results from Sail 0.12) and
# run them on cnlohr/mini-rv32ima via our ACT4 runner.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
DUT="$ROOT/conformance/dut/mini-rv32ima"

# Build the runner (embeds third_party/mini-rv32ima/mini-rv32ima/mini-rv32ima.h).
mkdir -p tools/bin
gcc -O2 -Wall -I third_party/mini-rv32ima/mini-rv32ima \
    -o tools/bin/mini-rv32ima-runner "$DUT/runner.c"

export PATH="$HOME/.local/bin:$PATH"
eval "$(mise activate bash)" 2>/dev/null || true
export PATH="$ROOT/tools/riscv-gcc/bin:$ROOT/tools/sail-0.12/bin:$ROOT/tools/bin:$PATH"

cd "$ROOT/third_party/riscv-arch-test"
echo "==> generate rv32im ELFs (Sail 0.12 reference)"
mise exec -- make elfs CONFIG_FILES="$DUT/test_config.yaml"
echo "==> run on mini-rv32ima"
mise exec -- ./run_tests.py "mini-rv32ima-runner" work/mini-rv32ima/elfs
