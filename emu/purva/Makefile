# purva - AOT pipeline: standalone transcode tool + an evaluator that runs its output.
#
#   make            build ./transcode (tool) and ./purva (evaluator)
#   make test       transcode the RV32IM examples to .img and run them on purva
#   make embed-demo build+run the embedding example (pvcc --rt guest, purvarun.h host)
#
# `transcode in.elf out.img` is the ONLY thing that decodes RISC-V: it lowers a
# whole RV32IM image into a self-contained runnable image (image.h: packed ops +
# rodata + rwdata, no ELF, no symbols). `purva out.img` runs it directly -- it never
# sees a raw instruction, and does not even link transcode.c. `in.elf` must be
# linked with --emit-relocs and purva.ld (separate .text/.rodata, needed so the
# transcoder can find every code address stored as a data word -- vtable slots,
# function-pointer globals -- and patch it to the right op index).

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra

GUESTCC      ?= clang
GUEST_ARCH   ?= --target=riscv32 -march=rv32im -mabi=ilp32
GUEST_CFLAGS ?= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
                -O2 -Wall -Wextra -Werror -Wpedantic
RVLD     := ld.lld
EX       := ../examples
GUEST_LD := purva.ld
LDFLAGS  ?= -m elf32lriscv -T $(GUEST_LD) --emit-relocs

.PHONY: all test clean
all: transcode purva

transcode: tctool.c transcode.c transcode.h image.h ../purv.h ../purvhost.h
	$(CC) $(CFLAGS) tctool.c transcode.c -o $@
# purva.c (the engine) is built -fcf-protection=none: its computed-goto dispatch's
# per-handler endbr landing pads are ~4.5% overhead for no real protection (the indirect
# jumps only ever hit its own fixed label table), and GCC offers no per-function control.
# Scoped to the engine TU; main.c (the driver) keeps CET. See the sqlite Makefile's note.
purva-engine.o: purva.c purva.h transcode.h image.h ../purv.h ../purvhost.h ../purvmemop.h
	$(CC) $(CFLAGS) -fcf-protection=none -c purva.c -o $@
purva: purva-engine.o main.c purva.h transcode.h image.h ../purv.h ../purvhost.h ../purvmemop.h
	$(CC) $(CFLAGS) purva-engine.o main.c -o $@

# RV32IM example elfs (local names; don't clobber purvs's rv32imc builds). Linked
# against purva.ld (separate .text/.rodata) with --emit-relocs.
fn.elf: $(EX)/fn.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o fn.o
	$(RVLD) $(LDFLAGS) --entry=0 fn.o -o $@
sigtest.elf: $(EX)/sigtest.S $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) -c $< -o sigtest.o
	$(RVLD) $(LDFLAGS) -e _start sigtest.o -o $@
hello.elf args.elf: %.elf: $(EX)/%.c $(GUEST_LD)
	$(GUESTCC) $(GUEST_ARCH) $(GUEST_CFLAGS) -c $< -o $*.o
	$(RVLD) $(LDFLAGS) -e _start $*.o -o $@

# elf -> .img via the transcode tool.
%.img: %.elf transcode
	./transcode $< $@

test: transcode purva fn.img sigtest.img hello.img args.img
	@echo "== wasm-style function calls =="
	@printf "square(7)   = "; ./purva fn.img --symbols=fn.elf --invoke=square --arg=7
	@printf "sum_to(100) = "; ./purva fn.img --symbols=fn.elf --invoke=sum_to --arg=100
	@printf "fib(20)     = "; ./purva fn.img --symbols=fn.elf --invoke=fib    --arg=20
	@echo "== userspace program (ecall syscalls) =="
	@./purva hello.img --user
	@./purva args.img --user one two
	@echo "== conformance signature dump =="
	@./purva sigtest.img --symbols=sigtest.elf --signature=out.sig && \
	  echo "signature:" && sed 's/^/  /' out.sig

# --- the embedding demo: pvcc-built guest + purvarun.h host, end to end ------
embed-demo: transcode purva-engine.o
	./pvcc ../examples/embed-app.c --rt -o embed-app.img
	$(CC) $(CFLAGS) ../examples/embed.c purva-engine.o -I. -I.. -o embed-host
	./embed-host embed-app.img

clean:
	rm -f transcode purva *.o *.elf *.img out.sig embed-host embed-app.elf
