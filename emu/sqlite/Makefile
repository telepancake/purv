# Freestanding SQLite on purv.
#
#   make            build the guest (guest.elf) and the host
#   make run        build, then run the SQL demo on the emulator
#   make benchmark  build the heavy bench.c workload native + on purv, time both
#   make clean      remove build outputs (keeps the cached sqlite3.o)
#   make distclean  also remove sqlite3.o and the fetched amalgamation
#
# build.sh / benchmark.sh are the single source of truth for the exact (many)
# compile flags; this Makefile just gives the usual targets. First build fetches
# the SQLite amalgamation and caches the big sqlite3.o.
.PHONY: all run benchmark clean distclean
all:
	./build.sh
run:
	./build.sh run
# Pass a size with `make benchmark SCALE=30` (percent of the default workload).
benchmark:
	./benchmark.sh $(SCALE)
clean:
	rm -f rt.o builtins.o vfs.o guest.o guest.elf host \
	      bench.o guest-bench.elf bench-native native.out native.err purv.out purv.err
distclean: clean
	rm -f sqlite3.o sqlite3-native.o
	rm -rf vendor
