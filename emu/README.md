# purv — single-header RISC-V emulator

A two-file copy of [atoomnetmarc/RISC-V-emulator][atoom] flattened into one
header plus one reference host, with the ISA flags baked in:
**RV32IMC + Zicsr + Zifencei**.

```
emu/
  purv.h      the whole emulator engine, one header, flags baked, dead branches stripped
  purv.c      the reference host: memory map + hooks + ELF loader + two run modes
  runfn.sh    compile a bare function and run it, wasm style
  tools/      flatten.py — regenerates purv.h from the upstream submodule
  examples/   fn.c (bare functions), sigtest.S (signature/tohost demo)
```

atoom's appeal is that it has essentially *no API*: you `#include` the engine
and define a handful of hook functions for your memory map. `purv.h` keeps that.
The engine reaches the outside world only through these, all defined in `purv.c`:

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

## Memory map

| region | address      | behaviour                                  |
|--------|--------------|--------------------------------------------|
| RAM    | `0x80000000` | flat backing array (`--ram=` bytes)        |
| UART   | `0x10000000` | byte store → stdout; `+5` reads tx-ready   |
| SYSCON | `0x11100000` | store `0x5555` → exit 0 (PASS)             |

## Regenerating the header

`purv.h` is generated, not hand-edited. `tools/flatten.py` inlines atoom's ~45
headers in dependency order, evaluating the baked `RVE_E_*` flags so disabled
extensions and the `RVE_E_HOOK` instrumentation are stripped out entirely
(8020 → ~4000 lines).

```sh
make regen      # reads ../third_party/atoomnetmarc-rv/include
```

Pinned to atoom commit `633526d4`.

[atoom]: https://github.com/atoomnetmarc/RISC-V-emulator
