# ACT — userspace conformance for the purv engine

This is a real conformance check for purv's user-level RV32IMC + Zifencei
behaviour: it diffs purv's result signatures against **golden signatures
produced by an independent reference model** — [Spike], the official
`riscv-isa-sim` — over the [RISC-V Architecture Compatibility Test][act]
(riscv-arch-test) suite. The goldens are cooked once from Spike and vendored
here under `golden/`, so verifying purv needs only a RISC-V cross-assembler;
Spike is required only to *re*-cook them.

This is deliberately *not* purv-graded-by-purv: the reference is a different
implementation by a different author, so a match is evidence of correctness,
not of agreement with ourselves.

## Result

```
ACT RV32IMC userspace: PASS=76 FAIL=0 SKIP=12
```

| suite     | in scope | result |
|-----------|---------:|--------|
| I         |       39 | 39/39  |
| M         |        8 | 8/8    |
| C (Zca)   |       28 | 28/28  |
| Zifencei  |        1 | 1/1    |

## Running it

```sh
make -C ..            # build ../purv
./run.sh              # fetch the suite, assemble, run purv, diff vs goldens
```

`run.sh` fetches `riscv-arch-test` (tag `3.9.1`, pinned) into `.work/` the
first time, assembles each in-scope test with the vendored `model_test.h` +
`link.ld`, runs `purv --signature`, and diffs against `golden/<ext>/<name>.sig`.
Knobs: `CC=` (cross-assembler, default `riscv64-unknown-elf-gcc`), `PURV=`,
`ACTDIR=` (use an existing checkout), `ACT_TAG=`.

## How a test halts (no machine mode required)

Each test runs its body, stores every result word into a signature region, then
ends with `sw x1, tohost; 1: j 1b`. purv's host (`../main.c`) watches the store
to the `tohost` symbol, stops the run, and dumps `[begin_signature,
end_signature)` one big-endian word per line — the format Spike emits with
`+signature-granularity=4`. `tohost` is an ordinary memory store, so nothing
here exercises CSRs, traps, or privileged state: it is a faithful exercise of a
userspace-only engine.

## Regenerating the goldens (independent reference)

```sh
REGEN=1 SPIKE=/path/to/spike ./run.sh
```

The vendored goldens were cooked with Spike `1.1.1-dev`
(`riscv-software-src/riscv-isa-sim`), invoked per test as:

```
spike --isa=rv32i<m|c> +signature=<out> +signature-granularity=4 <test.elf>
```

## What is out of scope, and why

12 of the 88 rv32 C/I/M/Zifencei tests are skipped — none is a purv failure:

* **`C/cebreak-01`** declares `def rvtest_mtrap_routine=True` and `Zicsr`: its
  `c.ebreak` must trap to a machine-mode handler (`mtvec`/`mepc`/`mcause`/
  `mret`). That privileged machinery is exactly what a userspace engine omits
  by design, so the test cannot apply.
* **11 `Zcb` tests** (`c.mul`, `c.lbu`, `c.lh(u)`, `c.sb`, `c.sh`, `c.not`,
  `c.sext.b/.h`, `c.zext.b/.h`) declare `RV32..._Zca_Zcb[_Zbb]`. `Zcb` (and the
  `Zbb` two of them pull in) is a separate extension from base RVC (`Zca`);
  those opcodes do not even assemble under `rv32ic`. purv is RV32I**M**C, so
  they are a different ISA, not a missing behaviour.

## Vendored third-party files

`model_test.h` and `link.ld` are copied verbatim from Spike's
`arch_test_target/spike/` (Apache-2.0, © RISC-V International / Spike authors).
The `golden/*.sig` files are signatures computed by Spike over the Apache-2.0 /
BSD-3-Clause riscv-arch-test sources.

[Spike]: https://github.com/riscv-software-src/riscv-isa-sim
[act]: https://github.com/riscv-non-isa/riscv-arch-test
