# Compact freestanding RV32 demo for purv (fixed-point Mandelbrot).
#   make        build mandelbrot.elf + the host
#   make run    build, then run on the emulator
#   make size   show the ELF size and remaining runtime symbols (should be none)
#   make clean
#
# The host and host-call ABI are shared with emu/sqlite (same emulator, same
# ecalls); only the guest differs.
RES   := $(shell clang -print-resource-dir)/include

GUEST := clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany -Os \
         -ffreestanding -nostdlib -fno-stack-protector -fomit-frame-pointer \
         -ffunction-sections -fdata-sections \
         -fno-unwind-tables -fno-asynchronous-unwind-tables \
         -nostdinc -isystem $(RES) -I../sqlite

.PHONY: all run run-flat size clean
all: mandelbrot.elf mandelbrot.bin host

mandelbrot.o: mandelbrot.c ../sqlite/hostcalls.h
	$(GUEST) -c mandelbrot.c -o mandelbrot.o

# Tiny ELF: headers folded into one segment, then section headers stripped.
mandelbrot.elf: mandelbrot.o link.ld tinyelf.py
	ld.lld -T link.ld --gc-sections -e _start -s -o mandelbrot.elf mandelbrot.o
	@python3 tinyelf.py mandelbrot.elf            # drop the unused section headers

# Flat image: no headers at all -- raw code+rodata, _start at offset 0.
# ld.lld emits the raw binary directly (--oformat binary), so no objcopy needed.
mandelbrot.bin: mandelbrot.o link-flat.ld
	ld.lld --oformat binary -T link-flat.ld --gc-sections -e _start -s -o mandelbrot.bin mandelbrot.o
	@echo "flat image: $$(stat -c%s mandelbrot.bin) bytes (vs ELF $$(stat -c%s mandelbrot.elf 2>/dev/null || echo '?'))"

# Reuse the emu/sqlite host verbatim (same engine, same host-call servicing).
host: ../sqlite/host.c ../purv.c
	gcc -std=c11 -O2 -Wall -Wextra -Wno-packed-bitfield-compat \
	    ../sqlite/host.c ../purv.c -o host

run: mandelbrot.elf host
	./host mandelbrot.elf

run-flat: mandelbrot.bin host
	./host --flat mandelbrot.bin

size: mandelbrot.elf
	@echo "ELF bytes: $$(stat -c%s mandelbrot.elf)"
	@echo "runtime symbols still undefined (want: none):"
	@llvm-nm -u mandelbrot.o 2>/dev/null | sed 's/^/  /' || true

clean:
	rm -f *.o *.elf host
