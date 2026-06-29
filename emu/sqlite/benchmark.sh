#!/bin/sh
# Benchmark the freestanding SQLite workload (bench.c) on two machines:
#
#   - the host CPU, native: bench.c linked against sqlite3.c built for this box;
#   - the emulated RV32 CPU: the same bench.c, same SQLite, run on purv.
#
# Both are built from the SAME bench.c with the SAME SQLite compile options, so
# the two transcripts must come out byte-identical -- the script diffs them and
# fails if they don't. Then it reports wall-clock time for each and the slowdown.
#
#   ./benchmark.sh                 # default workload (BENCH_SCALE=100)
#   ./benchmark.sh 30              # lighter/faster (percent of the default size)
#   BENCH_SCALE=200 ./benchmark.sh # heavier/longer
#
set -e
cd "$(dirname "$0")"

SCALE="${1:-${BENCH_SCALE:-100}}"
SQLDIR=vendor/sqlite-amalgamation-3470000
RES="$(clang -print-resource-dir)/include"

# Shared SQLite options (the freestanding minimum). The native build drops only
# SQLITE_OS_OTHER, so it uses this box's normal VFS instead of the guest's stub
# one; :memory: makes that immaterial. Every other option matches, so the SQL
# behaves identically -- in particular OMIT_FLOATING_POINT on both sides.
COMMON="-DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_DEFAULT_MEMSTATUS=0 \
-DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
-DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0 \
-DSQLITE_OMIT_FLOATING_POINT -DSQLITE_OMIT_DATETIME_FUNCS -DNDEBUG"
GUESTOPTS="-DSQLITE_OS_OTHER=1 $COMMON"

# build.sh does the shared, cached heavy lifting: fetch the amalgamation, build
# the guest runtime objects (rt/builtins/vfs), the cached guest sqlite3.o, and
# the purv host (with --stats). Reuse it rather than duplicate all that here.
echo "[1/5] shared guest objects + host (via build.sh)"
./build.sh >/dev/null

GUEST="clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany \
-ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
-nostdinc -isystem $RES -Iinclude -I$SQLDIR"

echo "[2/5] guest bench.elf (RV32, scale=$SCALE)"
$GUEST $GUESTOPTS -DPURV_GUEST -DBENCH_SCALE="$SCALE" -c bench.c -o bench.o
ld.lld -m elf32lriscv --image-base=0x80000000 -e _start --gc-sections \
  rt.o builtins.o vfs.o bench.o sqlite3.o -o guest-bench.elf

echo "[3/5] native sqlite3 (cached) + native bench (scale=$SCALE)"
# -w: the vendored amalgamation warns under OMIT_FLOATING_POINT (double literals
# folded to int64); harmless, and not our code to fix.
if [ ! -f sqlite3-native.o ] || [ "$SQLDIR/sqlite3.c" -nt sqlite3-native.o ]; then
  cc -O2 -w $COMMON -I"$SQLDIR" -c "$SQLDIR/sqlite3.c" -o sqlite3-native.o
fi
cc -O2 -DBENCH_SCALE="$SCALE" -I"$SQLDIR" bench.c sqlite3-native.o -o bench-native

echo "[4/5] run native, then purv"
./bench-native            >native.out  2>native.err
./host guest-bench.elf --stats >purv.out 2>purv.err

echo "[5/5] compare + report"
if ! diff -q native.out purv.out >/dev/null; then
  echo "MISMATCH: native and purv transcripts differ!" >&2
  diff native.out purv.out | head -40 >&2
  exit 1
fi

nat_ms=$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' native.err)
purv_ms=$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' purv.err)
purv_mips=$(sed -n 's/.*mips=\([0-9.]*\).*/\1/p' purv.err)
purv_insns=$(sed -n 's/.*insns=\([0-9]*\).*/\1/p' purv.err)
lines=$(wc -l <purv.out | tr -d ' ')

awk -v n="$nat_ms" -v p="$purv_ms" -v mips="$purv_mips" -v insns="$purv_insns" \
    -v lines="$lines" -v scale="$SCALE" 'BEGIN {
  printf "\n";
  printf "  SQLite freestanding benchmark   (BENCH_SCALE=%s, %d-line transcript, identical)\n\n", scale, lines;
  printf "    %-16s %10s   %s\n", "", "wall time", "throughput";
  printf "    %-16s %8.3f s\n", "native (host)", n/1000.0;
  printf "    %-16s %8.3f s   %.1f MIPS, %.2f G instructions\n", "purv (RV32 emu)", p/1000.0, mips, insns/1e9;
  if (n > 0) printf "    %-16s %8.1fx\n", "slowdown", p/n;
  printf "\n";
}'
