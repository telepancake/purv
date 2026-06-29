# SQLite on purv — freestanding, two host calls

SQLite, compiled completely freestanding (no libc, no OS) for RV32IMC, running on
the purv emulator. The guest's *entire* dependency on the outside world is a small
set of `ecall` host functions: **write**, **exit**, and the **memory group**
(`malloc`/`free`/`realloc`). Everything else — the SQL parser, the byte-code VM,
the b-tree, the in-memory pager — executes as pure computation inside the emulated
machine. The heap itself is a host service, so the guest carries no allocator and
reserves no memory upfront: it asks the host for memory on demand, exactly like it
asks for output.

```
$ make run
sqlite 3.47.0  (in-memory, freestanding on purv)

-- rows, ordered by qty desc --
3 | cherry | 12
4 | date | 7
...
```

Output is **byte-for-byte identical** to the system `sqlite3` for the same SQL.

## Why this is small

SQLite documents (<https://sqlite.org/selfcontained.html>) that it needs only
seven C-library functions at minimum — `memcmp memcpy memmove memset strcmp
strlen strncmp` — plus an optional allocator, plus a VFS for anything OS-ish. We
build for exactly that:

| file            | what it is                                                             |
|-----------------|------------------------------------------------------------------------|
| `hostcalls.h`   | the whole guest↔host ABI: `ecall` with fn in a7, args in a0.. → a0      |
| `rt.c`          | freestanding runtime: `_start`, the libc subset SQLite links, the `ecall` stubs (incl. `malloc`/`free`/`realloc` as thin host-call wrappers) |
| `builtins.c`    | compiler runtime `-nostdlib` drops: 64-bit divide, `__atomic_*` (single-core), and the soft-`double` symbols as **trap stubs** — this build has no floating point (see below) |
| `vfs.c`         | the minimal `SQLITE_OS_OTHER` VFS (in-guest PRNG + nominal time; file ops are stubs — `:memory:` never calls them) |
| `guest.c`       | the demo program: open `:memory:`, run SQL, print rows via `write`      |
| `bench.c`       | the heavy benchmark workload (shared by the guest and native builds)    |
| `host.c`        | the purv host: loads the ELF, runs the engine, services host calls, and runs the heap allocator over guest RAM (the malloc group) |
| `include/`      | header stubs so the freestanding compile resolves `<string.h>` etc.     |
| `vendor/`       | the unmodified SQLite amalgamation (3.47.0)                             |

The SQLite build options (in the `Makefile`) are the ones from sqlite.org that
strip it to that minimum: `SQLITE_OS_OTHER`, `SQLITE_THREADSAFE=0`,
`SQLITE_TEMP_STORE=3`, `SQLITE_OMIT_FLOATING_POINT` (see "No floating point"),
`SQLITE_OMIT_DATETIME_FUNCS`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_AUTOINIT`,
and friends. The result: the linked guest has **zero undefined symbols** and just
**five `ecall` sites** in the entire binary (`write`, `exit`, `malloc`, `free`,
`realloc`).

## The heap is a host service

The guest contains no allocator and no static heap — its writable image is a few
KB (`.bss` ≈ 0.5 KB, vs a 48 MB static heap in the first cut).
`malloc`/`free`/`realloc` are one-line `ecall` wrappers; the host runs the actual
allocator (a K&R first-fit with coalescing) over the guest's RAM arena, handing
back guest addresses. Memory grows on demand within the RAM the host already owns,
so there is no big upfront lump. A 2000-row recursive-CTE query in the demo churns
and grows the heap, and the result still matches the reference `sqlite3` exactly.

## No floating point

This build contains no floating-point implementation. `SQLITE_OMIT_FLOATING_POINT`
turns off SQL-level reals, but it does not make SQLite fully float-free at the C
level: a little real-`double` code survives — the printf `%f` branch
(`sqlite3FpDecode`/`dekkerMul2`), the float scanner (`sqlite3AtoF`), and
value↔REAL coercion. That code is *woven into core functions* (the printf engine,
the VDBE value system), so you can't cleanly `sed` it out — it isn't a separable
API, and removing the lines would break compilation.

Instead, the soft-`double` helpers the compiler emits for that code
(`__muldf3`, `__floatdidf`, `__eqdf2`, …) are provided in `builtins.c` as **trap
stubs**: they exist only so the program links, and execute `__builtin_trap()` if
ever called. An integer/text workload never reaches them — verified: the demo
(including the 2000-row query, whose `sum` exceeds 32 bits) runs to completion and
matches the reference, so no trap fires. If you ever `SELECT 3.14` or otherwise
parse/format a REAL, this build stops loudly (illegal instruction) rather than
silently computing — a deliberate "this build has no floats" stance. Swap the
stubs for a real soft-float library if you need reals.

## The host-call mechanism

Per the design: the engine's `ecall` hook does almost nothing — it raises a flag
and consumes the trap so execution resumes. The *run loop* checks the flag after
each instruction and that is where host functions are serviced (read `a7`/`a0..`,
do the work, write the result to `a0`). The engine has no syscall ABI baked in.

```c
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *st) {   /* the entire hook */
    g_ecall_pending = 1;
    RiscvEmulatorClearTrap(st);
}
/* ... and in the run loop: */
RiscvEmulatorLoop(st);
if (g_ecall_pending) { g_ecall_pending = 0; service_hostcall(st); }
```

## Build / run

```sh
make              # compile runtime + sqlite3.c + guest, link, build host
make run          # build, then run on the emulator
```

It's a real Makefile: every object/ELF/host binary is a file target with proper
prerequisites, so it rebuilds incrementally and only when an input changes. The
big `sqlite3.c` object is cached (recompiled only when the amalgamation source is
newer). Needs `clang` (targets riscv32 directly), `ld.lld`, and a host `gcc`.

## Benchmark: native vs emulated

`bench.c` is a deliberately heavy, fully reproducible SQL workload — bulk insert
in a transaction, a secondary index, `GROUP BY`/`HAVING`, window functions, a
correlated self-join, string functions, an `UPDATE`+`DELETE` pass, a large
`ORDER BY`, and two recursive CTEs (a Collatz-length tournament and an
integer-only Mandelbrot escape-count grid). It is **all integer arithmetic**
(the build has no floats) and every phase collapses to one checksum row, so the
transcript is small and deterministic.

```sh
make benchmark             # default workload
make benchmark SCALE=30    # percent of the default size (quicker)
```

The Makefile builds the *same* `bench.c` three ways — linked against a SQLite
compiled for this machine, as the freestanding RV32 guest on **purv**, and the
identical guest on **purvs** (the tagged-memory variant, via the same `host.c`
built `-DPURV_TAGGED` against `purvs.c`) — runs all three, **diffs the
transcripts (they must be byte-identical)**, and reports the wall-clock time,
the slowdown vs native, and the tagged-memory overhead:

```
  SQLite freestanding benchmark   (BENCH_SCALE=100, 13-line transcript, identical)

    native (host)            0.072 s
    purv (RV32)             28.214 s   79.3 MIPS, 2236144667 instructions
    purvs (RV32 + tags)     33.254 s   67.2 MIPS
    slowdown vs native   purv 391x, purvs 461x
    tag overhead         1.17x  (purvs vs purv)
```

The same SQLite options are used on all sides (the native build only swaps the
guest's stub VFS for this box's normal one — immaterial for `:memory:`), so the
comparison isolates the cost of interpreting RV32 instruction-by-instruction, and
the purv→purvs delta isolates the per-instruction tag-tracking cost (purvs's
hooks here are permissive — it tracks tags but enforces nothing, so ordinary
untagged SQLite runs unchanged). The host prints these stats only when asked
(`./host guest.elf --stats`); the plain demo run is unaffected. `~80 MIPS`,
`~390×`, and a `~1.2×` tag overhead are representative of a simple non-JIT
interpreter.

## Profiling the interpreter

The host is built with `-g` (still `-O2`), so a profiler can map back to source.
`host.c` and `purv.c` are separate translation units (no `-flto`), so the engine's
`RiscvEmulatorLoop` profiles as its own function instead of being inlined into
`main`:

```sh
valgrind --tool=callgrind --callgrind-out-file=cg.out ./host guest.elf
callgrind_annotate --auto=yes cg.out ../purv.c | less
```

A representative split: `RiscvEmulatorLoop` ~63%, the `RiscvEmulatorLoad` hook
~29% (instruction fetch dominates), `main` ~7%. With `--auto=yes` you get the hot
*lines* inside `RiscvEmulatorLoop` — the 16-bit fetch, the compressed-opcode test,
the dispatch switch. (If instead you see `???:main` at ~97% with `RiscvEmulatorLoop`
absent, that profile came from a build that inlined the engine into `main` — e.g.
`-flto` or compiling everything as one unit — and a `-g`-less binary, so there are
no source lines either.)
