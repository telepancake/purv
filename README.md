# purv

An RV32 RISC-V emulator (target featureset: **RV32IMC_Zicsr_Zifencei**).

## Conformance

purv is conformance-tested with [RISCOF](https://github.com/riscv-software-src/riscof)
against the [Sail RISC-V](https://github.com/riscv/sail-riscv) golden model,
using the official [riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test)
suite. [Spike](https://github.com/riscv-software-src/riscv-isa-sim) is vendored
as a second emulator so the harness can be validated (Spike-vs-Sail) before
purv is wired in, and used as a differential-testing oracle during development.

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
