#!/usr/bin/env bash
# Generate ACT4 self-checking ELFs (rv32imc, Sail 0.12 reference) and run them
# on TinyEMU. TinyEMU is a full-system emulator: we load each ELF as the "bios"
# and use its HTIF (0x40008000) for console + exit (see the DUT rvmodel_macros).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
DUT="$ROOT/conformance/dut/tinyemu"

# Build temu (no SDL/network).
if [ ! -x third_party/tinyemu/temu ]; then
  make -C third_party/tinyemu CONFIG_FS_NET= CONFIG_SDL= temu
fi

# Launcher: wrap each ELF in a minimal riscv32 config and run temu.
mkdir -p tools/bin
cat > tools/bin/tinyemu-run <<EOF
#!/usr/bin/env bash
set -e
elf="\$1"
cfg="\$(mktemp /tmp/temu-XXXXXX.cfg)"
printf '{ version: 1, machine: "riscv32", memory_size: 256, bios: "%s" }\n' "\$elf" > "\$cfg"
"$ROOT/third_party/tinyemu/temu" "\$cfg" </dev/null
rc=\$?
rm -f "\$cfg"
exit \$rc
EOF
chmod +x tools/bin/tinyemu-run

export PATH="$HOME/.local/bin:$PATH"
eval "$(mise activate bash)" 2>/dev/null || true
export PATH="$ROOT/tools/riscv-gcc/bin:$ROOT/tools/sail-0.12/bin:$ROOT/tools/bin:$PATH"

cd "$ROOT/third_party/riscv-arch-test"
echo "==> generate rv32imc ELFs (Sail 0.12 reference)"
mise exec -- make elfs CONFIG_FILES="$DUT/test_config.yaml"
echo "==> run on TinyEMU"
mise exec -- ./run_tests.py "tinyemu-run" work/tinyemu/elfs
