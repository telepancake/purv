#!/usr/bin/env bash
# Get the Sail reference model that ACT4 consumes.
#
# IMPORTANT: ACT4 is "currently compatible with version 0.12 of the model" and
# uses a PREBUILT 0.12 binary. Do NOT build the sail-riscv submodule HEAD from
# source — that is the new CMake single-binary build with a different CLI and a
# different model version. This script downloads the prebuilt 0.12 release.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools/sail-0.12
curl -sSL -o /tmp/sail012.tgz \
  "https://github.com/riscv/sail-riscv/releases/download/0.12/sail-riscv-$(uname)-$(arch).tar.gz"
tar xzf /tmp/sail012.tgz --directory=tools/sail-0.12 --strip-components=1
tools/sail-0.12/bin/sail_riscv_sim --version
echo "Sail 0.12 -> tools/sail-0.12/bin/sail_riscv_sim"
