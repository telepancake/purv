#!/bin/sh
# Build the freestanding SQLite guest for purv, plus the host driver.
#   ./build.sh          # build everything
#   ./build.sh run      # build + run
set -e
cd "$(dirname "$0")"

SQLDIR=vendor/sqlite-amalgamation-3470000
RES="$(clang -print-resource-dir)/include"

# Fetch the unmodified SQLite amalgamation on first build (not vendored in git).
if [ ! -f "$SQLDIR/sqlite3.c" ]; then
  echo "[0/6] fetching SQLite amalgamation 3.47.0"
  mkdir -p vendor
  curl -fsS -o vendor/amalg.zip \
    https://www.sqlite.org/2024/sqlite-amalgamation-3470000.zip
  ( cd vendor && unzip -oq amalg.zip && rm -f amalg.zip )
fi

# SQLite configured for a bare-metal, in-memory-only, no-float, single-thread
# build -- the option set from sqlite.org that reduces it to the 7 core libc
# functions (+ malloc) and a custom VFS.
SQOPTS="-DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 \
-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION \
-DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
-DSQLITE_DQS=0 -DSQLITE_OMIT_FLOATING_POINT -DSQLITE_OMIT_DATETIME_FUNCS -DNDEBUG"

# Freestanding riscv32: no stdlib, our own headers only, gc unused functions.
GUEST="clang --target=riscv32 -march=rv32imc -mabi=ilp32 -mcmodel=medany \
-ffreestanding -nostdlib -O2 -ffunction-sections -fdata-sections \
-nostdinc -isystem $RES -Iinclude -I$SQLDIR"

echo "[1/6] runtime (rt.c, -fno-builtin so mem/str aren't self-referential)"
$GUEST -fno-builtin -c rt.c       -o rt.o
echo "[2/6] compiler builtins (64-bit div, atomics, soft-double shim)"
$GUEST -c builtins.c              -o builtins.o
echo "[3/6] minimal VFS"
$GUEST $SQOPTS -c vfs.c           -o vfs.o
echo "[4/6] guest test program"
$GUEST $SQOPTS -c guest.c         -o guest.o
if [ ! -f sqlite3.o ] || [ "$SQLDIR/sqlite3.c" -nt sqlite3.o ]; then
  echo "[5/6] sqlite3.c (big; cached after first build)"
  $GUEST $SQOPTS -Wno-implicit-int -c "$SQLDIR/sqlite3.c" -o sqlite3.o
else
  echo "[5/6] sqlite3.o cached"
fi
echo "[6/6] link guest.elf + build host"
ld.lld -m elf32lriscv --image-base=0x80000000 -e _start --gc-sections \
  rt.o builtins.o vfs.o guest.o sqlite3.o -o guest.elf
gcc -std=c11 -O2 -Wall -Wextra -Wno-packed-bitfield-compat host.c ../purv.c -I.. -o host

echo "built: $(ls -la guest.elf host | awk '{print $5, $9}' | tr '\n' '  ')"
if [ "$1" = run ]; then echo "=== run ==="; ./host guest.elf; echo "[exit $?]"; fi
