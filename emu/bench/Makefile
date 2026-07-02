# Benchmark corpus for the purv / purvs / purva engines -- real C and C++
# codebases beyond the sqlite stand-in (../sqlite), same recipe throughout:
# one dual-build workload source, the sqlite hostcall ABI (write/exit + the
# malloc group), the sqlite hosts reused unchanged, byte-identical transcripts
# required between native and every engine.
#
#   make            build every workload: guest elfs, purva images, native refs
#   make benchmark  run everything on native + purv + purvs + purva, diff, report
#                   (make benchmark SCALE=30 -- percent of default workload size)
#   make clean      remove build outputs (keeps vendor/)
#   make distclean  also remove the fetched zlib
#
# Workloads:
#   zbench    zlib 1.3.1 (vendored, unmodified, -DZ_SOLO) deflate/inflate rounds
#   richards  the classic OS-simulation benchmark in C++ (virtual dispatch,
#             small-object new/delete) -- see richards.cc
#   deltablue the classic one-way constraint solver in C++ (deep OO call
#             chains, dynamic object graph) -- see deltablue.cc

ZVER := 1.3.1
ZDIR := vendor/zlib-$(ZVER)
ZURL := https://zlib.net/fossils/zlib-$(ZVER).tar.gz
ZOBJ := adler32 crc32 deflate inflate inffast inftrees trees zutil

WORKLOADS := zbench richards deltablue

RES   := $(shell clang -print-resource-dir)/include
SCALE ?= 100

# Toolchains -- same shape as ../sqlite/Makefile: purv guests are RV32IMC on
# purv.ld; purva guests are RV32IM (purva doesn't decode compressed) on
# ../purva/purva.ld with --emit-relocs (the transcoder patches code addresses
# found in data -- vtables land there, so the C++ workloads truly need it).
GUEST_C    := clang --target=riscv32 -mabi=ilp32 -mcmodel=medany \
              -ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
              -nostdinc -isystem $(RES) -I../sqlite -I../sqlite/include
GUEST_CXX  := clang++ --target=riscv32 -mabi=ilp32 -mcmodel=medany \
              -ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
              -fno-rtti -fno-exceptions -fno-threadsafe-statics \
              -Werror=global-constructors \
              -nostdinc -isystem $(RES) -I../sqlite/include
RVLD       := ld.lld -m elf32lriscv -T ../sqlite/purv.ld -e _start --gc-sections
RVLD_PURVA := ld.lld -m elf32lriscv -T ../purva/purva.ld -e _start --gc-sections --emit-relocs
TRANSCODE  := ../purva/transcode

# zlib is compiled -DZ_SOLO everywhere (guest AND native): no stdio, no
# stdlib -- the allocator arrives through zalloc/zfree (zbench.c wires malloc).
ZFLAGS   := -DZ_SOLO -Wno-deprecated-non-prototype
NATIVECC ?= cc
NATIVECXX ?= c++

.PHONY: all benchmark hosts clean distclean

ALL_BUILT := $(foreach w,$(WORKLOADS),$(w)-guest.elf $(w).img $(w)-native)
all: $(ALL_BUILT)

# --- fetch the unmodified zlib on first build --------------------------------
# One stamp stands for the whole extracted tree; the zlib compile rules depend
# on the stamp and name their .c directly, so make never needs a rule for files
# that only exist after the fetch.
vendor/.zlib-fetched:
	mkdir -p vendor
	curl -fsS -o vendor/zlib.tar.gz $(ZURL)
	cd vendor && tar xzf zlib.tar.gz && rm -f zlib.tar.gz
	touch $@

# --- BENCH_SCALE cache: workload objects rebuild when SCALE changes ----------
.scale: FORCE
	@[ "`cat .scale 2>/dev/null`" = "$(SCALE)" ] || echo "$(SCALE)" > .scale
FORCE:

# --- shared freestanding runtime (from ../sqlite, reused unchanged) ----------
rt.o: ../sqlite/rt.c ../sqlite/hostcalls.h
	$(GUEST_C) -march=rv32imc -fno-builtin -c $< -o $@
builtins.o: ../sqlite/builtins.c
	$(GUEST_C) -march=rv32imc -c $< -o $@
rt-cxx.o: rt-cxx.cc
	$(GUEST_CXX) -march=rv32imc -c $< -o $@
rt-purva.o: ../sqlite/rt.c ../sqlite/hostcalls.h
	$(GUEST_C) -march=rv32im -fno-builtin -c $< -o $@
builtins-purva.o: ../sqlite/builtins.c
	$(GUEST_C) -march=rv32im -c $< -o $@
rt-cxx-purva.o: rt-cxx.cc
	$(GUEST_CXX) -march=rv32im -c $< -o $@

# --- zlib objects (guest twice, native once; cached until the source moves) --
ZOBJS_RV32IMC := $(ZOBJ:%=z-%.o)
ZOBJS_RV32IM  := $(ZOBJ:%=z-%-purva.o)
ZOBJS_NATIVE  := $(ZOBJ:%=z-%-native.o)
z-%.o: vendor/.zlib-fetched
	$(GUEST_C) -march=rv32imc $(ZFLAGS) -c $(ZDIR)/$*.c -o $@
z-%-purva.o: vendor/.zlib-fetched
	$(GUEST_C) -march=rv32im $(ZFLAGS) -c $(ZDIR)/$*.c -o $@
z-%-native.o: vendor/.zlib-fetched
	$(NATIVECC) -O2 $(ZFLAGS) -c $(ZDIR)/$*.c -o $@

# --- zbench (C, zlib) ---------------------------------------------------------
zbench.o: zbench.c vendor/.zlib-fetched .scale
	$(GUEST_C) -march=rv32imc $(ZFLAGS) -I$(ZDIR) -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@
zbench-purva.o: zbench.c vendor/.zlib-fetched .scale
	$(GUEST_C) -march=rv32im $(ZFLAGS) -I$(ZDIR) -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@
zbench-guest.elf: rt.o builtins.o zbench.o $(ZOBJS_RV32IMC)
	$(RVLD) $^ -o $@
# rt-purva.o first: transcode requires the entry (_start) at address 0
zbench-purva.elf: rt-purva.o builtins-purva.o zbench-purva.o $(ZOBJS_RV32IM)
	$(RVLD_PURVA) $^ -o $@
zbench-native: zbench.c $(ZOBJS_NATIVE) .scale
	$(NATIVECC) -O2 $(ZFLAGS) -I$(ZDIR) -DBENCH_SCALE=$(SCALE) zbench.c $(ZOBJS_NATIVE) -o $@

# --- richards / deltablue (C++) -----------------------------------------------
%.o: %.cc .scale
	$(GUEST_CXX) -march=rv32imc -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@
%-purva.o: %.cc .scale
	$(GUEST_CXX) -march=rv32im -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@
%-guest.elf: rt.o builtins.o rt-cxx.o %.o
	$(RVLD) $^ -o $@
%-purva.elf: rt-purva.o builtins-purva.o rt-cxx-purva.o %-purva.o
	$(RVLD_PURVA) $^ -o $@
%-native: %.cc .scale
	$(NATIVECXX) -O2 -DBENCH_SCALE=$(SCALE) $< -o $@

# --- elf -> purva image --------------------------------------------------------
$(TRANSCODE):
	$(MAKE) -C ../purva transcode
%.img: %-purva.elf $(TRANSCODE)
	$(TRANSCODE) $< $@

# --- the hosts are ../sqlite's, reused unchanged -------------------------------
hosts:
	$(MAKE) -C ../sqlite host host-purvs host-purva

# --- run everything, require identical transcripts, report ---------------------
benchmark: all hosts
	@set -e; \
	printf '\n  purv bench corpus   (SCALE=%s, transcripts diffed native vs every engine)\n' "$(SCALE)"; \
	for w in $(WORKLOADS); do \
	  ./$$w-native                                    >$$w-native.out 2>$$w-native.err; \
	  ../sqlite/host       $$w-guest.elf --stats      >$$w-purv.out   2>$$w-purv.err; \
	  ../sqlite/host-purvs $$w-guest.elf --stats      >$$w-purvs.out  2>$$w-purvs.err; \
	  ../sqlite/host-purva $$w.img       --stats      >$$w-purva.out  2>$$w-purva.err; \
	  for e in purv purvs purva; do \
	    if ! diff -q $$w-native.out $$w-$$e.out >/dev/null; then \
	      echo "MISMATCH: $$w native vs $$e:"; diff $$w-native.out $$w-$$e.out | head -20; exit 1; fi; \
	  done; \
	  nat=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' $$w-native.err); \
	  rvtext=$$(size $$w-guest.elf | awk 'NR==2{print $$1}'); \
	  pabytes=$$(od -An -tu4 -N4 $$w.img | tr -d ' '); \
	  printf '\n  %s   native %d.%03d s   (%d KiB .text -> %d KiB bytecode)\n' \
	    "$$w" $$((nat/1000)) $$((nat%1000)) $$((rvtext/1024)) $$((pabytes/1024)); \
	  for e in purv purvs purva; do \
	    ms=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' $$w-$$e.err); \
	    mips=$$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' $$w-$$e.err); \
	    insns=$$(sed -n 's/.*insns=\([0-9]*\).*/\1/p' $$w-$$e.err); \
	    slow=$$([ "$$nat" -gt 0 ] && echo $$((ms/nat)) || echo '?'); \
	    printf '    %-6s %5d.%03d s   %7s MIPS  %12s insns   %sx native\n' \
	      "$$e" $$((ms/1000)) $$((ms%1000)) "$$mips" "$$insns" "$$slow"; \
	  done; \
	done; echo

clean:
	rm -f *.o *.elf *.img *-native *.out *.err .scale
distclean: clean
	rm -rf vendor
