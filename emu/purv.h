/*
 * purv.h - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: public interface.
 *
 * A small, hand-written engine in the spirit of atoomnetmarc/RISC-V-emulator
 * (Apache-2.0, (c) Marc Ketel): the host owns all policy and reaches the engine
 * only through the hooks below; the engine knows nothing of the memory map. The
 * VM state is opaque (its layout lives in purv.c) and this header is the entire
 * public surface.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>

/* Base of the architectural RAM region (where programs are loaded). */
#define RAM_ORIGIN 0x80000000u

/* Opaque emulator state. Create it with RiscvEmulatorCreate(); the layout is
 * private to purv.c. */
typedef struct RiscvEmulatorState RiscvEmulatorState_t;

/* ---- Lifecycle ---- */
/* initial_sp is the absolute value the stack pointer starts at (typically the
 * top of the host's RAM region); the engine sets sp to it verbatim and otherwise
 * knows nothing of the host's memory size or layout. */
RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp);  /* alloc + init */
void                  RiscvEmulatorDestroy(RiscvEmulatorState_t *state);
void                  RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t initial_sp);

/* ---- The VM: execute the instruction at pc, return the next pc. ---- */
uint32_t RiscvEmulatorLoop(RiscvEmulatorState_t *state, uint32_t pc);

/* ---- Register / program-counter access (x0..x31; x0 reads 0, ignores writes) ---- */
uint32_t RiscvEmulatorGetRegister(const RiscvEmulatorState_t *state, int index);
void     RiscvEmulatorSetRegister(RiscvEmulatorState_t *state, int index, uint32_t value);
uint32_t RiscvEmulatorGetProgramCounter(const RiscvEmulatorState_t *state);
uint32_t RiscvEmulatorGetNextProgramCounter(const RiscvEmulatorState_t *state);
void     RiscvEmulatorSetProgramCounter(RiscvEmulatorState_t *state, uint32_t pc);

/* ---- Decode/trap details a hook may need ---- */
uint32_t RiscvEmulatorGetInstruction(const RiscvEmulatorState_t *state);      /* raw word */
uint16_t RiscvEmulatorGetCsrNumber(const RiscvEmulatorState_t *state);        /* CSR being accessed */
uint32_t RiscvEmulatorGetTrapVectorBase(const RiscvEmulatorState_t *state);   /* mtvec.base */
void     RiscvEmulatorRaiseIllegalInstruction(RiscvEmulatorState_t *state);
void     RiscvEmulatorClearTrap(RiscvEmulatorState_t *state);                 /* consume pending trap (e.g. host-handled ecall) */

/* ---- Hooks YOU implement (atoom's whole "API"): the engine reaches your
 *      memory map and trap policy only through these. ---- */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
/* OPTIONAL fetch fast-path. The engine fetches one instruction per step through
 * RiscvEmulatorLoad by default; for a host whose code lives in flat, directly
 * addressable storage, implementing this lets the engine read instruction
 * half-words straight from a host pointer instead of paying a hook call +
 * memcpy on every fetch. Return a host pointer at which `address` is readable
 * and set *available to the number of contiguous bytes valid from there (the
 * engine caches the window and only re-calls on a miss); return NULL to fall
 * back to RiscvEmulatorLoad. The symbol is weak: a host that does not define it
 * keeps the original behaviour with no code change. */
const uint8_t *RiscvEmulatorGetFetchWindow(uint32_t address, uint32_t *available);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
/* Supply backing storage for a CSR the engine does not implement (the engine
 * reads/writes through the returned pointer); return NULL to raise illegal. */
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *state, uint16_t csrnum);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

#endif /* PURV_H_ */
