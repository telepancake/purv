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

/* Data memory is two self-describing regions (each carries its own base), clustered
 * around the two fixed anchors 0 and RISCV_HALF:
 *
 *   readonly  the read-only span around 0 -- code (also the instruction-fetch
 *             window, at [0, code.len)) and rodata. purva keeps rodata just below 0
 *             (at 0 - rodata.len) since its code is not data-addressable.
 *   writable  the read/write span around RISCV_HALF -- the stack just below it
 *             (grows down from RISCV_HALF) and the heap just above, as ONE region.
 *             sp starts at RISCV_HALF.
 *
 * A load that misses both regions, or a store outside the writable region, calls the
 * memory callback (which a default no-op installs). A translation is one bounded
 * check per region -- no half-split, no growing-toward-each-other pairs. */
#define RISCV_HALF  0x80000000u                  /* stack/heap boundary; sp starts here */

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

/* One memory region: host storage of `len` bytes holding the guest bytes at
 * [base, base + len). ptr == NULL (len 0) means the region is unmapped. Each region
 * is self-describing (carries its own base), so a translation is one bounded check
 * -- (uint64_t)(addr - base) + n <= len -- with no per-access base arithmetic. */
typedef struct {
    uint8_t *ptr;
    uint32_t len;
    uint32_t base;
} RiscvEmulatorRegion_t;

/* The whole VM state. Set it up by writing the fields:
 *   - pc:       the next instruction to execute (the run loop resumes here);
 *   - x[1..31]: registers (x[0] reads as zero; the engine never writes it);
 *   - readonly: the read-only span around 0 -- code (also the instruction-fetch
 *               window) and any rodata; based at 0 for engines whose code is real
 *               bytes (purv/purvs), or at 0 - rodata.len for purva, whose code is
 *               fetched separately and is not data-addressable;
 *   - writable: the read/write span around RISCV_HALF -- the stack (grows down from
 *               RISCV_HALF) and the heap (up from it) as ONE region; sp starts at
 *               RISCV_HALF;
 *   - ecall/ebreak/illegal: the trap handlers the engine calls;
 *   - callback: handles a data access that misses both regions, or a store into the
 *               read-only region (see RiscvEmulatorMemFn); Init installs a no-op default.
 * `inst` is engine-internal scratch: the run loop keeps the live next-pc in a
 * register and writes inst (and pc) to the state only when calling a handler, so
 * they are valid inside ecall/ebreak/illegal. */
struct RiscvEmulatorState {
    uint32_t pc;
    uint32_t inst;                               /* internal: raw word of the faulting insn */
    uint32_t x[32];
    RiscvEmulatorRegion_t readonly;              /* code (+ rodata): read-only, fetch window */
    RiscvEmulatorRegion_t writable;              /* stack + heap: one read/write span */
    RiscvEmulatorTrapFn   ecall, ebreak, illegal;
    RiscvEmulatorMemFn    callback;
};

/* Optional convenience: zero the state, map the two regions, install the default
 * (no-op) memory callback, set sp (x[2]) to RISCV_HALF -- the stack grows down from
 * there -- and pc to 0. Allocate the struct yourself; there is no create/destroy. */
void RiscvEmulatorInit(RiscvEmulatorState_t *state,
                       RiscvEmulatorRegion_t readonly, RiscvEmulatorRegion_t writable);

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

/* (To read or write guest memory from outside the engine, test the two regions:
 * rel = addr - region.base; if ((uint64_t)rel + n <= region.len) it is region.ptr +
 * rel. Try `writable` first, then `readonly`.) */

#endif /* PURV_H_ */
