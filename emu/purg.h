/*
 * purg.h - RISC-V (RV32IMC + Zifencei) userspace emulator: public interface (generated).
 *
 * The implementation is purg.c (generated from atoomnetmarc/RISC-V-emulator,
 * Apache-2.0, (c) Marc Ketel, with the privileged Zicsr surface baked off). This
 * header is the entire public surface: an opaque VM state, the run loop,
 * register/PC access, and the memory/userspace-trap hooks you implement. None of
 * the engine's internal types are exposed.
 */
#ifndef PURG_H_
#define PURG_H_

#include <stdint.h>

/* Base of the architectural RAM region (where programs are loaded). */
#define RAM_ORIGIN 0x80000000u

/* Opaque emulator state. Create it with RiscvEmulatorCreate(); the layout is
 * private to purg.c. */
typedef struct RiscvEmulatorState RiscvEmulatorState_t;

/* ---- Lifecycle ---- */
/* initial_sp is the absolute value the stack pointer starts at (typically the
 * top of the host's RAM region); the engine sets sp to it verbatim and otherwise
 * knows nothing of the host's memory size or layout. */
RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp);  /* alloc + init */
void                  RiscvEmulatorDestroy(RiscvEmulatorState_t *state);
void                  RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t initial_sp);

/* ---- The VM: execute one instruction. ---- */
void RiscvEmulatorLoop(RiscvEmulatorState_t *state);

/* ---- Register / program-counter access (x0..x31; x0 reads 0, ignores writes) ---- */
uint32_t RiscvEmulatorGetRegister(const RiscvEmulatorState_t *state, int index);
void     RiscvEmulatorSetRegister(RiscvEmulatorState_t *state, int index, uint32_t value);
uint32_t RiscvEmulatorGetProgramCounter(const RiscvEmulatorState_t *state);
uint32_t RiscvEmulatorGetNextProgramCounter(const RiscvEmulatorState_t *state);
void     RiscvEmulatorSetProgramCounter(RiscvEmulatorState_t *state, uint32_t pc);

/* ---- Decode details a hook may need ---- */
uint32_t RiscvEmulatorGetInstruction(const RiscvEmulatorState_t *state);      /* raw word */
void     RiscvEmulatorRaiseIllegalInstruction(RiscvEmulatorState_t *state);   /* a hook can force illegal */

/* ---- Hooks YOU implement (atoom's whole "API"): the engine reaches your
 *      memory map and userspace traps only through these. ---- */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
/* ecall is a service request to the host (a syscall); ebreak is a debugger
 * breakpoint. The engine hands control straight to these -- nothing vectors. */
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

#endif /* PURG_H_ */
