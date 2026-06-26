# Conformance setup — status & findings

_Living document. Update as steps are verified so we don't re-derive next session._

## Result (verified end-to-end)

The full ACT4 pipeline runs: **Sail 0.12 computes the golden expected results,
they're baked into self-checking ELFs, and Spike executes them and self-checks.**

Counts below: "build actions" is the build-plan task count the framework prints
as "N succeeded" (~5 actions per test: generate source, compile candidate, run
Sail for the expected signature, link the self-checking ELF, symlink). The
runnable ELF count is much smaller — and every generated ELF is run.

- `scripts/run-spike.sh spike-rv32-max` → 4245 build actions → **849 self-checking
  ELFs**, all 849 run: **842 passed, 7 failed**. The 7 are bleeding-edge
  privileged/security corners (`ExceptionsZicboS/U`, `Smstateen`, `Ssstrict*`)
  where Spike 1.1.1-dev and Sail 0.12 legitimately disagree — **all outside
  purv's RV32IMC scope**.
- `scripts/run-spike.sh spike-RVI20U32` (unprivileged IMAFDC, `include_priv_tests:
  False`) → 1485 build actions → **297 ELFs**, **all 297 passed** (clean green).
  Covers the I/M/C unprivileged tests purv cares about.

One command from a fresh checkout: **`scripts/reproduce.sh [config]`**
(default `spike-RVI20U32`). First run is heavy (downloads the GCC 16 toolchain
~550 MB + Sail 0.12, builds Spike, installs the framework).

### Third-party emulator DUTs (see `conformance/dut/README.md`)

The same suite runs on two external emulators via small C adapters. Genuine
conformance findings, not harness artifacts:

| Emulator | Scope | Result | Script |
|----------|-------|--------|--------|
| fernandotcl/TinyEMU | RV32IMC | **87/91** (most conformant; 4 timer/fence timeouts) | `scripts/run-tinyemu.sh` |
| atoomnetmarc/RISC-V-emulator | RV32IMC | **76/91** (I+M+C pass; CSR/Zicntr/fence.i/misalign gaps) | `scripts/run-atoomnetmarc.sh` |
| cnlohr/mini-rv32ima | RV32IM | **53/61** (I+M pass; 8 CSR gaps) | `scripts/run-mini-rv32ima.sh` |
| libriscv/libriscv | — | incompatible (userspace sandbox; no M-mode) | `scripts/run-libriscv.sh` |

## TL;DR / the big correction

The RISC-V architectural test suite **deprecated RISCOF**. The current
`riscv-arch-test` HEAD (`act4` branch, vendored at `49cdd65`) ships the new
**ACT4 Framework**. So the RISCOF scaffold under `conformance/` (config.ini,
plugin-purv, sail_cSim) is the **deprecated path — kept for reference only**.

**ACT4's model of operation** (from its README):
1. You describe the DUT (extensions/params) via a UDB config + an
   `rvmodel_macros.h` + a linker script.
2. ACT4 uses the **Sail reference model (pinned to release 0.12)** to compute
   the expected results and **bakes them into self-checking ELFs**.
3. You run those ELFs on the DUT (Spike now, `purv` later). Each test
   self-reports pass/fail — no runtime signature diff.

So "both emulators validated against each other" in the modern flow = Sail
generates the golden expectations, the DUT (Spike) executes and self-checks.

## Pinned versions

| Component        | Commit/Tag | Role                                   | Notes |
|------------------|-----------|----------------------------------------|-------|
| riscv-arch-test  | `49cdd65` | ACT4 test framework + sources          | new layout: `tests/rv32i`, `Makefile`, `run_tests.py` |
| riscv-isa-sim    | `55b4658` | DUT / differential oracle (Spike)      | builds clean from source |
| sail-riscv (submodule) | `a526939` | reference model SOURCE            | HEAD is CMake single-binary, **not** what ACT4 consumes |
| **Sail (used)**  | **release 0.12** | reference model ACT4 actually uses | **prebuilt binary**, downloaded to `tools/sail-0.12/` |

> Note: ACT4 is "currently compatible with version 0.12 of the model" — use the
> prebuilt 0.12 release, **not** a from-source build of the submodule HEAD.

## Verified working (this session)

- [x] HTTPS git egress works (the scoped git-credential proxy 403s, but plain
      HTTPS to github.com is open — that's how the submodules were fetched).
- [x] Prebuilt **Sail 0.12** → `tools/sail-0.12/bin/sail_riscv_sim` (`--version` = 0.12).
- [x] **Spike** built from source → `third_party/riscv-isa-sim/build/spike` (1.1.1-dev).
- [x] **ACT4 framework installed** via mise (`scripts/install-act4.sh`):
      ruby 3.4.9, uv, the `act`/`testgen`/`covergroupgen` python packages.
      UDB is pulled in through uv (no root Gemfile — the bundler step is a no-op).
- [x] `make tests` → generates **165 test suites / 87 extensions**, exit 0.
      Proves the generator + UDB + framework all work.
- [x] Spike RV32 DUT config exists out of the box:
      `config/spike/spike-rv32-max/` (link.ld, rvmodel_macros.h, run_cmd.txt,
      sail.json, test_config.yaml). Ideal template for a purv config.

## Fixes discovered (must apply — captured in scripts)

1. **Python 3.12 pin.** This env injects an "exclude-newer" date that makes uv
   resolve Python **3.14**, on which the pinned pydantic dies with
   `_eval_type() got an unexpected keyword argument 'prefer_fwd_module'`.
   Fix: `uv python pin 3.12` then `uv sync`. (In `install-act4.sh`.)
2. **GCC 15+.** ACT4 hard-requires GCC ≥ 15; Ubuntu 24.04's
   `gcc-riscv64-unknown-elf` is 13.2.0. Fix: use the prebuilt
   `riscv64-elf-ubuntu-24.04-gcc.tar.xz` from riscv-collab release `2026.06.06`
   → `tools/riscv-gcc/bin` (put first on PATH). (In `run-spike.sh`/`setup.sh`.)

## TODO (next session)

1. Finish the Spike reference run: `scripts/run-spike.sh spike-rv32-max`
   (generates self-checking ELFs via Sail 0.12, runs them on Spike). This was
   one toolchain-version bump away from running when the session ended.
2. Make a purv RV32IMC config by copying `config/spike/spike-rv32-max/` and
   trimming the ISA to `rv32imc_zicsr_zifencei` (UDB yaml + sail.json +
   run_cmd.txt + rvmodel_macros.h + link.ld).
3. Swap Spike's `run_cmd.txt` for `purv` once purv can execute an ELF and honour
   the `rvmodel_macros.h` halt/signature contract.

Reproduce everything: `scripts/setup.sh` → `scripts/install-act4.sh` →
`scripts/run-spike.sh`.
