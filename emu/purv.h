/*
 * purv.h - RISC-V (RV32IMC + Zifencei) userspace emulator: public interface.
 *
 * A small, hand-written engine. It runs user-level programs -- no CSRs, no
 * privileged modes, no trap-to-mtvec. Unlike the original atoom design it has no
 * link-time host hooks at all: everything it touches lives in the state. You
 *
 *   - map up to 8 memory regions into it (RiscvEmulatorSetMemory),
 *   - assign the ecall/ebreak/illegal handlers it calls (RiscvEmulatorSet*Handler),
 *   - set the pc, and run a batch with RiscvEmulatorLoop.
 *
 * The VM state is an opaque fixed-size value (the sockaddr pattern); its layout
 * lives in purv.c and this header is the entire public surface.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>

/* Base of the architectural RAM region. The engine's address space is 8 evenly
 * spaced memory regions, each up to 256 MiB, starting here: region i is based at
 * RAM_ORIGIN + i*0x10000000 (0x80000000, 0x90000000, ... 0xF0000000). */
#define RAM_ORIGIN 0x80000000u

/* The VM state, as an opaque fixed-size value -- the `struct sockaddr` pattern.
 * This is a complete type, so you can keep it on the stack, embed it, or copy it
 * as a blob; but its bytes are private. purv.c reinterprets this storage as its
 * real internal state type, which has the same alignment and fits within this
 * size (checked there with _Static_assert). Do not read the fields. */
typedef struct {
    uint64_t _opaque[44];
} RiscvEmulatorState_t;

/* A trap handler: the engine calls it for an ecall, ebreak, or illegal
 * instruction. Read/modify the guest through the accessors below. Return nonzero
 * to stop the run loop and hand control back to the caller of RiscvEmulatorLoop,
 * or zero to keep executing (a fully-serviced syscall returns zero and continues
 * in place; a terminal call like exit returns nonzero). */
typedef int (*RiscvEmulatorTrapFn)(RiscvEmulatorState_t *state);

/* ---- Lifecycle ---- */
/* initial_sp is the absolute starting stack pointer (typically the top of a RAM
 * region). Create starts with no memory mapped and no handlers set. */
RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp);  /* alloc + init */
void                  RiscvEmulatorDestroy(RiscvEmulatorState_t *state);
void                  RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t initial_sp);

/* ---- Memory map ----
 * Assign region `index` (0..7), based at RAM_ORIGIN + index*0x10000000: `ptr` is
 * host storage holding `len` bytes (capped at 256 MiB) for that region, and
 * `writable` says whether stores may modify it. A data load from an unmapped or
 * out-of-bounds address reads zero; a store to a read-only, unmapped, or
 * out-of-bounds address is dropped. (Pass ptr == NULL to clear a region.) */
void RiscvEmulatorSetMemory(RiscvEmulatorState_t *state, int index,
                            void *ptr, uint32_t len, int writable);
/* Peek/poke guest memory through the mapped regions (same bounds/writability
 * rules; OOB reads yield zero). For hosts/debuggers that don't own the storage. */
void RiscvEmulatorReadMemory(const RiscvEmulatorState_t *state, uint32_t addr,
                             void *dst, uint32_t len);
void RiscvEmulatorWriteMemory(RiscvEmulatorState_t *state, uint32_t addr,
                              const void *src, uint32_t len);

/* ---- Trap handlers (assign the function the engine calls; NULL = default) ----
 * ecall   default: nop, keep running (no syscall ABI).
 * ebreak  default: stop.
 * illegal default: stop. */
void RiscvEmulatorSetEcallHandler(RiscvEmulatorState_t *state, RiscvEmulatorTrapFn fn);
void RiscvEmulatorSetEbreakHandler(RiscvEmulatorState_t *state, RiscvEmulatorTrapFn fn);
void RiscvEmulatorSetIllegalHandler(RiscvEmulatorState_t *state, RiscvEmulatorTrapFn fn);

/* ---- The VM ----
 * Execute up to `max` instructions and return the number actually executed.
 *
 * The program counter lives in the state: execution resumes at the current pc
 * (set it with RiscvEmulatorSetProgramCounter before the first call) and the pc
 * is left at the next instruction to run, so re-entering simply continues.
 *
 * Instructions are fetched ONLY from the caller-provided code image: `code`
 * points at the host bytes for guest address `code_base` and spans `code_len`
 * bytes. The engine reads instruction words straight out of this window; a guest
 * pc outside [code_base, code_base+code_len) ends the batch and is left in the
 * state. A short return (fewer than `max`) with the pc outside the window is how
 * you detect that. (Data loads/stores go through the mapped regions above; only
 * instruction fetch uses this window.)
 *
 * Loop also stops when `max` is reached, or when an ecall/ebreak/illegal handler
 * returns nonzero. A halt the host decides by other means (e.g. polling a
 * tohost word it owns) is invisible to the engine; run Loop in bounded slices
 * and check between them. */
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *state,
                           const uint8_t *code, uint32_t code_len, uint32_t code_base,
                           uint64_t max);

/* ---- Register / program-counter access (x0..x31; x0 reads 0, ignores writes) ---- */
uint32_t RiscvEmulatorGetRegister(const RiscvEmulatorState_t *state, int index);
void     RiscvEmulatorSetRegister(RiscvEmulatorState_t *state, int index, uint32_t value);
uint32_t RiscvEmulatorGetProgramCounter(const RiscvEmulatorState_t *state);
uint32_t RiscvEmulatorGetNextProgramCounter(const RiscvEmulatorState_t *state);
void     RiscvEmulatorSetProgramCounter(RiscvEmulatorState_t *state, uint32_t pc);

/* ---- Decode details a handler may need ---- */
uint32_t RiscvEmulatorGetInstruction(const RiscvEmulatorState_t *state);      /* raw word */
void     RiscvEmulatorRaiseIllegalInstruction(RiscvEmulatorState_t *state);   /* force illegal */

#endif /* PURV_H_ */
