# purv — RISC-V emulator

A small, hand-written RISC-V emulator in the spirit of
[atoomnetmarc/RISC-V-emulator][atoom] — same hook-based premise, a conventional
header + implementation library, a separate host, ISA flags baked in:
**RV32IMC + Zifencei**. It is a *userspace* engine — a program runner, not a
machine: no CSRs, no privileged modes, no trap-to-`mtvec`. `ecall` (a syscall),
`ebreak` (a debugger trap), and an illegal instruction are handed straight to
the handlers you put in the state; nothing vectors anywhere.

```
emu/
  purv.h      public interface — the state struct + the calls you need (~90 lines)
  purv.c      the whole engine behind purv.h (hand-written, ~310 lines)
  act/        userspace ACT conformance vs Spike golden signatures (make act)
  purg.c/.h   the earlier engine, mechanically generated from atoom (g = generated;
              kept for reference, not built — see "How the engine is organized")
  main.c      the host/driver: memory map + handlers + ELF loader + two run modes
  runfn.sh    compile a bare function and run it, wasm style
  gdbstub.c   optional GDB Remote Serial Protocol server (build with GDB=1)
  gdbserve.py launcher that brokers a gdb connection and hands purv the fd
  tools/      flatten.py / inline.py / compact.py — the atoom-distilling pipeline (history)
  examples/   fn.c (bare functions), sigtest.S (signature demo), loop.c (gdb reverse-exec)
```

`purv.h` is the entire public surface: the VM state is a **plain struct** you
read and write directly (`pc`, `x[32]`, the memory map, the handlers). The
implementation — every instruction body — lives in `purv.c`. `main.c` is just one
host; write your own and link against the engine.

The engine is **self-contained: no link-time host hooks**. The state struct is
public, so you set it up by writing its fields -- there are no trivial get/set
wrappers. `RiscvEmulatorInit` zeroes the struct and wires the two regions every
program needs (the code window and the stack); you then map any data regions,
assign the handlers, and run a batch:

```c
RiscvEmulatorState_t st;                       /* allocate it yourself (stack or heap) */
RiscvEmulatorRegion_t code  = { ram,   ram_len,   /*writable=*/1 };  /* fetch window */
RiscvEmulatorRegion_t stack = { stkbuf, stk_len,  /*writable=*/1 };  /* -> region 7 */
RiscvEmulatorInit(&st, code, stack);            /* sets st.code, st.mem[7], sp, pc */
st.mem[0]  = (RiscvEmulatorRegion_t){ ram, ram_len, 1 };  /* data: region 0 */
st.ecall   = on_ecall;          /* int (*)(state): nonzero -> stop the run loop */
st.ebreak  = on_ebreak;
st.illegal = on_illegal;
st.pc      = entry;                             /* defaults to 0x80000000 */
uint64_t ran = RiscvEmulatorLoop(&st, max);     /* runs a batch of up to `max` */
```

The address space is **8 evenly spaced regions** (`st->mem[0..7]`), each up to
256 MiB, based at `0x80000000`, `0x90000000`, … `0xF0000000`. A data load from an
unmapped/out-of-bounds address reads zero; a store to a read-only or
out-of-bounds address is dropped. Instruction *fetch* comes from `st.code` (a
separate window based at `0x80000000`, never one of the data regions); a pc
outside it ends the batch. The **stack is always region 7** (`st.mem[7]`,
`0xF0000000`): `RiscvEmulatorInit` takes it as its second argument, points
`mem[7]` at it, and sets `sp` to the top of that region. `ecall`/`ebreak`/`illegal`
are the only handlers, and each returns nonzero to stop the run loop. The engine
exposes essentially two functions -- `RiscvEmulatorLoop` and `RiscvEmulatorInit`;
you allocate the state, write its fields, and to touch guest memory from outside
you index `st.mem[]` yourself. The struct is also naturally sized per target --
smaller on a 32-bit host, where its pointers are 4 bytes -- and the engine builds
and runs on both.

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
`ecall` is delivered as a syscall (`a7`=number, `a0..`=args, result in `a0`).
`main.c` implements `write`, `exit`, and `brk`; everything else returns
`-ENOSYS`. Examples:

```sh
make examples/hello.elf && ./purv --user examples/hello.elf
# hello from riscv userspace
make examples/args.elf  && ./purv --user examples/args.elf one two
# argc=3 / args.elf / one / two
```

This is the design pattern for purv: the engine stays minimal and the host owns
policy. The engine bakes in no syscall ABI — an `ecall` simply calls the handler
in `st->ecall`, which services the call and returns 0 to resume in place (it is a
service request to the execution environment, so it returns straight to the
caller). Without `--user` that handler ignores it.

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
`RiscvEmulatorLoop` (one instruction at a time), reaching guest memory by walking
`state->mem[]` — so the engine is untouched. `--gdb=FD` composes with the
other modes (`--user`, plain run-to-halt): gdb just takes over execution.

### Reverse execution (time travel)

```sh
(gdb) reverse-stepi      # step one instruction backward
(gdb) reverse-continue   # run backward to the previous breakpoint
```

gdb's own `record full` does **not** support riscv (`the current architecture
doesn't support record function`), so a remote stub is the *only* way to reverse-
debug riscv in gdb — and an emulator is the natural place for it. The stub keeps a
per-instruction undo history: before each step it snapshots the register file, so
reversing puts the registers back exactly. History is a bounded ring (~64k
instructions); past the oldest recorded step, reverse is a safe no-op.

> **Note:** with the self-contained memory model the engine performs stores
> internally — there is no store hook for the stub to record overwritten bytes
> through — so reverse execution currently unwinds **registers only**, not
> memory. (`RiscvEmulatorGdbRecordStore` is retained for when a store-observation
> path is reintroduced.)

```sh
make examples/loop.elf
./gdbserve.py 1234 -- ./purv --user examples/loop.elf   # a loop that writes memory
```

The stub also speaks `vCont` (gdb's modern step/continue), `QStartNoAckMode` (drops
the `+`/`-` handshake once negotiated), and `T` stop replies that expedite `pc`/`sp`
so gdb needn't re-read the whole register file on every stop. The remaining gdb
features that aren't native here degrade gracefully: hardware watchpoints fall back
to gdb's software watchpoints, and conditional breakpoints are evaluated gdb-side.

## Run a conformance test (RISCOF DUT contract)

```sh
purv [--signature=FILE [--signature-granularity=4]] <elf>
```

Loads the ELF, runs from its entry point until the program signals completion —
a store to the `tohost` symbol, which the host polls between run slices — then
dumps `[begin_signature, end_signature)` as one hex word per line. Exit status is
`0` on PASS.

This matches the CLI that `conformance/plugin-purv` already expects, so purv can
drop straight into the RISCOF harness given a `riscv*-gcc` cross-compiler.

## Running the real conformance suite

```sh
make act        # ./act/run.sh — userspace RV32IMC vs Spike golden signatures
```

The real correctness check is the **RISC-V Architecture Compatibility Test**
(riscv-arch-test): each test is assembled, run on purv, and its result signature
diffed against a golden signature **cooked from an independent reference model**
([Spike], the official `riscv-isa-sim`). The 76 in-scope goldens are vendored
under `act/golden/`, so verifying needs only a RISC-V cross-assembler — Spike is
needed only to regenerate them. This is *not* purv-graded-by-purv: the reference
is a different implementation, so a match is evidence of correctness.

```
ACT RV32IMC userspace: PASS=76 FAIL=0 SKIP=12
```

The 12 skips are out of scope, not failures: `cebreak-01` needs a machine-mode
trap routine (Zicsr/`mtvec`/`mret`), and 11 `Zcb` tests (`c.mul`/`c.lbu`/
`c.sext.b`/…) are a separate extension that does not assemble under `rv32ic`.
See `act/README.md` for provenance, scope, and how the goldens were cooked.

Because purv is userspace-only, the classic machine-mode **riscv-tests** path
(`selfcheck.sh`, which relies on `mtvec`/`mret`/HTIF trap setup) no longer
applies; ACT is the conformance path.

## Memory map

The engine's address space is 8 regions of up to 256 MiB, based 256 MiB apart
from `0x80000000`; the host maps host storage into them by writing `st->mem[i]`.

| region | base address | `main.c` use                                |
|-------:|--------------|---------------------------------------------|
| 0      | `0x80000000` | flat RAM backing array (`--ram=` bytes), writable |
| 1..6   | `0x90000000`…`0xE0000000` | unmapped (reads 0, stores dropped) |
| 7      | `0xF0000000` | user stack (`--user`), writable; `sp` starts at its top |

There is no MMIO: console output is the `write` syscall (`--user`) and a
conformance test halts by writing the `tohost` word, which the host polls (both
live in region 0). Stores to a read-only or unmapped/out-of-bounds address are
silently dropped; such loads read zero.

## How the engine is organized

`purv.c` is hand-written (so are `purv.h` and `main.c`). It keeps atoom's premise
— the host owns the memory map; the engine reaches the world only through the
hooks above — but trims the redundancy of the original, in ~310 lines. Two ideas
do most of that:

- **Registers are indices into `x[32]`, not pointers.** `x0` is never written, so
  it reads as a hard zero and `wr()` simply drops writes to it. No `void *rd`
  juggling, no per-instruction `x0` guard.
- **One executor for both encodings, no intermediate decoded form.** Each opcode
  arm — 32-bit or compressed — decodes only the operands it needs straight from
  the instruction into a few locals, then `goto`s a shared action body
  (`alu`/`load`/`store`/`branch`/…). So the compressed and base encodings reuse
  the *same* execution code without decompressing into a struct first, and no
  instruction decodes a field it won't use. Reserved/illegal encodings `goto
  illegal`, which calls the host's illegal hook.

`RiscvEmulatorLoop(state, max)` runs a *batch* of up to `max` instructions in one
call — the fetch → decode → execute step is the body of an internal loop, so there
is no per-instruction call boundary — and returns how many it ran. The pc lives in
the state; instructions are fetched from `state->code`, data goes through the
mapped regions (`state->mem[]`).

Correctness is **verified against an independent reference** — the ACT suite
diffed against Spike golden signatures (`make act`, 76/76 in scope; see above and
`act/README.md`).

The earlier engine — `purg.c` / `purg.h` (purg = "generated") — is kept as a
reference and is not part of the build. The `tools/` pipeline (`flatten.py` /
`inline.py` / `compact.py`) and the `atoomnetmarc-rv` submodule (pinned to commit
`633526d4`) mechanically distil the upstream engine into it with the privileged
`Zicsr` surface baked off (`RVE_E_ZICSR=0`), so it too is userspace-only; `make
regen` rebuilds `purg.c` from the submodule (it never touches the hand-written
`purv.c` / `purv.h`). `purg`'s public API predates this rewrite — its
`RiscvEmulatorLoop(state)` steps an internal pc rather than taking/returning one.

[atoom]: https://github.com/atoomnetmarc/RISC-V-emulator
[Spike]: https://github.com/riscv-software-src/riscv-isa-sim
