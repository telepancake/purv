# bench — the benchmark corpus beyond sqlite

`../sqlite` was the first "everything stands on this" evaluation workload, but
one C codebase is a narrow lens. This directory adds real C and C++ workloads
with different shapes, all built the same way (the `bench.c` dual-build
pattern), all speaking the same host-call ABI (`../sqlite/hostcalls.h`), and
all run by the **sqlite hosts unchanged** — there is no new host code here.

```
make benchmark              # build everything, run native + purv + purvs + purva,
                            # require byte-identical transcripts, print the table
make benchmark SCALE=30     # percent of the default workload size
```

| workload    | language | what it stresses                                          |
|-------------|----------|-----------------------------------------------------------|
| `zbench`    | C        | zlib 1.3.1 (vendored, unmodified, `-DZ_SOLO`): deflate/inflate rounds over a deterministic mixed corpus — bit twiddling, table walks, tight inner loops |
| `richards`  | C++      | the classic OS-simulation benchmark: virtual dispatch, small-object `new`/`delete`, queue/pointer chasing |
| `deltablue` | C++      | the classic incremental constraint solver: a *dynamic* object graph, deep virtual call chains, heavy allocator traffic (which lands on the malloc-group host calls — a dimension sqlite barely touches) |

Each workload is one dual-build source file: compiled with `-DPURV_GUEST` it
is the freestanding RV32 guest (output through the `write` host call, heap
through the malloc group); compiled without, it is a native, self-timed
reference binary. `make benchmark` diffs the native transcript against every
engine's — any divergence is a hard failure, so the corpus doubles as a
correctness test. Every workload also self-checks (richards against the
published packet/hold counts, deltablue by verifying each propagated value,
zbench by verifying every round trip), so a miscomputation fails even if it
would be consistent across builds.

## How a workload is built

Exactly the sqlite recipe (see `../sqlite/Makefile`):

- **purv/purvs guest**: RV32IMC, `../sqlite/rt.c` (freestanding libc subset +
  ecall stubs) + `../sqlite/builtins.c`, linked with `../sqlite/purv.ld`.
- **purva guest**: RV32IM (purva doesn't decode compressed), linked with
  `../purva/purva.ld` + `--emit-relocs`, then `transcode` to a `.img`.
  The C++ workloads genuinely need the kept relocations: their vtables are
  code addresses stored in data, which the transcoder patches to op indexes.
- **C++**: the freestanding-C++ flavour pvcc uses (`-fno-rtti
  -fno-exceptions -fno-threadsafe-statics`), plus `rt-cxx.cc` (operator
  new/delete over the malloc host calls, the pure-virtual trap). Global
  constructors are banned at compile time (`-Werror=global-constructors`) —
  nothing runs `.init_array` in these guests.

zlib is fetched (pinned 1.3.1, from zlib.net's fossils) into `vendor/` on
first build and compiled `-DZ_SOLO` **in all builds including native**, so the
exact same zlib code runs everywhere and the transcripts can be compared.

## Adding a workload

1. Write one dual-build source (copy the `#ifdef PURV_GUEST` shims from
   `richards.cc`). Keep it deterministic and integer-only (no floats in the
   guest), self-checking, and give it a `BENCH_SCALE` knob.
2. Add its name to `WORKLOADS` in the Makefile. A plain C++ workload needs no
   further rules (the pattern rules cover it); a workload with vendored C
   sources gets explicit rules like zbench's.
