# purva - AOT pipeline: standalone transcode tool + an evaluator that runs its output.
#
#   make            build ./transcode (tool) and ./purva (evaluator)
#   make test       transcode the RV32IM examples and run them on purva
#
# `transcode in.elf out.ops` is the ONLY thing that decodes RISC-V: one packed op
# word per instruction (ops[pc>>2]); nothing else. `purva --ops=out.ops in.elf` runs
# it -- the host still loads the ELF for memory + symbols (the upper layer, like the
# purv driver); the evaluator just threads the ops and never decodes or links the
# transcoder.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra

GUESTCC      ?= clang
GUEST_ARCH   ?= --target=riscv32 -march=rv32im -mabi=ilp32
GUEST_CFLAGS ?= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
                -O2 -Wall -Wextra -Werror -Wpedantic
RVLD     := ld.lld
EX       := ../examples
GUEST_LD := $(EX)/purv.ld
LDFLAGS  ?= -m elf32lriscv -T $(GUEST_LD)

.PHONY: all test clean
all: transcode purva

transcode: tctool.c transcode.c transcode.h ../purv.h ../purvhost.h
	$(CC) $(CFLAGS) tctool.c transcode.c -o $@
purva: purva.c main.c purva.h transcode.h ../purv.h ../purvhost.h
	$(CC) $(CFLAGS) purva.c main.c -o $@

fn.elf: $(EX)/fn.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o fn.o
	$(RVLD) $(LDFLAGS) --entry=0 fn.o -o $@
sigtest.elf: $(EX)/sigtest.S $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) -c $< -o sigtest.o
	$(RVLD) $(LDFLAGS) -e _start sigtest.o -o $@
hello.elf args.elf: %.elf: $(EX)/%.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $*.o
	$(RVLD) $(LDFLAGS) -e _start $*.o -o $@

%.ops: %.elf transcode
	./transcode $< $@

test: transcode purva fn.ops sigtest.ops hello.ops args.ops
	@echo "== wasm-style function calls =="
	@printf "square(7)   = "; ./purva --ops=fn.ops --invoke=square --arg=7   fn.elf
	@printf "sum_to(100) = "; ./purva --ops=fn.ops --invoke=sum_to --arg=100 fn.elf
	@printf "fib(20)     = "; ./purva --ops=fn.ops --invoke=fib    --arg=20  fn.elf
	@echo "== userspace program (ecall syscalls) =="
	@./purva --ops=hello.ops --user hello.elf
	@./purva --ops=args.ops --user args.elf one two
	@echo "== conformance signature dump =="
	@./purva --ops=sigtest.ops --signature=out.sig sigtest.elf && \
	  echo "signature:" && sed 's/^/  /' out.sig

clean:
	rm -f transcode purva *.o *.elf *.ops out.sig
