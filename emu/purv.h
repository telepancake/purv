/*
 * purv.h - RISC-V (RV32IMC + Zifencei) userspace emulator: public interface.
 *
 * A small, hand-written engine. It runs user-level programs -- no CSRs, no
 * privileged modes, no trap-to-mtvec -- and is self-contained: it has no host
 * hooks, only the handlers and memory you put in the state. The state struct is
 * public (below), so you set it up by writing its fields directly; the engine
 * provides functions only for things that are more than a field access: running
 * the VM, and reading/writing guest memory through the mapped regions.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>

/* Base of the address space. It is RISCV_REGIONS regions, each up to
 * RISCV_REGION_SIZE bytes, based that far apart: region i covers
 * [RAM_ORIGIN + i*RISCV_REGION_SIZE, ...). */
#define RAM_ORIGIN         0x80000000u
#define RISCV_REGIONS      8u
#define RISCV_REGION_SIZE  0x10000000u           /* 256 MiB */

typedef struct RiscvEmulatorState RiscvEmulatorState_t;

/* A trap handler: the engine calls it for an ecall, ebreak, or illegal
 * instruction. Return nonzero to stop the run loop and hand control back to the
 * caller of RiscvEmulatorLoop, or zero to keep executing (a fully-serviced
 * syscall returns zero and continues in place; a terminal call like exit returns
 * nonzero). NULL means the default: ecall is a nop that keeps running; ebreak
 * and illegal stop. */
typedef int (*RiscvEmulatorTrapFn)(RiscvEmulatorState_t *state);

/* One memory region. `ptr` is host storage of `len` bytes (<= RISCV_REGION_SIZE)
 * mapped at this region's base; `writable` says whether stores may modify it.
 * ptr == NULL means the region is unmapped. */
typedef struct {
    uint8_t *ptr;
    uint32_t len;
    uint8_t  writable;
} RiscvEmulatorRegion_t;

/* The whole VM state. Set it up by writing the fields:
 *   - pc:       the next instruction to execute (the run loop resumes here);
 *   - x[1..31]: registers (x[0] reads as zero; the engine never writes it);
 *   - code:     the instruction-fetch window, based at RAM_ORIGIN -- the engine
 *               reads instructions only from here, never from mem[];
 *   - mem[i]:   data region i (a load from an unmapped/out-of-bounds address
 *               reads zero; a store to a read-only/out-of-bounds address is
 *               dropped). mem[RISCV_REGIONS-1] is conventionally the stack;
 *   - ecall/ebreak/illegal: the handlers the engine calls.
 * The remaining fields (npc, inst, trap) are engine-internal scratch. */
struct RiscvEmulatorState {
    uint32_t pc;
    uint32_t npc;                                /* internal: next pc within a step */
    uint32_t inst;                               /* internal: raw word of the current insn */
    uint32_t x[32];
    uint8_t  trap;                               /* internal: pending illegal flag */
    RiscvEmulatorRegion_t code;                  /* instruction fetch, based at RAM_ORIGIN */
    RiscvEmulatorRegion_t mem[RISCV_REGIONS];
    RiscvEmulatorTrapFn   ecall, ebreak, illegal;
};

/* Optional convenience: zero the state, then map `code` as the instruction-fetch
 * window (based at RAM_ORIGIN) and `stack` as the last data region
 * (mem[RISCV_REGIONS-1], based at the top of the address space). sp (x[2]) is set
 * to the top of the stack region, and pc to RAM_ORIGIN. Allocate the struct
 * yourself (stack or heap); there is no create/destroy. */
void RiscvEmulatorInit(RiscvEmulatorState_t *state,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t stack);

/* ---- The VM ----
 * Execute up to `max` instructions and return the number actually executed.
 * Execution resumes at state->pc and leaves state->pc at the next instruction.
 *
 * Instructions are fetched ONLY from state->code, the window based at RAM_ORIGIN:
 * a pc outside [RAM_ORIGIN, RAM_ORIGIN + code.len) ends the batch and is left in
 * state->pc (a short return -- fewer than `max` -- with pc outside the window is
 * how you detect it). Data loads/stores go through state->mem; only fetch uses
 * the code window.
 *
 * Loop also stops when `max` is reached, or an ecall/ebreak/illegal handler
 * returns nonzero. A halt the host decides by other means (e.g. polling a tohost
 * word it owns) is invisible to the engine; run Loop in bounded slices and check
 * between them. */
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *state, uint64_t max);

/* (To read or write guest memory from outside the engine, index state->mem[]
 * directly: region i covers [RAM_ORIGIN + i*RISCV_REGION_SIZE, + region.len).) */

#endif /* PURV_H_ */
