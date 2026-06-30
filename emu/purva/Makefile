# purva - AOT pipeline: a standalone transcode tool + an evaluator that runs its output.
#
#   make            build ./transcode (the tool) and ./purva (the evaluator)
#   make test       build, transcode the RV32IM examples to .tc, run them on purva
#
# `transcode in.elf out.tc` is the ONLY thing that decodes RISC-V: it lowers a whole
# RV32IM image into a packed op stream + memory image (.tc, see tcfile.h). `purva
# out.tc` loads that and threads over it with a computed goto -- it never sees a raw
# instruction, and does not even link transcode.c.

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

# The tool decodes RISC-V; the evaluator does not (note: purva omits transcode.c).
transcode: tctool.c transcode.c transcode.h tcfile.h ../purv.h ../purvhost.h
	$(CC) $(CFLAGS) tctool.c transcode.c -o $@
purva: purva.c main.c purva.h transcode.h tcfile.h ../purv.h ../purvhost.h
	$(CC) $(CFLAGS) purva.c main.c -o $@

# RV32IM example elfs (local names; don't clobber purvs's rv32imc builds).
fn.elf: $(EX)/fn.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o fn.o
	$(RVLD) $(LDFLAGS) --entry=0 fn.o -o $@
sigtest.elf: $(EX)/sigtest.S $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) -c $< -o sigtest.o
	$(RVLD) $(LDFLAGS) -e _start sigtest.o -o $@
hello.elf args.elf: %.elf: $(EX)/%.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $*.o
	$(RVLD) $(LDFLAGS) -e _start $*.o -o $@

# elf -> .tc via the transcode tool.
%.tc: %.elf transcode
	./transcode $< $@

test: transcode purva fn.tc sigtest.tc hello.tc args.tc
	@echo "== wasm-style function calls =="
	@printf "square(7)   = "; ./purva --invoke=square --arg=7   fn.tc
	@printf "sum_to(100) = "; ./purva --invoke=sum_to --arg=100 fn.tc
	@printf "fib(20)     = "; ./purva --invoke=fib    --arg=20  fn.tc
	@echo "== userspace program (ecall syscalls) =="
	@./purva --user hello.tc
	@./purva --user args.tc one two
	@echo "== conformance signature dump =="
	@./purva --signature=out.sig sigtest.tc && \
	  echo "signature:" && sed 's/^/  /' out.sig

clean:
	rm -f transcode purva *.o *.elf *.tc out.sig
