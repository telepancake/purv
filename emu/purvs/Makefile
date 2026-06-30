# purvs - monadic (decode-ahead + pluggable threaded eval) variant of purv.
#
#   make          build ./purvs
#   make test     build + run the example programs (same set purv runs)
#
# purvs is purv restructured: a decode pass lowers a straight-line run of
# instructions into a wide internal buffer, then a PLUGGABLE evaluator threads
# through it (default: ordinary RV32IMC emulation via a computed-goto jump
# table). The host/driver and guest examples are shared with purv.

CC      ?= gcc
# -g keeps source line info for profilers/debuggers (no effect on -O2 codegen).
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra

# Guest toolchain (compiles the RISC-V example programs the emulator runs).
GUESTCC      ?= clang
GUEST_ARCH   ?= --target=riscv32 -march=rv32imc -mabi=ilp32
GUEST_CFLAGS ?= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
                -O2 -Wall -Wextra -Werror -Wpedantic
RVLD     := ld.lld
# The guest layout (code in the lower half at 0, writable data in the upper half
# at 0x80000000) is shared with purv; reuse its linker script and examples.
EX       := ../examples
GUEST_LD := $(EX)/purv.ld
LDFLAGS  ?= -m elf32lriscv -T $(GUEST_LD)

.PHONY: all test clean
all: purvs

# purvs.c is the engine; main.c (shared with purv) is the host/driver.
purvs: purvs.c main.c purvs.h
	$(CC) $(CFLAGS) purvs.c main.c -o $@

# Library-style codelet (called via --invoke, entry at 0).
$(EX)/fn.elf: $(EX)/fn.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) --entry=0 $<.o -o $@
# Full programs with _start, run to halt.
$(EX)/sigtest.elf: $(EX)/sigtest.S $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) -e _start $<.o -o $@
$(EX)/hello.elf $(EX)/args.elf: $(EX)/%.elf: $(EX)/%.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) -e _start $<.o -o $@

test: purvs $(EX)/fn.elf $(EX)/sigtest.elf $(EX)/hello.elf $(EX)/args.elf
	@echo "== wasm-style function calls =="
	@printf "square(7)   = "; ./purvs --invoke=square --arg=7   $(EX)/fn.elf
	@printf "sum_to(100) = "; ./purvs --invoke=sum_to --arg=100 $(EX)/fn.elf
	@printf "fib(20)     = "; ./purvs --invoke=fib    --arg=20  $(EX)/fn.elf
	@echo "== userspace program (ecall syscalls) =="
	@./purvs --user $(EX)/hello.elf
	@./purvs --user $(EX)/args.elf one two
	@echo "== conformance signature dump =="
	@./purvs --signature=$(EX)/out.sig $(EX)/sigtest.elf && \
	  echo "signature:" && sed 's/^/  /' $(EX)/out.sig

clean:
	rm -f purvs $(EX)/fn.c.o $(EX)/sigtest.S.o $(EX)/hello.c.o $(EX)/args.c.o
