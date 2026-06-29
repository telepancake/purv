# compact — a tiny no-runtime RV32 ELF on purv

The opposite extreme from `emu/sqlite`: a **447-byte**, fixed-address,
statically-linked ELF that pulls in **zero** compiler-runtime helpers, yet does
something non-trivial — it renders the Mandelbrot set as ASCII. It runs on the
same emulator and the same host-call ABI as the SQLite port (memory / output /
exit), so `emu/sqlite/host` loads it unchanged.

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
| **fixed-point, not float** | Q16.16 integer math (`fxmul` = `(int64)a*b >> 16`, two RV32 instructions). No `double`, so none of the soft-float family SQLite needed. |
| **no 64-bit divide** | only 32-bit `/` (hardware `div`) and inlined 64-bit multiply/shift — no `__divdi3`/`__udivdi3`. |
| **one translation unit, freestanding** | `-Os -march=rv32imc` (compressed), `-nostdlib`, `--gc-sections`. The only outside contact is the `ecall` host calls. |
| **lean link** | `link.ld` folds the ELF header + program header into the single `PT_LOAD` segment (no padding); `-s` strips symbols. |
| **drop section headers** | `tinyelf.py` removes the section-header table (a loader only needs *program* headers) and truncates trailing bytes. |

The result references **no runtime symbols at all** (`make size` to confirm) —
contrast SQLite, which needed `memcpy`/`memset`/64-bit divide/atomics/soft-float.
A larger program would still need the `mem*` four, but nothing here triggers them.

## The 447 bytes

```
   52  ELF header
   32  one program header (PT_LOAD, RWX, includes the headers themselves)
 ~363  code + rodata (the renderer + the palette/banner strings)
    0  section headers (removed)
```

So ~84 bytes of unavoidable ELF structure and the rest is the actual program.

## Even leaner: a flat binary (363 bytes, no headers)

ELF isn't the floor. `make` also produces `mandelbrot.bin`, a **raw flat image**
(`link-flat.ld` puts `_start` first; `ld.lld --oformat binary` emits it directly):
no ELF header, no
program header, no section headers — just the 300 bytes of code + 63 bytes of
strings. The host loads it at `0x80000000` and jumps to offset 0:

```sh
make run-flat        # ./host --flat mandelbrot.bin
```

`--flat` is **explicit** — the host never sniffs the file; an ELF without `--flat`
is parsed as ELF, a flat image without `--flat` is rejected. (For reference, the
old Unix `a.out` is both heavier — a 32-byte exec header — and unsupported by the
riscv toolchain; a flat binary is leaner than either.) At 363 bytes the file *is*
the program: every byte is code or data you wrote.

## Files

```
mandelbrot.c   the whole thing: _start, the ecall stubs, and the renderer
link.ld        single-segment layout, headers folded in, fixed base 0x80000000
tinyelf.py     post-link: strip the section-header table
Makefile       build / run / size
```

`hostcalls.h` and the host come from `emu/sqlite` — same platform, smaller guest.

## Writing your own compact guest

See `emu/PORTING.md` for the general rules (avoid float, 64-bit divide, atomics;
which flags drop which compiler-runtime calls). This directory is a worked
example of all of them at once.
