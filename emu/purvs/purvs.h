/*
 * purvs.h - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: public interface.
 *
 * The implementation is purv.c (generated from atoomnetmarc/RISC-V-emulator,
 * Apache-2.0, (c) Marc Ketel). This header is the entire public surface: an
 * opaque VM state, the run loop, register/PC access, and the memory/trap hooks
 * you implement. None of the engine's internal types are exposed.
 */
#ifndef PURVS_H_
#define PURVS_H_

#include <stdint.h>

/* Base of the architectural RAM region (where programs are loaded). */
#define RAM_ORIGIN 0x80000000u

/* Opaque emulator state. Create it with RiscvEmulatorCreate(); the layout is
 * private to purv.c. */
typedef struct RiscvEmulatorState RiscvEmulatorState_t;

/* ---- Lifecycle ---- */
RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t ram_length);  /* alloc + init */
void                  RiscvEmulatorDestroy(RiscvEmulatorState_t *state);
void                  RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t ram_length);

/* ---- The VM: execute one instruction. ---- */
void RiscvEmulatorLoop(RiscvEmulatorState_t *state);

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
void RiscvEmulatorFetch(uint32_t address, void *destination, uint8_t length); /* instruction read */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);  /* data read  */
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length); /* data write */
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
/* Supply backing storage for a CSR the engine does not implement (the engine
 * reads/writes through the returned pointer); return NULL to raise illegal. */
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *state, uint16_t csrnum);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

#endif /* PURVS_H_ */
