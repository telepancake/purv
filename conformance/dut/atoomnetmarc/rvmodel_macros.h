# rvmodel_macros.h — DUT-specific macros for atoomnetmarc/RISC-V-emulator
# SPDX-License-Identifier: BSD-3-Clause
#
# Differences from a typical (Spike/HTIF) DUT:
#  * Termination is via mini-rv32ima's SYSCON at 0x11100000 (0x5555 = poweroff),
#    not HTIF tohost. The runner maps SYSCON 0x5555 -> exit 0 (PASS) and any
#    other value -> exit 1 (FAIL).
#  * Console is mini-rv32ima's 16550-ish UART at 0x10000000 (LSR at +5 reads
#    transmit-ready), which the runner forwards to stdout.

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

# Kept for compatibility with test infra that references tohost/fromhost; the
# DUT does not use HTIF — termination is via SYSCON below.
#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;                \
        .align 8; .global tohost; tohost: .dword 0;         \
        .align 8; .global fromhost; fromhost: .dword 0;     \
        .popsection

##### STARTUP #####
# mini-rv32ima starts in M-mode with CSRs present; the default boot works.

##### TERMINATION #####
.EQU SYSCON_ADDR, 0x11100000

# Pass: write 0x5555 to SYSCON (mini-rv32ima poweroff) -> runner exits 0.
#define RVMODEL_HALT_PASS  \
  li t1, 0x5555           ;\
  li t0, SYSCON_ADDR      ;\
  halt_pass_loop:         ;\
    sw t1, 0(t0)          ;\
    j halt_pass_loop      ;\

# Fail: write a non-0x5555 value to SYSCON -> runner exits 1.
#define RVMODEL_HALT_FAIL \
  li t1, 0x3333           ;\
  li t0, SYSCON_ADDR      ;\
  halt_fail_loop:         ;\
    sw t1, 0(t0)          ;\
    j halt_fail_loop      ;\

##### IO (16550 UART at 0x10000000) #####
.EQU UART_BASE_ADDR, 0x10000000
.EQU UART_THR, (UART_BASE_ADDR + 0)
.EQU UART_LSR, (UART_BASE_ADDR + 5)

# No init needed — the runner's UART is always ready.
#define RVMODEL_IO_INIT(_R1, _R2, _R3)

#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR)               \
1:                           ;                       \
  lbu _R1, 0(_STR_PTR)        ;/* Load byte */        \
  beqz _R1, 3f                ;/* Exit if null */     \
2: /* uart_putc */           ;                      \
  li _R2, UART_LSR            ;\
  4: /* wait THR empty */     ;\
    lbu _R3, 0(_R2)           ;\
    andi _R3, _R3, 0x20       ;\
    beqz _R3, 4b              ;\
  li _R2, UART_THR            ;\
  sb _R1, 0(_R2)              ;\
  addi _STR_PTR, _STR_PTR, 1  ;\
  j 1b                        ;\
3:

##### Access Fault #####
#define RVMODEL_ACCESS_FAULT_ADDRESS 0x00000000

##### Machine Timer (mini-rv32ima CLINT) #####
#define RVMODEL_MTIME_ADDRESS    0x1100BFF8
#define RVMODEL_MTIMECMP_ADDRESS 0x11004000
#define RVMODEL_INTERRUPT_LATENCY 10
#define RVMODEL_TIMER_INT_SOON_DELAY 100
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1

##### Interrupts (unused for the rv32im unprivileged scope; defined to satisfy
##### tests/env/check_defines.h) #####
#define RVMODEL_SET_MEXT_INT(_R1, _R2)
#define RVMODEL_CLR_MEXT_INT(_R1, _R2)
#define RVMODEL_SET_MSW_INT(_R1, _R2)
#define RVMODEL_CLR_MSW_INT(_R1, _R2)
#define RVMODEL_SET_SEXT_INT(_R1, _R2)
#define RVMODEL_CLR_SEXT_INT(_R1, _R2)
#define RVMODEL_SET_SSW_INT(_R1, _R2)
#define RVMODEL_CLR_SSW_INT(_R1, _R2)

#endif // _RVMODEL_MACROS_H
