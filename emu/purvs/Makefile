# purvs - secure (tagged-memory) variant of purv.
#
#   make          build ./purvs
#   make test     build + run the pointer-safety demo (ok / oob / uaf / cross)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra

# Guest toolchain. rv32im (no compressed) so the shadow tag datapath, which
# decodes 32-bit instructions, sees every instruction.
GUESTCC      ?= clang
GUEST_ARCH   ?= --target=riscv32 -march=rv32im -mabi=ilp32
GUEST_CFLAGS ?= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -O1
RVLD     := ld.lld
LDFLAGS  ?= -m elf32lriscv --image-base=0x80000000

.PHONY: all test clean
all: purvs

purvs: purvs.c main.c purvs.h
	$(CC) $(CFLAGS) purvs.c main.c -o $@

examples/safe.elf: examples/safe.c
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $<.o
	$(RVLD) $(LDFLAGS) -e _start $<.o -o $@

test: purvs examples/safe.elf
	@echo "== ok (in-bounds) =="      ; ./purvs examples/safe.elf ok      ; echo "exit=$$?"
	@echo "== diff (same-object) =="  ; ./purvs examples/safe.elf diff    ; echo "exit=$$?"
	@echo "== align (p & ~15) =="     ; ./purvs examples/safe.elf align   ; echo "exit=$$?"
	@echo "== oob =="                 ; ./purvs examples/safe.elf oob     ; echo "exit=$$?"
	@echo "== uaf =="                 ; ./purvs examples/safe.elf uaf     ; echo "exit=$$?"
	@echo "== subdiff (cross diff) =="; ./purvs examples/safe.elf subdiff ; echo "exit=$$?"
	@echo "== addp (ptr + ptr) =="    ; ./purvs examples/safe.elf addp    ; echo "exit=$$?"
	@echo "== rsub (int - ptr) =="    ; ./purvs examples/safe.elf rsub    ; echo "exit=$$?"
	@echo "== scale (shift ptr) =="   ; ./purvs examples/safe.elf scale   ; echo "exit=$$?"
	@echo "== cross-object =="        ; ./purvs examples/safe.elf cross   ; echo "exit=$$?"
	@echo "== xdata (exec data) =="   ; ./purvs examples/safe.elf xdata   ; echo "exit=$$?"

clean:
	rm -f purvs examples/*.o examples/*.elf
