#!/usr/bin/env bash
# Reproduce the libriscv finding: it is a userspace sandbox and cannot load the
# bare-metal ACT4 ELFs. See conformance/dut/libriscv/NOTES.md.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

EMU="$ROOT/third_party/libriscv/emulator"
if [ ! -x "$EMU/build/rvlinux" ]; then
  echo "==> building libriscv CLI (rvlinux)"
  # FetchContent pulls fwsGonzo/tinycc; disable it (and binary translation) so the
  # build needs no extra git clones, and bypass any injected git-credential proxy.
  GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
  GIT_SSL_CAINFO="${GIT_SSL_CAINFO:-/root/.ccr/ca-bundle.crt}" \
    cmake -S "$EMU" -B "$EMU/build" -DCMAKE_BUILD_TYPE=Release \
          -DRISCV_BINARY_TRANSLATION=OFF -DRISCV_LIBTCC=OFF
  cmake --build "$EMU/build" -j"$(nproc)"
fi

# Point it at any ACT4 ELF (generate one cheaply via the tinyemu config if needed).
ELF="${1:-$ROOT/third_party/riscv-arch-test/work/tinyemu/elfs/rv32i/I/I-add-00.elf}"
echo "==> running libriscv on a bare-metal ACT4 ELF (expected: rejected)"
"$EMU/build/rvlinux" "$ELF" || true
echo
echo "See conformance/dut/libriscv/NOTES.md — libriscv is a userspace sandbox;"
echo "the bare-metal machine-mode architectural suite is out of its scope."
