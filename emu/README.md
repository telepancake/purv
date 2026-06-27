# purv — RISC-V emulator

A copy of [atoomnetmarc/RISC-V-emulator][atoom] flattened into a conventional
header + implementation library, with a separate host, ISA flags baked in:
**RV32IMC + Zicsr + Zifencei**.

```
emu/
  purv.h      public interface — opaque state + the calls you need (~50 lines)
  purv.c      the whole engine, hidden behind purv.h (generated, ~4000 lines)
  main.c      the host/driver: memory map + hooks + ELF loader + two run modes
  runfn.sh    compile a bare function and run it, wasm style
  tools/      flatten.py — regenerates purv.c from the upstream submodule
  examples/   fn.c (bare functions), sigtest.S (signature/tohost demo)
```

`purv.h` is the entire public surface: the VM state is an **opaque type**, so
none of the engine's internal instruction-format / CSR structs leak out. You
`#include "purv.h"`, create a state, call `RiscvEmulatorLoop()` to step it, and
read registers through accessors. The implementation — every instruction body —
lives in `purv.c` and is invisible to callers. `main.c` is just one host; write
your own and link against the engine.

atoom's appeal is that it has essentially *no API*: you define a handful of hook
functions and the engine reaches your memory map only through them (defined in
`main.c` here):

```c
void RiscvEmulatorLoad (uint32_t addr, void *dst, uint8_t len);
void RiscvEmulatorStore(uint32_t addr, const void *src, uint8_t len);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *);
```

## Build

```sh
make            # builds ./purv with gcc (CC=clang also works)
make test       # builds + runs the examples
```

## Run a bare function (wasm style)

No cross-gcc required — clang targets riscv32 directly and lld links it.

```sh
./runfn.sh examples/fn.c square --arg=7      # => 49 (0x00000031)
./runfn.sh examples/fn.c fib    --arg=20     # => 6765 (0x00001a6d)
```

Under the hood: `clang --target=riscv32 -march=rv32imc` compiles the function,
`ld.lld --image-base=0x80000000` links it, and `purv --invoke=SYM` sets `ra` to
a sentinel, loads args into `a0..`, runs until the function returns, and prints
`a0`.

## Run userspace code (syscalls)

```sh
purv --user <elf> [program args...]    # ecall -> Linux/RISC-V syscall
```

In `--user` mode the host sets up a real initial stack (`argc`, `argv`, an empty
`envp`, and an `AT_NULL` auxv) from the trailing command-line args, then an
`ecall` is delivered as a syscall (`a7`=number, `a0..`=args, result in `a0`)
instead of trapping to `mtvec`. `main.c` implements `write`, `exit`, and `brk`;
everything else returns `-ENOSYS`. Examples:

```sh
make examples/hello.elf && ./purv --user examples/hello.elf
# hello from riscv userspace
make examples/args.elf  && ./purv --user examples/args.elf one two
# argc=3 / args.elf / one / two
```

This is the design pattern for purv: the engine stays minimal and the host owns
policy. The engine exposes just one new primitive — `RiscvEmulatorClearTrap()` —
which the host's `RiscvEmulatorHandleECALL` calls to consume the `ecall` and
resume, rather than the engine baking in any syscall ABI. Without `--user`,
`ecall` traps normally, so machine-mode conformance is unaffected.

## Run a conformance test (RISCOF DUT contract)

```sh
purv [--signature=FILE [--signature-granularity=4]] <elf>
```

Loads the ELF, runs from its entry point until the program signals completion —
a store to the `tohost` symbol (RISCOF) or a `0x5555` poweroff write to SYSCON
at `0x11100000` (mini-rv32ima / ACT4) — then dumps `[begin_signature,
end_signature)` as one hex word per line. Exit status is `0` on PASS.

This matches the CLI that `conformance/plugin-purv` already expects, so purv can
drop straight into the RISCOF harness once a `riscv*-gcc` cross-compiler and the
Sail reference model are available.

## Running the real conformance suite

```sh
make conformance        # ./selfcheck.sh rv32ui rv32um rv32uc
```

`selfcheck.sh` fetches the classic **riscv-tests** ISA suite, builds each test
for rv32imc with **clang** (`--target=riscv32` + `ld.lld` — no cross-gcc), and
runs it on purv. These tests are **self-checking**: each embeds its expected
results and writes pass/fail to the HTIF `tohost` word, so **no reference model
is needed**. purv decodes that word (`tohost==1` -> pass) into its exit code.

It only needs network egress (to fetch the suite) and clang. Last measured here:
rv32ui 41/41, rv32um 8/8, rv32uc 1/1; `rv32ui/ma_data` is excluded and
`rv32mi/mcsr` fails (machine-CSR gap — see below).

**Optional reference (Spike).** If `spike` is on `PATH`, `selfcheck.sh` uses it
only to skip tests Spike itself can't pass under this config (e.g. `ma_data`).
Build it once from the pinned submodule (a C++ host program — no cross-toolchain):

```sh
apt-get install -y device-tree-compiler
git submodule update --init third_party/riscv-isa-sim        # or fetch the tarball
( cd third_party/riscv-isa-sim && mkdir -p build && cd build && ../configure && make -j )
export PATH="$PWD/third_party/riscv-isa-sim/build:$PATH"
```

**Note on the pinned ACT4 suite.** `third_party/riscv-arch-test` is the newer
ACT4 framework. Building *its* ELFs needs a UDB-generated `rvtest_config.h`
(ruby/mise) and Sail to bake reference signatures — heavy, and this Spike build
has no `+signature` flag to diff against. `selfcheck.sh` (riscv-tests,
self-checking) is the lightweight path that runs anywhere with just clang; reach
for the ACT4 framework only when you need its exhaustive coverage.

## Memory map

| region | address      | behaviour                                  |
|--------|--------------|--------------------------------------------|
| RAM    | `0x80000000` | flat backing array (`--ram=` bytes)        |
| UART   | `0x10000000` | byte store → stdout; `+5` reads tx-ready   |
| SYSCON | `0x11100000` | store `0x5555` → exit 0 (PASS)             |

## Regenerating the engine

`purv.c` is generated, not hand-edited (`purv.h` and `main.c` are hand-written).
`tools/flatten.py` inlines atoom's ~45 headers in dependency order, evaluating
the baked `RVE_E_*` flags so disabled extensions and the `RVE_E_HOOK`
instrumentation are stripped out entirely. It emits the internal types and every
instruction body as plain `static` functions into `purv.c`, turns atoom's state
typedef into the tagged `struct RiscvEmulatorState` that completes the opaque
type, and exposes only the public API.

```sh
make regen      # reads ../third_party/atoomnetmarc-rv/include
```

Pinned to atoom commit `633526d4`.

[atoom]: https://github.com/atoomnetmarc/RISC-V-emulator
