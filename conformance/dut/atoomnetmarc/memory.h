/* ACT4 host backing memory for atoomnetmarc/RISC-V-emulator. */
#ifndef MEMORY_H_
#define MEMORY_H_
#include <stdint.h>

/* One flat region covering the ACT4 link base (0x80000000 = RAM_ORIGIN). */
#define RAM_LENGTH 0x1000000   /* 16 MiB */

extern uint8_t memory[RAM_LENGTH];
extern uint8_t pleasestop;
extern int g_exitcode;
#endif
