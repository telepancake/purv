# Compact freestanding RV32 demo for purv: a flat (headerless) binary.
#   make / make run   build mandelbrot.bin + host, then run it
#   make size         flat image size + remaining runtime symbols (want: none)
#   make clean
#
# Self-contained: the ONLY dependency is the purv engine (../purv.c, ../purv.h).
# Nothing here reaches into the sqlite example.
RES := $(shell clang -print-resource-dir)/include

GUEST := clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany -Os \
         -ffreestanding -nostdlib -fno-stack-protector -fomit-frame-pointer \
         -ffunction-sections -fdata-sections \
         -fno-unwind-tables -fno-asynchronous-unwind-tables \
         -nostdinc -isystem $(RES) -I.

.PHONY: all run size clean
all: mandelbrot.bin host

mandelbrot.o: mandelbrot.c hostcalls.h
	$(GUEST) -c mandelbrot.c -o mandelbrot.o

# Flat image: no ELF header, no program/section headers -- raw code+rodata with
# _start at offset 0. ld.lld emits it directly (--oformat binary), no objcopy.
mandelbrot.bin: mandelbrot.o link-flat.ld
	ld.lld --oformat binary -T link-flat.ld --gc-sections -e _start -s -o mandelbrot.bin mandelbrot.o
	@echo "flat image: $$(stat -c%s mandelbrot.bin) bytes"

# Host: its own, depending only on the purv engine. -g keeps source line info for
# profilers/debuggers (no effect on -O2 codegen).
host: host.c ../purv.c ../purv.h
	gcc -std=c11 -O2 -g -Wall -Wextra -Wno-packed-bitfield-compat host.c ../purv.c -o host

run: mandelbrot.bin host
	./host mandelbrot.bin

size: mandelbrot.bin mandelbrot.o
	@echo "flat image: $$(stat -c%s mandelbrot.bin) bytes"
	@echo "runtime symbols still undefined (want: none):"
	@llvm-nm -u mandelbrot.o 2>/dev/null | sed 's/^/  /' || true

clean:
	rm -f *.o *.bin host
