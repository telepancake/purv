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

.PHONY: all run size clean
all: mandelbrot.elf host

mandelbrot.elf: mandelbrot.c link.ld ../sqlite/hostcalls.h tinyelf.py
	$(GUEST) -c mandelbrot.c -o mandelbrot.o
	ld.lld -T link.ld --gc-sections -e _start -s -o mandelbrot.elf mandelbrot.o
	@python3 tinyelf.py mandelbrot.elf            # drop the unused section headers

# Reuse the emu/sqlite host verbatim (same engine, same host-call servicing).
host: ../sqlite/host.c ../purv.c
	gcc -std=c11 -O2 -Wall -Wextra -Wno-packed-bitfield-compat \
	    ../sqlite/host.c ../purv.c -o host

run: all
	./host mandelbrot.elf

size: mandelbrot.elf
	@echo "ELF bytes: $$(stat -c%s mandelbrot.elf)"
	@echo "runtime symbols still undefined (want: none):"
	@llvm-nm-18 -u mandelbrot.o 2>/dev/null | sed 's/^/  /' || true

clean:
	rm -f *.o *.elf host
