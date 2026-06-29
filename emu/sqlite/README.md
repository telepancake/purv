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
$ ./build.sh run
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
| `builtins.c`    | compiler runtime `-nostdlib` drops: 64-bit divide, `__atomic_*`, a tiny soft-double shim (all pure computation, not host deps) |
| `vfs.c`         | the minimal `SQLITE_OS_OTHER` VFS (in-guest PRNG + nominal time; file ops are stubs — `:memory:` never calls them) |
| `guest.c`       | the test program: open `:memory:`, run SQL, print rows via `write`      |
| `host.c`        | the purv host: loads the ELF, runs the engine, services host calls, and runs the heap allocator over guest RAM (the malloc group) |
| `include/`      | header stubs so the freestanding compile resolves `<string.h>` etc.     |
| `vendor/`       | the unmodified SQLite amalgamation (3.47.0)                             |

The SQLite build options (in `build.sh`) are the ones from sqlite.org that strip
it to that minimum: `SQLITE_OS_OTHER`, `SQLITE_THREADSAFE=0`,
`SQLITE_TEMP_STORE=3`, `SQLITE_OMIT_FLOATING_POINT` (no soft-float needed),
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
./build.sh        # compile runtime + sqlite3.c + guest, link, build host
./build.sh run    # build, then run on the emulator
```

Needs `clang` (targets riscv32 directly), `ld.lld`, and a host `gcc`. The big
`sqlite3.c` object is cached after the first build.
