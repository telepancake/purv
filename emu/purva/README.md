# purva — compile C/C++ to a compact image, embed it anywhere

This directory is the packaged product: take C and C++ sources with no
external dependencies, compile them into one self-contained loadable blob,
and run that blob inside any host program through a small callback API, with
instruction-level fuel metering. No RTTI, no exceptions, no STL, no libc, no
OS — and everything ordinary code needs still works: `malloc`, `memcpy`,
strings, virtual dispatch, function pointers.

```
$ make -C emu/purva embed-demo
guest: hello from inside the image
guest: malloc'd and memset: ...........
guest: fib(24) = 46368
guest: the host rolled 6 on a d6
host: guest finished, exit=7, ~1697378 insns in 9 fuel slices
```

## The five-minute version

**Guest** (`app.c` — or `.cc`, same everything):

```c
#include <string.h>        /* the runtime provides the libc subset          */
#include "purvguest.h"     /* host_write / host_exit / purv_hostcall / cmain */

int cmain(void) {
    char *p = malloc(64);                 /* the heap is a host service     */
    host_write(1, "hi\n", 3);             /* -> the host's write callback   */
    long v = purv_hostcall(16, 5, 0, 0);  /* -> the host's hostcall hook    */
    return 0;                             /* the exit code the host sees    */
}
```

**Build** (one command; produces `app.img`, typically a few KB):

```
emu/purva/pvcc app.c --rt -o app.img
```

**Host** (any C or C++ program; link `purva-engine.o`, include one header):

```c
#include "purvarun.h"

PurvaRun r;
PurvaCallbacks cb = { .write = my_write, .hostcall = my_hook, .user = ctx };
purva_run_init(&r, blob, blob_len, 0, 0, &cb);      /* blob = app.img bytes */
while (purva_run(&r, 1000000, NULL) == PURVA_RUN_FUEL)
    ;                                                /* host things between slices */
printf("exit=%d\n", r.exit_code);
purva_run_free(&r);
```

Working copies of both halves: [`../examples/embed-app.c`](../examples/embed-app.c)
and [`../examples/embed.c`](../examples/embed.c), wired up by `make embed-demo`.

## What the guest environment is

`pvcc --rt` links the freestanding runtime (`../sqlite/rt.c`, plus
`../bench/rt-cxx.cc` when any source is C++):

- **entry**: you implement `int cmain(void)`; its return value is the exit code.
- **libc subset**: `<string.h>`, `<stdlib.h>`, `<ctype.h>` work. `malloc`/
  `free`/`realloc` are host services (the guest carries no allocator and no
  static heap). `memcpy`/`memset`/`memcmp`/`memchr`/`strlen`/`strcmp`/
  `strchr`/`strncmp` are each a *single custom instruction* the engine
  services at host speed (`../purvmemop.h`).
- **C++**: `-fno-rtti -fno-exceptions -fno-threadsafe-statics`; `new`/`delete`
  route to the malloc hostcalls; virtual dispatch works (vtables are patched
  at transcode time and fuse into one op). Global constructors are a
  compile error rather than a silent no-op — nothing runs `.init_array`.
- **not there, on purpose**: files, clocks, environment, floats (soft-float
  helpers are trap stubs — integer code only, loudly enforced), threads.
  Anything host-ish is a hostcall you define on both sides: pick a number
  >= 16, `purv_hostcall(n, ...)` in the guest, `cb.hostcall` in the host.

## What the host API is

`purvarun.h` is one self-contained header over the engine (`purva.c`, built as
`purva-engine.o`). `purva_run_init` copies everything out of the blob, so the
blob buffer can be freed immediately; instances are independent and may
interleave. `purva_run(&r, fuel, &used)` executes up to ~`fuel` guest
instructions and returns:

| status           | meaning                                              |
|------------------|------------------------------------------------------|
| `PURVA_RUN_DONE` | guest returned from `cmain` / called `host_exit`; `r.exit_code` |
| `PURVA_RUN_FUEL` | budget exhausted mid-run; call again to continue     |
| `PURVA_RUN_TRAP` | illegal op, wild jump, or a hostcall said stop       |

Fuel is semi-accurate RV32 instruction counting: fused ops charge what they
replaced (a fused prologue counts its stores; a bulk `memcpy` charges ~n/4).
A slice may overshoot by one straight-line run — budget checks sit at jumps —
and never splits a hostcall.

Memory: the guest sees code at pc 0 (fetch-only op words), rodata just below
address 0, and one writable span (stack growing down to `RISCV_HALF`, then
data/bss/heap). Region sizes default to 1 MiB stack + 16 MiB RAM; pass
explicit sizes to `purva_run_init` to shrink or grow (the image carries its
minimums). Guest loads/stores cannot reach host memory — misses fall to the
engine's memory callback, which defaults to read-0/drop.

## The pipeline underneath (when you need more than the demo)

| piece            | what it is                                                      |
|------------------|-----------------------------------------------------------------|
| `pvcc`           | clang/lld wrapper: sources -> RV32IM ELF (`--emit-relocs`, `purva.ld`) -> `transcode` -> `.img`. `--rt` links the runtime; `--lib` skips `_start` for wasm-style `--invoke` use |
| `transcode`      | the ONLY RISC-V decoder in the pipeline: lowers instructions to packed op words, fusing measured-hot patterns (prologues, address chains, load+branch, the vtable call) — see `transcode.h` |
| `purva` (CLI)    | standalone runner: `--user` programs, `--invoke fn --arg n` calls into `--lib` images, conformance signatures |
| `purvarun.h`     | the embed API above                                             |
| `purvguest.h`    | the guest API (in `../sqlite/`, next to the runtime)            |
| `image.h`        | the blob format: 5-word header + ops + rodata + rwdata          |
| `profile.py`     | joins a `PURVA_PROFILE` run's counters with the image: dynamic op mix, hot pairs — the evidence any new fused op must bring |

Debugging story: the same sources build unmodified for **purv** (the plain
RV32IMC interpreter one directory up), which has a GDB remote stub — develop
and debug there, ship the purva image. The sqlite hosts service the custom
mem/str instruction from purv's illegal-instruction hook, so `--rt` guests
run on both engines with identical results.

Evaluation lives next door: `make -C ../sqlite benchmark` (freestanding
SQLite + JSON, diffed against native) and `make -C ../bench benchmark`
(zlib, richards, deltablue — C and C++, byte-identical transcripts required).
Current standing on those: ~10x native for compute-heavy C++, ~9x for zlib.
