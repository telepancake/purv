#!/usr/bin/env bash
# runfn.sh — compile a bare RISC-V function and run it, wasm style.
#
#   ./runfn.sh <source.c> <symbol> [--arg=N ...]
#
# Example:
#   ./runfn.sh examples/fn.c square --arg=7      # -> 49 (0x00000031)
#
# Compiles <source.c> for rv32imc with clang (no cross-gcc needed), links it with
# code in the read-only lower half at 0 (examples/purv.ld), then invokes <symbol>
# in the purv emulator and prints the returned value (a0).
set -euo pipefail
cd "$(dirname "$0")"

src=${1:?usage: runfn.sh <source.c> <symbol> [--arg=N ...]}
sym=${2:?missing symbol}
shift 2

GUESTCC=${GUESTCC:-clang}
GUEST_ARCH=${GUEST_ARCH:---target=riscv32 -march=rv32imc -mabi=ilp32}
GUEST_CFLAGS=${GUEST_CFLAGS:--ffreestanding -nostdlib -fno-builtin -fno-stack-protector -O2 -Wall -Wextra -Werror -Wpedantic}
LD=${LD:-ld.lld}
LDFLAGS=${LDFLAGS:--m elf32lriscv -T examples/purv.ld}

[ -x ./purv ] || make purv >/dev/null

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
$GUESTCC $GUEST_ARCH $GUEST_CFLAGS -c "$src" -o "$tmp/o.o"
$LD $LDFLAGS --entry=0 "$tmp/o.o" -o "$tmp/a.elf"
exec ./purv --invoke="$sym" "$@" "$tmp/a.elf"
