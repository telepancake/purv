# Freestanding SQLite on purv -- a real Makefile (file targets + prerequisites,
# so everything rebuilds incrementally and only when its inputs change).
#
#   make            build the demo guest (guest.elf) and the host
#   make run        build, then run the SQL demo on the emulator
#   make benchmark  build bench.c native + on purv, run both, diff, report timings
#                   (size with `make benchmark SCALE=30` -- percent of default)
#   make host-pgo   build the host profile-guided: instrument, train on the bench
#                   guest, rebuild with the profile (~+8% on the hot dispatch loop)
#   make clean      remove build outputs (keeps the cached big sqlite3 objects)
#   make distclean  also remove the cached objects and the fetched amalgamation
#
# The big sqlite3.c objects (guest and native) are cached: they only recompile
# when the amalgamation source is newer. The first build fetches the amalgamation.

SQLVER  := 3470000
SQLDIR  := vendor/sqlite-amalgamation-$(SQLVER)
SQLURL  := https://www.sqlite.org/2024/sqlite-amalgamation-$(SQLVER).zip
# presence of this file => the amalgamation has been fetched.
AMALG   := $(SQLDIR)/sqlite3.c
RES     := $(shell clang -print-resource-dir)/include
# benchmark size, percent of default. (No inline comment: make would fold the
# trailing spaces into the value, breaking the .scale comparison below.)
SCALE   ?= 10

# Shared SQLite options: the freestanding minimum from sqlite.org. The native
# build (for the benchmark) drops only SQLITE_OS_OTHER, so it uses this box's
# normal VFS instead of the guest's stub one -- immaterial for :memory:. Every
# other option matches, so the SQL behaves identically (notably no floats).
COMMON  := -DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_DEFAULT_MEMSTATUS=0 \
           -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
           -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0 \
           -DSQLITE_OMIT_FLOATING_POINT -DSQLITE_OMIT_DATETIME_FUNCS -DNDEBUG
SQOPTS  := -DSQLITE_OS_OTHER=1 $(COMMON)

# Guest toolchain: freestanding riscv32, our headers only, gc unused functions.
GUEST   := clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany \
           -ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
           -nostdinc -isystem $(RES) -Iinclude -I$(SQLDIR)
RVLD    := ld.lld -m elf32lriscv -T purv.ld -e _start --gc-sections

# purva's guest toolchain: RV32IM only (no compressed -- purva doesn't decode C),
# linked with purva.ld (separate .text/.rodata, see ../purva/purva.ld) and
# --emit-relocs, which the transcode tool needs to find every code address stored
# as a data word (vtable slots, function-pointer globals) and patch it.
GUEST_PURVA := clang --target=riscv32 -march=rv32im -mabi=ilp32 -mcmodel=medany \
               -ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
               -nostdinc -isystem $(RES) -Iinclude -I$(SQLDIR)
RVLD_PURVA  := ld.lld -m elf32lriscv -T ../purva/purva.ld -e _start --gc-sections --emit-relocs
TRANSCODE   := ../purva/transcode

HOSTCC    ?= gcc
# -g: keep debug line info (does not change -O2 codegen) so a profiler
# (callgrind_annotate --auto=yes, perf) can map back to purv.c / host.c source
# lines instead of showing `???`. host.c and purv.c stay separate translation
# units (no -flto), so the engine's RiscvEmulatorLoop is profiled as its own
# function rather than being inlined into main.
HOSTFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-packed-bitfield-compat
NATIVECC  ?= cc

# shared prerequisites for the guest objects.
GHDRS := hostcalls.h $(AMALG)

.PHONY: all run benchmark clean distclean FORCE
all: guest.elf host

# --- fetch the unmodified SQLite amalgamation on first build ----------------
$(AMALG):
	mkdir -p vendor
	curl -fsS -o vendor/amalg.zip $(SQLURL)
	cd vendor && unzip -oq amalg.zip && rm -f amalg.zip

# --- guest objects ----------------------------------------------------------
# rt.c is compiled -fno-builtin so its mem/str defns aren't turned into calls to
# themselves.
rt.o: rt.c hostcalls.h
	$(GUEST) -fno-builtin -c $< -o $@
builtins.o: builtins.c
	$(GUEST) -c $< -o $@
vfs.o: vfs.c $(GHDRS)
	$(GUEST) $(SQOPTS) -c $< -o $@
guest.o: guest.c $(GHDRS)
	$(GUEST) $(SQOPTS) -c $< -o $@
# bench.o (and bench-native) depend on .scale so a changed SCALE forces a rebuild.
bench.o: bench.c $(GHDRS) .scale
	$(GUEST) $(SQOPTS) -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@

# The big one (cached). Benign warnings under OMIT_FLOATING_POINT (double literals
# folded to i64) and no-lock-free-atomics are silenced; not our code to fix.
sqlite3.o: $(AMALG)
	$(GUEST) $(SQOPTS) -Wno-implicit-int -Wno-literal-conversion \
	  -Wno-constant-conversion -Wno-atomic-alignment -c $< -o $@

# --- links ------------------------------------------------------------------
guest.elf: rt.o builtins.o vfs.o guest.o sqlite3.o purv.ld
	$(RVLD) $(filter %.o,$^) -o $@
guest-bench.elf: rt.o builtins.o vfs.o bench.o sqlite3.o purv.ld
	$(RVLD) $(filter %.o,$^) -o $@

# --- purva guest objects (RV32IM) --------------------------------------------
rt-purva.o: rt.c hostcalls.h
	$(GUEST_PURVA) -fno-builtin -c $< -o $@
builtins-purva.o: builtins.c
	$(GUEST_PURVA) -c $< -o $@
vfs-purva.o: vfs.c $(GHDRS)
	$(GUEST_PURVA) $(SQOPTS) -c $< -o $@
bench-purva.o: bench.c $(GHDRS) .scale
	$(GUEST_PURVA) $(SQOPTS) -DPURV_GUEST -DBENCH_SCALE=$(SCALE) -c $< -o $@
sqlite3-purva.o: $(AMALG)
	$(GUEST_PURVA) $(SQOPTS) -Wno-implicit-int -Wno-literal-conversion \
	  -Wno-constant-conversion -Wno-atomic-alignment -c $< -o $@

guest-bench-purva.elf: rt-purva.o builtins-purva.o vfs-purva.o bench-purva.o sqlite3-purva.o ../purva/purva.ld
	$(RVLD_PURVA) $(filter %.o,$^) -o $@

# elf -> purva image, via the standalone transcode tool (the only thing that
# decodes RISC-V in this whole pipeline).
guest-bench-purva.img: guest-bench-purva.elf $(TRANSCODE)
	$(TRANSCODE) guest-bench-purva.elf $@

# --- host (depends only on the purv engine) ---------------------------------
host: host.c ../purv.c ../purv.h hostcalls.h
	$(HOSTCC) $(HOSTFLAGS) host.c ../purv.c -I.. -o $@

# Same host, built against purvs (the monadic decode-ahead + threaded-eval
# engine) instead of purv, so the benchmark runs the identical workload through
# both and compares their timings. purvs shares purv's public API, so -DPURV_PURVS
# only swaps the engine header; the host code is otherwise the plain (non-tagged)
# path. host.c stays a separate TU from the engine (no -flto) so purvs's
# RiscvEmulatorLoop is profiled as its own function.
host-purvs: host.c ../purvs/purvs.c ../purvs/purvs.h hostcalls.h
	$(HOSTCC) $(HOSTFLAGS) -DPURV_PURVS host.c ../purvs/purvs.c -I.. -I../purvs -o $@

# purva (the AOT-transcoded engine): host-purva loads a purva IMAGE directly (no
# ELF, no decode at run time -- see host-purva.c) and drives the same evaluator
# (purva.c) the standalone purva binary uses. transcode lives in ../purva; delegate
# to its own (incremental) build via FORCE so a stale transcode binary here is
# rebuilt when its sources change -- without FORCE this recipe runs only when the
# binary is absent, and an out-of-date transcode would silently mis-transcode.
$(TRANSCODE): FORCE
	$(MAKE) -C ../purva transcode
# The engine TU is built -fcf-protection=none: purva's dispatch is a computed goto
# whose indirect jumps only ever land on its own fixed table of internal labels, so
# CET's endbr landing pad on every one of the 50+ handlers is pure overhead (~4.5% of
# the hot loop -- one endbr per dispatch) for no real protection. GCC has no per-
# function control (the optimize attribute rejects it; nocf_check drops only the entry
# pad, not the label pads), so it's scoped to this TU. The driver keeps CET.
purva-engine.o: ../purva/purva.c ../purva/purva.h ../purva/transcode.h ../purva/image.h ../purv.h ../purvhost.h
	$(HOSTCC) $(HOSTFLAGS) -fcf-protection=none -c ../purva/purva.c -o $@
host-purva: host-purva.c purva-engine.o ../purva/purva.h ../purva/image.h hostcalls.h
	$(HOSTCC) $(HOSTFLAGS) host-purva.c purva-engine.o -I.. -I../purva -o $@

# Profile-guided host (make host-pgo). Three phases: build instrumented, train on
# the bench guest, rebuild with the profile so the compiler lays out and allocates
# the hot dispatch loop from real execution data instead of static heuristics
# (~+8% vs plain -O2 here). The engine source is untouched -- this is purely a
# build option. Both compiles use the same output name so the .gcda profile keys
# match. -fprofile-correction tolerates the (single-threaded, so absent) races.
PGODIR := pgo-data
host-pgo: host.c ../purv.c ../purv.h hostcalls.h guest-bench.elf
	rm -rf $(PGODIR)
	$(HOSTCC) $(HOSTFLAGS) -fprofile-generate=$(PGODIR) host.c ../purv.c -I.. -o $@
	./$@ guest-bench.elf --max-insns=200000000 >/dev/null 2>&1 || true   # train (cap-exit is fine)
	$(HOSTCC) $(HOSTFLAGS) -fprofile-use=$(PGODIR) -fprofile-correction host.c ../purv.c -I.. -o $@

# --- native build of the same workload (for the benchmark) ------------------
sqlite3-native.o: $(AMALG)
	$(NATIVECC) -O2 -w $(COMMON) -I$(SQLDIR) -c $< -o $@
bench-native: bench.c sqlite3-native.o $(AMALG) .scale
	$(NATIVECC) -O2 $(COMMON) -I$(SQLDIR) -DBENCH_SCALE=$(SCALE) bench.c sqlite3-native.o -o $@

# A stamp that holds the current SCALE; rewritten (touched) only when SCALE
# changes, so bench.o / bench-native rebuild exactly when the size changes.
.scale: FORCE
	@[ -f $@ ] && [ "$$(cat $@)" = "$(SCALE)" ] || echo "$(SCALE)" > $@
FORCE:

# --- run targets ------------------------------------------------------------
run: host guest.elf
	@echo "=== run ==="; ./host guest.elf; echo "[exit $$?]"

# Build the workload native, on purv, on purvs, and on purva; run each, require all
# transcripts byte-identical, and report timings + MIPS. purv and purvs run the very
# same guest-bench.elf (RV32IMC), so that comparison isolates the engine; purva runs
# its own guest-bench-purva.img (RV32IM, no compressed -- purva doesn't decode C, see
# ../purva/), a SEPARATE compile of the identical C source, so its instruction count
# is reported separately rather than asserted equal (a different -march can make the
# compiler choose a different operation sequence even from the same source).
benchmark: host host-purvs host-purva guest-bench.elf guest-bench-purva.img bench-native
	@./bench-native                        >native.out 2>native.err
	@./host       guest-bench.elf --stats  >purv.out   2>purv.err
	@./host-purvs guest-bench.elf --stats  >purvs.out  2>purvs.err
	@./host-purva guest-bench-purva.img --stats >purva.out 2>purva.err
	@if ! diff -q native.out purv.out >/dev/null; then \
	   echo "MISMATCH: native vs purv transcripts differ:"; \
	   diff native.out purv.out | head -40; exit 1; fi
	@if ! diff -q purv.out purvs.out >/dev/null; then \
	   echo "MISMATCH: purv vs purvs transcripts differ:"; \
	   diff purv.out purvs.out | head -40; exit 1; fi
	@if ! diff -q purv.out purva.out >/dev/null; then \
	   echo "MISMATCH: purv vs purva transcripts differ:"; \
	   diff purv.out purva.out | head -40; exit 1; fi
	@nat=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' native.err); \
	 pv=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' purv.err); \
	 pvm=$$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' purv.err); \
	 ps=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' purvs.err); \
	 psm=$$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' purvs.err); \
	 pa=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' purva.err); \
	 pam=$$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' purva.err); \
	 painsns=$$(sed -n 's/.*insns=\([0-9]*\).*/\1/p' purva.err); \
	 insns=$$(sed -n 's/.*insns=\([0-9]*\).*/\1/p' purv.err); \
	 lines=$$(wc -l <purv.out | tr -d ' '); \
	 rvtext=$$(size guest-bench.elf 2>/dev/null | awk 'NR==2{print $$1}'); \
	 pabytes=$$(od -An -tu4 -N4 guest-bench-purva.img | tr -d ' '); \
	 rvkib=$$(( $${rvtext:-0} / 1024 )); pakib=$$(( $${pabytes:-0} / 1024 )); \
	 echo; \
	 printf '  SQLite freestanding benchmark   (BENCH_SCALE=%s, %s-line transcript, identical)\n\n' "$(SCALE)" "$$lines"; \
	 printf '    %-20s %5d.%03d s\n' 'native (host)' $$((nat/1000)) $$((nat%1000)); \
	 printf '    %-20s %5d.%03d s   %s MIPS, %s instructions  (%s KiB .text)\n' 'purv (RV32IMC)' $$((pv/1000)) $$((pv%1000)) "$$pvm" "$$insns" "$$rvkib"; \
	 printf '    %-20s %5d.%03d s   %s MIPS, %s instructions  (%s KiB .text)\n' 'purvs (RV32IMC)' $$((ps/1000)) $$((ps%1000)) "$$psm" "$$insns" "$$rvkib"; \
	 printf '    %-20s %5d.%03d s   %s MIPS, %s instructions  (%s KiB bytecode)\n' 'purva (RV32IM)' $$((pa/1000)) $$((pa%1000)) "$$pam" "$$painsns" "$$pakib"; \
	 [ "$$nat" -gt 0 ] && printf '    %-20s purv %dx, purvs %dx, purva %dx\n' 'slowdown vs native' $$((pv/nat)) $$((ps/nat)) $$((pa/nat)); \
	 echo

clean:
	rm -f rt.o builtins.o vfs.o guest.o bench.o guest.elf guest-bench.elf \
	      rt-purva.o builtins-purva.o vfs-purva.o bench-purva.o sqlite3-purva.o \
	      guest-bench-purva.elf guest-bench-purva.img \
	      host host-purvs host-purva purva-engine.o host-pgo bench-native .scale \
	      native.out native.err purv.out purv.err purvs.out purvs.err purva.out purva.err
	rm -rf $(PGODIR)
distclean: clean
	rm -f sqlite3.o sqlite3-native.o sqlite3-purva.o
	rm -rf vendor
