#!/usr/bin/env bash
#
# setup.sh — reproducible record of the conformance environment setup.
#
# This captures the VERIFIED steps from the bring-up session. It is the modern
# ACT4 path (riscv-arch-test's RISCOF replacement). See conformance/STATUS.md
# for the full picture and what's still TODO.
#
# Idempotent where practical. Run from the repo root.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
export DEBIAN_FRONTEND=noninteractive

# --- 1. Submodules (fetched over HTTPS; the injected git proxy is repo-scoped) -
echo "==> submodules"
if ! git submodule update --init --recursive; then
  GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
  GIT_SSL_CAINFO="${GIT_SSL_CAINFO:-/root/.ccr/ca-bundle.crt}" \
    git submodule update --init --recursive
fi

# --- 2. RISC-V cross toolchain + dtc  [VERIFIED] -------------------------------
echo "==> riscv toolchain"
apt-get update -qq
apt-get install -y -qq gcc-riscv64-unknown-elf device-tree-compiler make git
riscv64-unknown-elf-gcc --version | head -1   # expect 13.2.0 on Ubuntu 24.04

# --- 3. Prebuilt Sail 0.12 (the reference model ACT4 consumes)  [VERIFIED] -----
# NOT a from-source build of the sail-riscv submodule HEAD — ACT4 wants 0.12.
echo "==> prebuilt Sail 0.12"
if [ ! -x tools/sail-0.12/bin/sail_riscv_sim ]; then
  mkdir -p tools/sail-0.12
  curl -sSL -o /tmp/sail012.tgz \
    "https://github.com/riscv/sail-riscv/releases/download/0.12/sail-riscv-$(uname)-$(arch).tar.gz"
  tar xzf /tmp/sail012.tgz --directory=tools/sail-0.12 --strip-components=1
fi
tools/sail-0.12/bin/sail_riscv_sim --version   # expect 0.12

# --- 4. Spike (DUT / differential oracle)  [VERIFIED] --------------------------
echo "==> spike"
if [ ! -x third_party/riscv-isa-sim/build/spike ]; then
  ( cd third_party/riscv-isa-sim && mkdir -p build && cd build && ../configure && make -j"$(nproc)" )
fi
third_party/riscv-isa-sim/build/spike --help >/dev/null 2>&1 && echo "spike OK"

# --- 5. ACT4 framework  [TODO — not yet verified this session] -----------------
# Recommended (handles uv/Python + Ruby + UDB gem automatically):
#   curl https://mise.jdx.dev/install.sh | sh
#   eval "$("$HOME/.local/bin/mise" activate bash)"
#   ( cd third_party/riscv-arch-test && mise install )
#
# Or without mise (you supply Python 3.10+, Ruby, Bundler, riscv-unified-db):
#   ( cd third_party/riscv-arch-test && \
#       pip install -e ./framework -e ./generators/testgen -e ./generators/coverage )
#
# Then, from third_party/riscv-arch-test, generate self-checking ELFs with the
# Sail 0.12 model and run them on Spike via the built-in targets:
#   make spike-<ext>      # see `make help` / README for exact target names
echo "==> ACT4 framework: see conformance/STATUS.md (step 5, TODO)"

echo "Setup complete through step 4. ACT4 framework install is the next step."
