# Freestanding SQLite on purv.
#
#   make            build the guest (guest.elf) and the host
#   make run        build, then run the SQL demo on the emulator
#   make clean      remove build outputs (keeps the cached sqlite3.o)
#   make distclean  also remove sqlite3.o and the fetched amalgamation
#
# build.sh is the single source of truth for the exact (many) compile flags;
# this Makefile just gives the usual targets. First build fetches the SQLite
# amalgamation and caches the big sqlite3.o.
.PHONY: all run clean distclean
all:
	./build.sh
run:
	./build.sh run
clean:
	rm -f rt.o builtins.o vfs.o guest.o guest.elf host
distclean: clean
	rm -f sqlite3.o
	rm -rf vendor
