# Third-party emulator DUTs

ACT4 adapters that run the conformance suite on external RISC-V emulators. Each
DUT directory is self-contained and lives **outside** the pinned `riscv-arch-test`
submodule, so generation is driven with `make elfs CONFIG_FILES=<dir>/test_config.yaml`
and execution with `run_tests.py "<runner>" work/<name>/elfs` (see the
`scripts/run-*.sh` wrappers).

## How an adapter works

ACT4 self-checking ELFs need a DUT that (1) executes the ELF, (2) prints
`RVMODEL_IO_WRITE_STR` text — including `RVCP-SUMMARY: TEST PASSED/FAILED` — to a
console, and (3) exits 0 on `RVMODEL_HALT_PASS` / non-zero on `RVMODEL_HALT_FAIL`
(`run_tests.py` requires **both** exit 0 **and** a `TEST PASSED` summary line).

Neither emulator speaks HTIF, so each adapter provides:
- a small C **runner** (`runner.c`) — ELF loader + flat RAM at `0x80000000` +
  a UART at `0x10000000` (→ stdout) + a SYSCON at `0x11100000`
  (`0x5555` → exit 0, else exit 1);
- `rvmodel_macros.h` — `HALT_PASS/FAIL` via SYSCON, `IO_WRITE_STR` via the UART;
- `link.ld`, a UDB config (test selection), and a `sail.json` (the Sail 0.12
  reference, configured to the DUT's ISA).

## Results

Run: `scripts/run-mini-rv32ima.sh`, `scripts/run-atoomnetmarc.sh`.

### cnlohr/mini-rv32ima — RV32IM (+Zicsr), **53/61**

Standalone `rv32ima` emulator (A and Zifencei stubbed, no C). Scoped to RV32IM.
All **I and M** tests pass. The 8 failures are **Zicsr/Zicntr** CSR tests — a
genuine limitation: mini-rv32ima implements only the CSR behavior Linux needs
(e.g. no WARL masking of `mepc` low bits — expects `…90`, returns `…91`).

### atoomnetmarc/RISC-V-emulator — RV32IMC (+Zicsr), **76/91**

Embeddable library; we provide a host runner around it (RV32IMC enabled). All
**I, M and C (compressed)** instruction tests pass. The 15 failures are real
emulator gaps, classified by the runner's diagnostics:
- **Zicsr** (6): CSR WARL semantics (same `mepc` low-bit issue as above);
- **Zicntr** (2): `cycle`/`time` counter CSRs (`0xc00`) unimplemented → illegal;
- **fence.i** (1): rejected when the reserved immediate field is non-zero;
- **Misalign lw/sw** (2): misaligned-access/trap behavior differs from the reference;
- plus a few that fall out of the above (control reaches an unimplemented path).

These are exactly the kind of findings conformance testing exists to surface; the
adapters are faithfully reporting the emulators' real behavior, not masking it.
