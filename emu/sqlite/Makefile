# Freestanding SQLite on purv -- a real Makefile (file targets + prerequisites,
# so everything rebuilds incrementally and only when its inputs change).
#
#   make            build the demo guest (guest.elf) and the host
#   make run        build, then run the SQL demo on the emulator
#   make benchmark  build bench.c native + on purv, run both, diff, report timings
#                   (size with `make benchmark SCALE=30` -- percent of default)
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
SCALE   ?= 100

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

# --- host (depends only on the purv engine) ---------------------------------
host: host.c ../purv.c ../purv.h hostcalls.h
	$(HOSTCC) $(HOSTFLAGS) host.c ../purv.c -I.. -o $@

# Same host, built against the tagged-memory engine (purvs) instead of purv, so
# the benchmark can run the identical workload through the tag-tracking datapath
# and show its overhead. -DPURV_TAGGED selects the tagged hook signatures in
# host.c (implemented permissively: untagged code runs, tags aren't enforced).
host-purvs: host.c ../purvs/purvs.c ../purvs/purvs.h hostcalls.h
	$(HOSTCC) $(HOSTFLAGS) -DPURV_TAGGED host.c ../purvs/purvs.c -I.. -I../purvs -o $@

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

# Build the workload native and on purv, run each, require byte-identical
# transcripts, and report timings + MIPS. (The purvs tagged-engine comparison is
# disabled for now; build host-purvs by hand if you want it.)
benchmark: host guest-bench.elf bench-native
	@./bench-native                   >native.out 2>native.err
	@./host       guest-bench.elf --stats >purv.out  2>purv.err
	@if ! diff -q native.out purv.out >/dev/null; then \
	   echo "MISMATCH: native vs purv transcripts differ:"; \
	   diff native.out purv.out | head -40; exit 1; fi
	@nat=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' native.err); \
	 pv=$$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' purv.err); \
	 pvm=$$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' purv.err); \
	 insns=$$(sed -n 's/.*insns=\([0-9]*\).*/\1/p' purv.err); \
	 lines=$$(wc -l <purv.out | tr -d ' '); \
	 echo; \
	 printf '  SQLite freestanding benchmark   (BENCH_SCALE=%s, %s-line transcript, identical)\n\n' "$(SCALE)" "$$lines"; \
	 printf '    %-20s %5d.%03d s\n' 'native (host)' $$((nat/1000)) $$((nat%1000)); \
	 printf '    %-20s %5d.%03d s   %s MIPS, %s instructions\n' 'purv (RV32)' $$((pv/1000)) $$((pv%1000)) "$$pvm" "$$insns"; \
	 [ "$$nat" -gt 0 ] && printf '    %-20s purv %dx\n' 'slowdown vs native' $$((pv/nat)); \
	 echo

clean:
	rm -f rt.o builtins.o vfs.o guest.o bench.o guest.elf guest-bench.elf \
	      host host-purvs bench-native .scale \
	      native.out native.err purv.out purv.err purvs.out purvs.err
distclean: clean
	rm -f sqlite3.o sqlite3-native.o
	rm -rf vendor
