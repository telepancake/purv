/*
 * gdbstub.h - optional GDB Remote Serial Protocol server for the purv emulator.
 *
 * Compiled only when PURV_GDBSTUB is defined (see the Makefile's GDB=1 build).
 * It speaks the RSP over an already-connected file descriptor; there is no
 * listening or connection setup here -- a launcher hands the fd in. The stub
 * drives the emulator through the public purv.h API alone.
 */
#ifndef PURV_GDBSTUB_H_
#define PURV_GDBSTUB_H_

#ifdef PURV_GDBSTUB

#include "purv.h"

/* Serve gdb on the connected fd until gdb detaches/kills or the program ends.
 * The engine is stepped from its current PC; *halted (set by the host's hooks
 * when the guest exits) ends a run and is reported to gdb with *exitcode. */
void RiscvEmulatorGdbServe(RiscvEmulatorState_t *state, int fd,
                           const int *halted, const int *exitcode);

/* Hook the host calls from RiscvEmulatorStore (for in-RAM writes) with the bytes
 * about to be overwritten, so the stub can undo them for reverse execution. It
 * records only while an instruction is being stepped under gdb; at all other
 * times (gdb memory pokes, reverse restores) it is a no-op. */
void RiscvEmulatorGdbRecordStore(uint32_t address, const void *old_bytes, uint8_t length);

#endif /* PURV_GDBSTUB */
#endif /* PURV_GDBSTUB_H_ */
