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
  gdbstub.c   optional GDB Remote Serial Protocol server (build with GDB=1)
  gdbserve.py launcher that brokers a gdb connection and hands purv the fd
  tools/      flatten.py — regenerates purv.c from the upstream submodule
  examples/   fn.c (bare functions), sigtest.S (signature demo), loop.c (gdb reverse-exec)
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

## Debug with gdb

The engine has an optional GDB stub, compiled in only when you ask for it:

```sh
make GDB=1                 # builds ./purv with the gdb stub (adds --gdb=FD)
```

purv never opens a socket itself — it serves the GDB Remote Serial Protocol on a
**file descriptor handed to it** (`--gdb=FD`), so connection setup is entirely the
launcher's business. `gdbserve.py` is that launcher: it listens, accepts one gdb,
and execs purv with the connected socket as the fd. The guest's console output
stays on your terminal; only the protocol rides the socket.

```sh
./gdbserve.py 1234 -- ./purv --user examples/hello.elf
```

Then, from a riscv-capable gdb (`gdb-multiarch`, or a `riscv*-elf-gdb`):

```sh
gdb-multiarch examples/hello.elf -ex 'target remote :1234'
(gdb) break _start
(gdb) continue
(gdb) info registers
(gdb) stepi
```

The stub serves a `riscv:rv32` target description (the 33 registers `x0..x31`,
`pc`, with ABI names so `$sp`/`$ra`/`$a0` resolve), register and memory read/write,
single-step, continue, software breakpoints (`Z0`/`z0`, tracked in the stub rather
than by patching guest code), Ctrl-C to interrupt a run, and a program exit
reported back to gdb. It drives the engine purely through `purv.h` — stepping with
`RiscvEmulatorLoop`, reaching guest memory through the same `RiscvEmulatorLoad`/
`Store` hooks — so the generated engine is untouched. `--gdb=FD` composes with the
other modes (`--user`, plain run-to-halt): gdb just takes over execution.

### Reverse execution (time travel)

```sh
(gdb) reverse-stepi      # step one instruction backward
(gdb) reverse-continue   # run backward to the previous breakpoint
```

gdb's own `record full` does **not** support riscv (`the current architecture
doesn't support record function`), so a remote stub is the *only* way to reverse-
debug riscv in gdb — and an emulator is the natural place for it. The stub keeps a
per-instruction undo history: before each step it snapshots the register file, and
the host feeds it the bytes each store overwrites (`RiscvEmulatorGdbRecordStore`,
the one line added to the host's store hook). Reversing puts the registers back
and replays those bytes, so **both registers and memory unwind exactly** — a
forward run that fills an array and a reverse run that empties it land on
identical state at every step. History is a bounded ring (~64k instructions); past
the oldest recorded step, reverse is a safe no-op. The engine itself stays
unmodified — it never knows it is being recorded.

```sh
make examples/loop.elf
./gdbserve.py 1234 -- ./purv --user examples/loop.elf   # a loop that writes memory
```

Most other gdb features that aren't native here degrade gracefully: hardware
watchpoints fall back to gdb's software watchpoints, `vCont` to plain `c`/`s`,
conditional breakpoints are evaluated gdb-side, and ack-mode simply stays on.

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
