#!/usr/bin/env bash
#
# reproduce.sh — one command, fresh checkout -> conformance run on Spike.
#
# Chains the three verified stages. Each is idempotent, so re-running is cheap.
# Heavy the first time: clones submodules, builds Spike, downloads the prebuilt
# Sail 0.12 model (~3 MB) and the RISC-V GCC 16 toolchain (~550 MB), and installs
# the ACT4 framework (mise/uv + Ruby). See conformance/STATUS.md for details.
#
# Usage:
#   scripts/reproduce.sh                 # default config: spike-RVI20U32 (clean green)
#   scripts/reproduce.sh spike-rv32-max  # full max-ISA config (7 known exotic fails)
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-spike-RVI20U32}"

echo "######## 1/3  environment + emulators (setup.sh) ########"
scripts/setup.sh

echo "######## 2/3  ACT4 framework (install-act4.sh) ########"
scripts/install-act4.sh

echo "######## 3/3  conformance run: ${CONFIG} (run-spike.sh) ########"
scripts/run-spike.sh "${CONFIG}"
