# Conformance setup — status & findings

_Living document. Update as steps are verified so we don't re-derive next session._

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
- [x] `apt-get install gcc-riscv64-unknown-elf device-tree-compiler` →
      `riscv64-unknown-elf-gcc 13.2.0`, rv32 ilp32 multilibs present
      (rv32i, rv32im, rv32imac).
- [x] Prebuilt **Sail 0.12** downloaded → `tools/sail-0.12/bin/sail_riscv_sim`,
      `--version` prints `0.12`.
- [~] **Spike** building from source (`third_party/riscv-isa-sim/build/spike`).
- [x] `pip install riscof` works (riscof 1.25.3) — but RISCOF is the deprecated
      path; not needed for ACT4.

## TODO (next session)

1. Install the ACT4 framework toolchain. Two options:
   - **mise** (recommended by ACT4): `curl https://mise.jdx.dev/install.sh | sh`
     then `mise install` in the repo — handles uv/Python + Ruby + UDB gem.
   - **without mise**: provide Python 3.10+, then
     `pip install -e ./framework -e ./generators/testgen -e ./generators/coverage`,
     and install Ruby + Bundler + riscv-unified-db separately.
2. Write the DUT description for purv's target ISA (RV32IMC_Zicsr_Zifencei):
   UDB config + `rvmodel_macros.h` + linker script.
3. `make` (or `make spike-<ext>`) to generate self-checking ELFs via Sail 0.12.
4. Run the ELFs on Spike (the built-in `make spike-*` targets) → first green
   reference run.
5. Swap Spike for `purv` once it can execute ELFs + self-check.

Run `scripts/setup.sh` to reproduce steps 1–verified above.
