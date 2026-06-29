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
 *   - mem[i]:   map region i (a data load from an unmapped/out-of-bounds address
 *               reads zero; a store to a read-only/out-of-bounds address is
 *               dropped);
 *   - ecall/ebreak/illegal: the handlers the engine calls.
 * The remaining fields (npc, inst, trap) are engine-internal scratch. */
struct RiscvEmulatorState {
    uint32_t pc;
    uint32_t npc;                                /* internal: next pc within a step */
    uint32_t inst;                               /* internal: raw word of the current insn */
    uint32_t x[32];
    uint8_t  trap;                               /* internal: pending illegal flag */
    RiscvEmulatorRegion_t mem[RISCV_REGIONS];
    RiscvEmulatorTrapFn   ecall, ebreak, illegal;
};

/* ---- Lifecycle (convenience; you may also just zero a state and set fields) ---- */
/* Init zeroes the state, sets sp (x[2]) to initial_sp, and parks pc at a reset
 * vector. Create heap-allocates an initialized state; Destroy frees it. */
RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp);
void                  RiscvEmulatorDestroy(RiscvEmulatorState_t *state);
void                  RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t initial_sp);

/* ---- The VM ----
 * Execute up to `max` instructions and return the number actually executed.
 * Execution resumes at state->pc and leaves state->pc at the next instruction.
 *
 * Instructions are fetched ONLY from the code window: `code` points at the host
 * bytes for guest address `code_base` and spans `code_len` bytes. A pc outside
 * [code_base, code_base+code_len) ends the batch and is left in state->pc (a
 * short return -- fewer than `max` -- with pc outside the window is how you
 * detect it). Data loads/stores go through state->mem; only fetch uses the
 * window.
 *
 * Loop also stops when `max` is reached, or an ecall/ebreak/illegal handler
 * returns nonzero. A halt the host decides by other means (e.g. polling a tohost
 * word it owns) is invisible to the engine; run Loop in bounded slices and check
 * between them. */
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *state,
                           const uint8_t *code, uint32_t code_len, uint32_t code_base,
                           uint64_t max);

/* ---- Guest memory through the mapped regions (OOB reads zero; OOB / read-only
 *      writes are dropped). For hosts/debuggers that don't own the storage. ---- */
void RiscvEmulatorReadMemory(const RiscvEmulatorState_t *state, uint32_t addr,
                             void *dst, uint32_t len);
void RiscvEmulatorWriteMemory(RiscvEmulatorState_t *state, uint32_t addr,
                              const void *src, uint32_t len);

#endif /* PURV_H_ */
