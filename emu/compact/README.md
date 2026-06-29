# compact — a tiny no-runtime RV32 flat binary on purv

The opposite extreme from `emu/sqlite`: a **363-byte**, fixed-address, headerless
**flat binary** that pulls in **zero** compiler-runtime helpers, yet renders the
Mandelbrot set as ASCII. It is fully self-contained — its only dependency is the
purv engine (`../purv.c`, `../purv.h`); it shares nothing with the sqlite example.

```sh
make run
```

```
fixed-point Mandelbrot (Q16.16, no floats) on purv
                          ...
                       ...=@@@@@@@@@@@@+@@@@@@@@@@@@@@@@@@@@@@@@.
                          ...
```

## What makes it small

| lever | effect |
|-------|--------|
| **fixed-point, not float** | Q16.16 integer math (`fxmul` = `(int64)a*b >> 16`, two RV32 instructions). No `double`, so none of the soft-float family. |
| **no 64-bit divide** | only 32-bit `/` (hardware `div`) and inlined 64-bit multiply/shift — no `__divdi3`/`__udivdi3`. |
| **one TU, freestanding** | `-Os -march=rv32imc` (compressed), `-nostdlib`, `--gc-sections`. The only outside contact is the `ecall` host calls. |
| **flat binary** | `link-flat.ld` puts `_start` first; `ld.lld --oformat binary` emits raw bytes — no ELF header, no program headers, no section headers. |

The result references **no runtime symbols at all** (`make size` to confirm) —
contrast SQLite, which needed `memcpy`/`memset`/64-bit divide/atomics/soft-float.

## The 363 bytes

```
  300  machine code (106 instructions, ~2.8 bytes each -> RVC)
   63  read-only data (the banner + the " .:-=+*#%@" palette)
    0  headers of any kind
```

Every byte is code or data you wrote. (An ELF of the same program would add ~84
bytes of header structure; `a.out` would add a 32-byte exec header — a flat binary
is leaner than either, and is how bare-metal firmware images are shipped.)

## Self-contained platform

```
mandelbrot.c   the guest: _start, the ecall stubs, the renderer
hostcalls.h    this example's host-call ABI (exit / write / malloc / free)
link-flat.ld   single-segment, headerless layout, _start first, base 0x80000000
host.c         the host: loads the flat image, runs the purv engine, services
               the four host calls. Depends only on ../purv.c + ../purv.h.
Makefile       build / run / size
```

The host loads the flat image at `0x80000000` and jumps to offset 0. The guest
reaches it through `ecall` only; the engine has no syscall ABI baked in (the ecall
hook just raises a flag, the run loop services it).

## Writing your own compact guest

See `emu/PORTING.md` for the general rules (avoid float, 64-bit divide, atomics;
which flags drop which compiler-runtime calls, and how to get a headerless flat
binary). This directory is a worked example of all of them.
