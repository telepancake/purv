/*
 * ACT4 implementation-specific glue for atoomnetmarc/RISC-V-emulator.
 *
 * Provides the memory access + environment hooks the emulator library expects.
 * Memory map (matches conformance/dut/atoomnetmarc/rvmodel_macros.h):
 *   RAM    0x80000000 (RAM_ORIGIN), 16 MiB  -> backing array memory[]
 *   UART   0x10000000 store -> stdout; 0x10000005 load -> 0x60 (tx ready)
 *   SYSCON 0x11100000 store 0x5555 -> exit 0 (PASS), else exit 1 (FAIL)
 */
#include <string.h>
#include <stdio.h>

#include <RiscvEmulatorDefine.h>   /* RAM_ORIGIN, ROM_ORIGIN */
#include <RiscvEmulatorType.h>
#include "memory.h"

#ifndef RiscvEmulatorImplementationSpecific_H_
#define RiscvEmulatorImplementationSpecific_H_

#define ACT_UART_THR 0x10000000u
#define ACT_UART_LSR 0x10000005u
#define ACT_SYSCON   0x11100000u

static inline void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length) {
    if (address == ACT_UART_LSR) { *(uint8_t *)destination = 0x60; return; } /* THR empty + TEMT */
    if (address >= RAM_ORIGIN && (uint64_t)address + length <= (uint64_t)RAM_ORIGIN + RAM_LENGTH) {
        memcpy(destination, &memory[address - RAM_ORIGIN], length);
        return;
    }
    memset(destination, 0, length);  /* unmapped reads as zero */
}

static inline void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length) {
    if (address == ACT_UART_THR) { putchar(*(const uint8_t *)source); fflush(stdout); return; }
    if (address == ACT_SYSCON) {
        uint32_t v = 0;
        memcpy(&v, source, length < 4 ? length : 4);
        g_exitcode = (v == 0x5555u) ? 0 : 1;
        pleasestop = 1;
        return;
    }
    if (address >= RAM_ORIGIN && (uint64_t)address + length <= (uint64_t)RAM_ORIGIN + RAM_LENGTH) {
        memcpy(&memory[address - RAM_ORIGIN], source, length);
        return;
    }
    pleasestop = 1; g_exitcode = 1;  /* stray store -> fail */
}

/* Unrecognized instruction: vector to the test's trap handler if one is set,
 * otherwise stop (the test cannot self-check). */
static inline void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state) {
#if (RVE_E_ZICSR == 1)
    if (state->csr.mtvec.base != 0) return;  /* RiscvEmulatorTrap will vector */
#else
    (void)state;
#endif
    pleasestop = 1; g_exitcode = 1;
}

#if (RVE_E_ZICSR == 1)
/* Unimplemented CSR: raise illegal-instruction so it vectors like real HW. */
static inline void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state) {
    state->trapflag.illegalinstruction = 1;
}
#endif

/* ECALL/EBREAK already set the trap flags in the library; let them vector to
 * the test's handler. Nothing for the host to do. */
static inline void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state) { (void)state; }
static inline void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state) { (void)state; }

#endif
