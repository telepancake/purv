# Writing your own guest code for purv

purv executes **RV32IMC + Zicsr/Zifencei**: it has 32-bit multiply/divide (M) and
compressed instructions (C), but **no** hardware floating point (no F/D), **no**
atomics (no A), and no bit-manip (no Zbb). A "compiler builtin" / runtime-library
call appears for exactly two reasons: the ISA has no instruction for the
operation, or a language feature needs runtime support. Keeping those calls to a
minimum is mostly about staying inside what the ISA can do.

## What each feature costs (measured on `-march=rv32imc -mabi=ilp32 -O2`)

| you write… | emits | note |
|---|---|---|
| `float` / `double` math | `__adddf3 __muldf3 __divdf3 __fixdfdi __floatdidf …` | soft-float — **the big one** |
| 64-bit `/` or `%` | `__divdi3 __udivdi3 __moddi3 __umoddi3` | M does only 32-bit divide |
| `_Atomic` / `std::atomic` | `__atomic_*` | no A extension |
| struct/array copy, big init | `memcpy` (`memset/memmove/memcmp`) | compiler-synthesized — always |
| 32-bit `* / %` | *(nothing)* | hardware (M) |
| 64-bit `*`, `<<`, `+`, `-` | *(nothing)* | inlined; only 64-bit **divide** needs a helper |
| `__builtin_popcount/clz/ctz` | *(nothing)* | clang open-codes them at `-O2` even without Zbb |
| C++ `new`/`throw`/RTTI/virtual | `_Znaj __cxa_throw _ZTIi __cxa_guard_* …` | language runtime |

## Rules of thumb, biggest lever first

1. **No floating point.** Use integers or fixed-point (see `compact/mandelbrot.c`
   for Q16.16). Removes the entire soft-float family.
2. **Keep division 32-bit.** 32-bit `* / %` are free; 64-bit `+ - * <<` are free;
   it's 64-bit `/` and `%` by a *runtime* value that call `__divdi3`. (Dividing an
   `i64` by a constant becomes a multiply — fine.)
3. **Single-threaded, no atomics.** Otherwise you must provide `__atomic_*`
   (trivial as plain load/store on one core — see `sqlite/builtins.c`).
4. **You'll always need `memcpy/memset/memmove/memcmp`** for aggregates; just
   provide them (tiny). Avoid big struct copies / zero-inits in hot paths to keep
   them rare.

## Flags

C baseline:
```
clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany \
      -ffreestanding -nostdlib -fno-stack-protector -Os
```
Use `-Os`/`-O2` (not `-O0`, which emits more incidental calls; avoid `-O3`
vectorization that can synthesize `memcpy`/`memset`). Use `-fno-builtin` only in
the file that *implements* `mem*`.

C++ — each removes a whole category of runtime:
```
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
-fno-unwind-tables -fno-asynchronous-unwind-tables
```
…and avoid the STL/iostreams (they pull exceptions, locale, `new`/`delete`).
Classes, templates and virtuals themselves are fine — vtables are just data.

## The workflow (ground truth)

Compile and list the undefined symbols — that's exactly what you must provide or
eliminate:
```
clang … -ffreestanding -nostdlib -O2 -c foo.c -o foo.o
llvm-nm -u foo.o
```
For each symbol: **eliminate** (change the code — preferred) or **provide** (a few
lines, like `sqlite/builtins.c`). `emu/compact` reaches *zero* such symbols;
`emu/sqlite` needs a handful and provides them.

## Smallest ELF

`-ffunction-sections -fdata-sections` + `ld.lld --gc-sections -s`, a linker
script with a single `PT_LOAD` that folds in the ELF/program headers (no
padding), and a post-link pass to drop the section-header table (a loader only
reads program headers). `emu/compact` does all of this — 447 bytes.

Leaner still: a **flat binary** (`link-flat.ld` with `_start` first, then
`objcopy -O binary`) drops *all* headers — 363 bytes, every byte program. The
host loads it explicitly with `--flat` (it never auto-detects the format).

## The escape hatch

If you would rather *use* floats/atomics than avoid them, extend purv with the
F/D and A extensions and build `-march=rv32imafdc -mabi=ilp32d`; the FPU and
atomics become native instructions and the libcalls disappear. That's engine
work — on stock `rv32imc`, avoid-and-provide is the path.
