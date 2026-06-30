# purvs — monadic purv (decode-ahead + pluggable threaded eval)

A restructuring of purv that separates **decoding** from **execution**, so the
value semantics become a swappable plug rather than something baked into the
interpreter loop. purv's inner loop fuses fetch, decode, and execute; purvs cuts
that loop in two:

```
purvs/
  purvs.c, purvs.h   the engine: decode-ahead + the default threaded evaluator
  main.c             the host/driver (shared with purv: ELF loader, syscalls,
                     conformance signature dump). Identical to purv's main.c.
```

## The two halves

1. **Decode-ahead.** From the current `pc`, the engine walks the code window and
   lowers each raw instruction — 32-bit or compressed — into a wide
   `RiscvEmulatorDecoded_t` in an internal buffer, stopping at the first control
   transfer (kept as the run's last record) or when the buffer / code window is
   exhausted. Decode reads only the code bytes: it records register **indices**
   and immediates, never register **values** (those change as earlier
   instructions in the run execute). Because code lives in the read-only lower
   half, nothing execution does can invalidate a decoded run.

2. **Threaded execution.** A pluggable evaluator (`state->eval`, default
   `RiscvEmulatorDefaultEval`) threads through the decoded buffer with a
   **computed-goto jump table** indexed by the record's leaf op. Each handler
   ends by dispatching the next record directly ("token threading"), with no
   loop-top round-trip.

## Our opcodes ≠ their opcodes

The decoded `op` is **our** flat leaf opcode, one per evaluator handler (`ADD`,
`SUB`, `LB`, `BEQ`, …) — there is deliberately no 1:1 correspondence with the
RISC-V encoding. Decode folds the base opcode, `funct3`, and `funct7` into that
single byte, so the evaluator dispatches in one indexed jump with **no secondary
decode**. A few operand-source fields remain on the record (`rs1`/`rs2`/`rd`,
`imm`, and `b_imm` — whether an ALU op's second operand is the immediate or
`x[rs2]`); everything else is encoded by the op itself.

| record field | meaning |
|--------------|---------|
| `op`         | our leaf opcode (`RISCV_OP_*`), the jump-table index |
| `rd/rs1/rs2` | register indices |
| `imm`        | immediate / shift amount, pre-extended for the op |
| `b_imm`      | ALU: second operand is `imm` (1) or `x[rs2]` (0) |
| `width`      | encoded length (2 or 4), for `pc += width` |
| `ends_block` | this record ends the decoded run (control transfer / trap) |

## The pluggable evaluator

```c
typedef uint32_t (*RiscvEmulatorEvalFn)(RiscvEmulatorState_t *state,
                                        const RiscvEmulatorDecoded_t *block,
                                        uint32_t count, int *halt);
```

`eval` is handed the whole decoded run and executes it in order, leaving
`state->pc` at the next instruction and returning the number of records it ran;
it sets `*halt` to stop the engine (a terminal `ecall`, a breakpoint, an illegal
instruction). It is the **one place value semantics live** — the default does
ordinary RV32IMC computation, and an alternative (tag/provenance tracking, taint,
tracing, symbolic execution, …) is a drop-in replacement that reuses the same
decode pass and run loop. `RiscvEmulatorInit` installs the default; assign
`state->eval` to swap it.

The earlier tagged-memory purv (a per-register provenance datapath) is the
natural first alternate evaluator to layer back on top of this split.

## Demo

`make test` builds the engine and runs the same example programs purv runs
(wasm-style function calls, a userspace program over `ecall` syscalls, and a
conformance signature dump):

```sh
make test
```

Conformance: the engine passes the RV32IMC userspace ACT suite (the same
`emu/act` runner purv uses) — point it at this binary with
`PURV=purvs/purvs ./act/run.sh`.

## Notes / limits

- Execution uses GCC/Clang computed gotos (labels-as-values) for the threaded
  dispatch; both toolchains the project uses support it.
- A decoded run ends at the first control transfer, so a conditional branch,
  jump, or trap is always the run's last record. Long straight-line code is
  decoded in successive buffer-sized batches.
