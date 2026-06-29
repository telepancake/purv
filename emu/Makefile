# purv — single-header RISC-V (RV32IMC + Zicsr/Zifencei) emulator.
#
#   make            build the host emulator binary `purv`
#   make test       build + run the examples (wasm-style call + signature dump)
#   make sqlite      build + run freestanding SQLite on purv (see sqlite/README.md)
#   make benchmark   build a heavy SQLite workload native + on purv, compare timings
#   make compact     build + run the 363-byte no-runtime flat binary (see compact/README.md)
#   make regen      regenerate purv.h from the atoom submodule (needs it checked out)
#   make clean
#
# Host compiler (the emulator runs on your machine).
# -Wno-packed-bitfield-compat: purv.c decodes instructions with packed
# bit-field structs; gcc emits a note about a pre-4.4 layout change that is
# irrelevant here (the layout is consistent within our own build).
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-packed-bitfield-compat

# Guest toolchain (compiles RISC-V code the emulator runs). clang needs no
# cross-gcc: it targets riscv32 directly and lld links it.
GUESTCC      ?= clang
GUEST_ARCH   ?= --target=riscv32 -march=rv32imc -mabi=ilp32
# "scary flags": strict, freestanding, no runtime assumptions.
GUEST_CFLAGS ?= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
                -O2 -Wall -Wextra -Werror -Wpedantic
# Named RVLD (not LD) so an inherited `LD=ld` in the environment can't shadow it.
RVLD     := ld.lld
LDFLAGS  ?= -m elf32lriscv --image-base=0x80000000

ATOOM_INC ?= ../third_party/atoomnetmarc-rv/include
OUTDIR    ?= .

.PHONY: all test conformance sqlite benchmark compact regen clean
all: purv

# Freestanding SQLite running on purv: fetches the amalgamation, builds the
# guest + host, and runs the SQL demo. Self-contained; see sqlite/README.md.
sqlite:
	$(MAKE) -C sqlite run

# Benchmark: the heavy bench.c workload compiled both native and for purv, run on
# each, transcripts diffed for equality, wall-clock + MIPS reported. `SCALE=` (a
# percentage of the default size) dials the runtime; see sqlite/Makefile.
# Pass SCALE through only when set, so the sub-make keeps its own default.
benchmark:
	$(MAKE) -C sqlite benchmark $(if $(SCALE),SCALE=$(SCALE))

# Compact no-runtime demo: a 363-byte headerless fixed-point Mandelbrot flat
# binary, self-contained (own host). See PORTING.md and compact/README.md.
compact:
	$(MAKE) -C compact run

# Real conformance: the self-checking riscv-tests ISA suite (needs network +
# clang; uses spike only if present). See README "Running the real conformance
# suite" and selfcheck.sh.
conformance: purv
	./selfcheck.sh

# Optional GDB stub: `make GDB=1` compiles gdbstub.c (guarded by PURV_GDBSTUB)
# into the binary, adding `--gdb=FD` so gdb can drive the emulator over a
# launcher-provided fd. Without GDB=1 none of the stub is compiled.
GDB_SRC := $(if $(filter 1,$(GDB)),gdbstub.c)
GDB_DEF := $(if $(filter 1,$(GDB)),-DPURV_GDBSTUB)

# purv.c is the generated engine; main.c is the host/driver. Separate TUs.
purv: purv.c main.c purv.h $(GDB_SRC) $(if $(GDB_SRC),gdbstub.h)
	$(CC) $(CFLAGS) $(GDB_DEF) purv.c main.c $(GDB_SRC) -o $@

# Build any guest .c/.S into an ELF loadable at RAM_ORIGIN (0x80000000).
# .c is library-style (called via --invoke, so no entry needed); .S is a full
# program with _start, run to halt.
%.elf: %.c
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) --entry=0 $<.o -o $@
%.elf: %.S
	$(GUESTCC) $(GUEST_ARCH) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) -e _start $<.o -o $@

# Userspace programs: need _start as the ELF entry (--user runs from e_entry).
examples/hello.elf examples/args.elf examples/loop.elf: %.elf: %.c
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) -e _start $<.o -o $@

test: purv examples/fn.elf examples/sigtest.elf examples/hello.elf examples/args.elf
	@echo "== wasm-style function calls =="
	@printf "square(7)   = "; ./purv --invoke=square --arg=7   examples/fn.elf
	@printf "sum_to(100) = "; ./purv --invoke=sum_to --arg=100 examples/fn.elf
	@printf "fib(20)     = "; ./purv --invoke=fib    --arg=20  examples/fn.elf
	@echo "== userspace program (ecall syscalls) =="
	@./purv --user examples/hello.elf
	@./purv --user examples/args.elf one two
	@echo "== conformance signature dump =="
	@./purv --signature=examples/out.sig examples/sigtest.elf && \
	  echo "signature:" && sed 's/^/  /' examples/out.sig

# Regenerate purv.c from the upstream submodule and post-process it into the
# checked-in form: inline the single-call-site handler tree into the public
# functions (tools/inline.py), then reflow (tools/compact.py). Both are
# behavior-preserving; the result is verified by `make conformance`.
regen:
	python3 tools/flatten.py $(ATOOM_INC) $(OUTDIR)
	python3 tools/compact.py $(OUTDIR)/purv.c
	python3 tools/inline.py  $(OUTDIR)/purv.c
	python3 tools/compact.py $(OUTDIR)/purv.c
	@echo "regenerated purv.c from $(ATOOM_INC): inlined + compacted (purv.h is hand-written)"

clean:
	rm -f purv purv_clang examples/*.o examples/*.elf examples/out.sig
