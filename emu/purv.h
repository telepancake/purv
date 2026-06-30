/*
 * purv.h - RISC-V (RV32IMC + Zifencei) userspace emulator: public interface.
 *
 * A small, hand-written engine. It runs user-level programs -- no CSRs, no
 * privileged modes, no trap-to-mtvec -- and is self-contained: it has no host
 * hooks, only the handlers and memory you put in the state. The state struct is
 * public (below), so you set it up by writing its fields directly; the engine
 * provides functions only for things that are more than a field access: running
 * the VM, and a default no-op memory callback.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>

/* The address space is split in half at the bit-31 boundary. Each half holds two
 * regions that grow toward each other from its ends:
 *
 *   lower half [0, RISCV_HALF)        -- readable, NOT writable:
 *       code   [0, code.len)              (also the instruction-fetch window)
 *       rodata [RISCV_HALF - rodata.len, RISCV_HALF)
 *   upper half [RISCV_HALF, 2^32)     -- readable AND writable:
 *       heap   [RISCV_HALF, RISCV_HALF + heap.len)
 *       stack  [2^32 - stack.len, 2^32)   (grows down from 0; sp starts at 0)
 *
 * A data load outside all four regions, or a store to the lower (read-only) half
 * or outside, calls the memory callback (which a default no-op installs). */
#define RISCV_HALF  0x80000000u                  /* lower/upper boundary; heap base */

typedef struct RiscvEmulatorState RiscvEmulatorState_t;

/* A trap handler: the engine calls it for an ecall, ebreak, or illegal
 * instruction. Return nonzero to stop the run loop and hand control back to the
 * caller of RiscvEmulatorLoop, or zero to keep executing (a fully-serviced
 * syscall returns zero and continues in place; a terminal call like exit returns
 * nonzero). NULL means the default: ecall is a nop that keeps running; ebreak and
 * illegal stop. */
typedef int (*RiscvEmulatorTrapFn)(RiscvEmulatorState_t *state);

/* Operation passed to the memory callback. */
enum { RISCV_MEM_LOAD = 0, RISCV_MEM_STORE = 1 };

/* The out-of-bounds / read-only memory callback. The engine calls it for any data
 * access it cannot satisfy from the regions: a load outside all four regions, or a
 * store to the read-only lower half or outside. `op` is RISCV_MEM_LOAD or
 * RISCV_MEM_STORE, `addr` the guest address, `value` the word being stored (0 for
 * a load). The return value is the word delivered to a load (its low bytes for a
 * narrow load); it is ignored for a store. RiscvEmulatorInit installs a default
 * that does nothing and returns zero. */
typedef uint32_t (*RiscvEmulatorMemFn)(RiscvEmulatorState_t *state, int op,
                                       uint32_t addr, uint32_t value);

/* One memory region: host storage of `len` bytes mapped at the region's (fixed)
 * base. There is no writable flag -- writability follows the address half. ptr ==
 * NULL (len 0) means the region is unmapped. */
typedef struct {
    uint8_t *ptr;
    uint32_t len;
} RiscvEmulatorRegion_t;

/* The whole VM state. Set it up by writing the fields:
 *   - pc:       the next instruction to execute (the run loop resumes here);
 *   - x[1..31]: registers (x[0] reads as zero; the engine never writes it);
 *   - code:     [0, code.len) -- the instruction-fetch window, and readable data;
 *   - rodata:   [RISCV_HALF - rodata.len, RISCV_HALF) -- read-only data;
 *   - heap:     [RISCV_HALF, RISCV_HALF + heap.len) -- read/write data;
 *   - stack:    [2^32 - stack.len, 2^32) -- read/write, grows down (sp starts 0);
 *   - ecall/ebreak/illegal: the trap handlers the engine calls;
 *   - callback: handles a data access that misses the regions or hits read-only
 *               memory (see RiscvEmulatorMemFn); Init installs a no-op default.
 * The remaining fields (npc, inst, trap) are engine-internal scratch. */
struct RiscvEmulatorState {
    uint32_t pc;
    uint32_t npc;                                /* internal: next pc within a step */
    uint32_t inst;                               /* internal: raw word of the current insn */
    uint32_t x[32];
    uint8_t  trap;                               /* internal: pending illegal flag */
    RiscvEmulatorRegion_t code;                  /* [0, len): fetch + read-only data */
    RiscvEmulatorRegion_t rodata;                /* (RISCV_HALF - len .. RISCV_HALF): read-only */
    RiscvEmulatorRegion_t heap;                  /* [RISCV_HALF, +len): read/write */
    RiscvEmulatorRegion_t stack;                 /* (2^32 - len .. 2^32): read/write, grows down */
    RiscvEmulatorTrapFn   ecall, ebreak, illegal;
    RiscvEmulatorMemFn    callback;
};

/* Optional convenience: zero the state, map the four regions, install the default
 * (no-op) memory callback, set sp (x[2]) to 0 -- the stack grows down from 2^32 --
 * and pc to 0 (code is based at 0). Allocate the struct yourself (stack or heap);
 * there is no create/destroy. */
void RiscvEmulatorInit(RiscvEmulatorState_t *state,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t rodata,
                       RiscvEmulatorRegion_t heap, RiscvEmulatorRegion_t stack);

/* ---- The VM ----
 * Execute up to `max` instructions and return the number actually executed.
 * Execution resumes at state->pc and leaves state->pc at the next instruction.
 *
 * Instructions are fetched ONLY from state->code, the window based at 0: a pc
 * outside [0, code.len) ends the batch and is left in state->pc (a short return --
 * fewer than `max` -- with pc outside the window is how you detect it). Data
 * loads/stores go through the regions / the callback; only fetch uses code as
 * instructions.
 *
 * Loop also stops when `max` is reached, or an ecall/ebreak/illegal handler
 * returns nonzero. A halt the host decides by other means (e.g. polling a tohost
 * word it owns) is invisible to the engine; run Loop in bounded slices and check
 * between them. */
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *state, uint64_t max);

/* (To read or write guest memory from outside the engine, index the region whose
 * range covers the address: code [0,+len), rodata [RISCV_HALF-len, RISCV_HALF),
 * heap [RISCV_HALF,+len), stack [(uint32_t)(0-len), 2^32).) */

#endif /* PURV_H_ */
