# purv

An RV32 RISC-V emulator (target featureset: **RV32IMC_Zicsr_Zifencei**).

## Conformance

purv is conformance-tested against the official
[riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test) suite. The
current suite uses the **ACT4 framework**, which uses the
[Sail RISC-V](https://github.com/riscv/sail-riscv) model (release **0.12**) to
generate self-checking ELFs that run on the device under test.
[Spike](https://github.com/riscv-software-src/riscv-isa-sim) is vendored as the
initial DUT (and a differential-testing oracle) before purv is wired in.

> **Read [`conformance/STATUS.md`](conformance/STATUS.md) first.** It records
> the verified setup steps and the key finding that **riscv-arch-test
> deprecated RISCOF in favour of ACT4** — so the RISCOF scaffold under
> `conformance/` (config*.ini, plugin-purv, sail_cSim) is kept for reference
> only and is **not** the active path. `scripts/setup.sh` reproduces the
> verified environment.

These three live as git submodules under `third_party/`, pinned to specific
commits. Bring the whole conformance environment up with:

```
make bootstrap     # clone submodule contents + install riscof
make spike sail    # build the reference emulators
make validate      # RISCOF: Spike (DUT) vs Sail (reference) — proves the harness
make conformance   # RISCOF: purv  (DUT) vs Sail (reference) — the real test
```

Pinned submodule commits:

| Submodule         | Repo                                | Commit    |
|-------------------|-------------------------------------|-----------|
| `riscv-isa-sim`   | riscv-software-src/riscv-isa-sim    | `55b4658` |
| `sail-riscv`      | riscv/sail-riscv                    | `a526939` |
| `riscv-arch-test` | riscv-non-isa/riscv-arch-test       | `49cdd65` |

See [`conformance/README.md`](conformance/README.md) for the full walkthrough,
the per-featureset test buckets, and the RV32C decode watch-list.

> **Note:** cloning the submodules needs HTTPS access to `github.com`.
> `scripts/bootstrap.sh` falls back to the plain HTTPS egress proxy if a
> session injects a repo-scoped git-credential helper that blocks third-party
> repos.
