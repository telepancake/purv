# SQLite on purv — freestanding, two host calls

SQLite, compiled completely freestanding (no libc, no OS) for RV32IMC, running on
the purv emulator. The guest's *entire* dependency on the outside world is two
`ecall` host functions: **write** and **exit**. Everything else — the SQL parser,
the byte-code VM, the b-tree, the in-memory pager, memory allocation — executes as
pure computation inside the emulated machine.

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
| `rt.c`          | freestanding runtime: `_start`, the libc subset SQLite links, a K&R `malloc` over a static heap, the `ecall` stubs |
| `builtins.c`    | compiler runtime `-nostdlib` drops: 64-bit divide, `__atomic_*`, a tiny soft-double shim (all pure computation, not host deps) |
| `vfs.c`         | the minimal `SQLITE_OS_OTHER` VFS (in-guest PRNG + nominal time; file ops are stubs — `:memory:` never calls them) |
| `guest.c`       | the test program: open `:memory:`, run SQL, print rows via `write`      |
| `host.c`        | the purv host: loads the ELF, runs the engine, services host calls      |
| `include/`      | header stubs so the freestanding compile resolves `<string.h>` etc.     |
| `vendor/`       | the unmodified SQLite amalgamation (3.47.0)                             |

The SQLite build options (in `build.sh`) are the ones from sqlite.org that strip
it to that minimum: `SQLITE_OS_OTHER`, `SQLITE_THREADSAFE=0`,
`SQLITE_TEMP_STORE=3`, `SQLITE_OMIT_FLOATING_POINT` (no soft-float needed),
`SQLITE_OMIT_DATETIME_FUNCS`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_AUTOINIT`,
and friends. The result: the linked 1.6 MB guest has **zero undefined symbols**
and exactly **two `ecall` sites** in the entire binary.

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
