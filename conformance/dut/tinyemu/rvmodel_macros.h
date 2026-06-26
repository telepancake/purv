# rvmodel_macros.h — DUT-specific macros for TinyEMU (fernandotcl/TinyEMU)
# SPDX-License-Identifier: BSD-3-Clause
#
# TinyEMU exposes an HTIF device at the fixed address 0x40008000 (NOT the ELF
# `tohost` symbol). Protocol (see riscv_machine.c htif_handle_cmd):
#   * write tohost == 1            -> exit(0)                (HALT)
#   * write tohost with device=1,cmd=1 -> console putchar(low byte)
# A 64-bit tohost is written as two 32-bit stores: low at +0, high at +4 (the
# +4 store triggers the command). Pass and fail both exit(0); run_tests.py
# distinguishes them via the RVCP-SUMMARY console string.

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

.EQU HTIF_TOHOST, 0x40008000

# Unused for HTIF, but keep the section for infra that references the symbols.
#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;                \
        .align 8; .global tohost; tohost: .dword 0;         \
        .align 8; .global fromhost; fromhost: .dword 0;     \
        .popsection

##### STARTUP #####
# TinyEMU boots in M-mode; default boot works.

##### TERMINATION (write tohost=1 -> exit) #####
#define RVMODEL_HALT_PASS  \
  li t0, HTIF_TOHOST       ;\
  li t1, 1                 ;\
  halt_pass_loop:          ;\
    sw t1, 0(t0)           ;\
    sw x0, 4(t0)           ;\
    j halt_pass_loop       ;\

# Fail also exits via tohost=1; the RVCP-SUMMARY: TEST FAILED line (already
# printed below) is what marks the failure.
#define RVMODEL_HALT_FAIL  \
  li t0, HTIF_TOHOST       ;\
  li t1, 1                 ;\
  halt_fail_loop:          ;\
    sw t1, 0(t0)           ;\
    sw x0, 4(t0)           ;\
    j halt_fail_loop       ;\

##### IO: HTIF console (device=1, cmd=1) #####
# high word 0x01010000 = (device 1 << 24)|(cmd 1 << 16); low word = char.
#define RVMODEL_IO_INIT(_R1, _R2, _R3)
#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR)               \
1:                              ;\
  lbu _R1, 0(_STR_PTR)          ;\
  beqz _R1, 3f                  ;\
  li _R2, HTIF_TOHOST           ;\
  sw _R1, 0(_R2)                ;\
  li _R3, 0x01010000            ;\
  sw _R3, 4(_R2)                ;\
  addi _STR_PTR, _STR_PTR, 1    ;\
  j 1b                          ;\
3:

##### Access Fault #####
#define RVMODEL_ACCESS_FAULT_ADDRESS 0x00000000

##### Machine Timer (TinyEMU CLINT) #####
#define RVMODEL_MTIME_ADDRESS    0x0200BFF8
#define RVMODEL_MTIMECMP_ADDRESS 0x02004000
#define RVMODEL_INTERRUPT_LATENCY 10
#define RVMODEL_TIMER_INT_SOON_DELAY 100
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1

##### Interrupts (unused for the rv32imc unprivileged scope) #####
#define RVMODEL_SET_MEXT_INT(_R1, _R2)
#define RVMODEL_CLR_MEXT_INT(_R1, _R2)
#define RVMODEL_SET_MSW_INT(_R1, _R2)
#define RVMODEL_CLR_MSW_INT(_R1, _R2)
#define RVMODEL_SET_SEXT_INT(_R1, _R2)
#define RVMODEL_CLR_SEXT_INT(_R1, _R2)
#define RVMODEL_SET_SSW_INT(_R1, _R2)
#define RVMODEL_CLR_SSW_INT(_R1, _R2)

#endif // _RVMODEL_MACROS_H
