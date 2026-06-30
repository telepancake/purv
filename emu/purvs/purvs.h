/*
 * purvs.h - RISC-V (RV32IMC + Zifencei) monadic emulator: public interface.
 *
 * purvs is purv restructured into a monadic execution engine: it decodes a
 * straight-line run of instructions into a wide, already-classified internal
 * form, then runs a PLUGGABLE evaluator over that form. The evaluator is the one
 * place the value semantics live, so the engine itself is not tied to any
 * particular interpretation -- ordinary emulation is just the default eval, and
 * alternatives (tag/provenance tracking, taint, tracing, symbolic execution) are
 * drop-in replacements that reuse the same decode pass and run loop.
 *
 * Like purv, this is a user-level program runner, not a machine: no CSRs, no
 * privileged modes, no trap-to-mtvec. The state struct is public (below); set it
 * up by writing its fields directly. The engine provides functions only for
 * things that are more than a field access: running the VM, the default eval, and
 * a default no-op memory callback.
 */
#ifndef PURVS_H_
#define PURVS_H_

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

/* The four regions, indexed `half*2 + direction`. The low bit is the grow
 * direction within a half (0 = up from the half's base, 1 = down from its top);
 * the high bit is the half (0 = lower / read-only, 1 = upper / read-write):
 *   region[RISCV_CODE]   [0, len)                     fetch + read-only data
 *   region[RISCV_RODATA] [RISCV_HALF - len, RISCV_HALF)   read-only data
 *   region[RISCV_HEAP]   [RISCV_HALF, RISCV_HALF + len)   read/write data
 *   region[RISCV_STACK]  [2^32 - len, 2^32)               read/write, grows down */
enum { RISCV_CODE, RISCV_RODATA, RISCV_HEAP, RISCV_STACK };

/* ---- decoded instruction: the engine's wide internal form ----
 *
 * The run loop is split in two. A decode pass walks the code window and lowers
 * each raw instruction -- 32-bit or compressed -- into one of these flat records;
 * an eval pass then threads through the records, dispatching on `op`.
 *
 * `op` is OUR opcode, not RISC-V's: a flat LEAF code, one per evaluator handler
 * (ADD, SUB, LB, BEQ, ...), so the evaluator dispatches with a single jump-table
 * index -- no secondary decode of funct3/funct7. There is deliberately no 1:1
 * correspondence with the RISC-V encoding; decode folds the 32-bit and compressed
 * forms, funct3, and funct7 into this one byte. Operand sources still vary, so a
 * couple of fields remain: for an ALU op the second operand is imm when `b_imm`,
 * else x[rs2]; loads/stores use x[rs1]+imm; branches/jal use imm.
 *
 * Decode records only what is fixed at decode time -- the leaf op, register
 * INDICES, and immediates -- and deliberately does NOT read register values
 * (those change as earlier instructions in the same run execute). Eval reads the
 * live operands from state->x when the instruction runs; that split is what lets
 * the value semantics be swapped out (see RiscvEmulatorEvalFn). */
enum {
    /* ALU: a = x[rs1]; b = b_imm ? imm : x[rs2]; rd = a <op> b */
    RISCV_OP_ADD, RISCV_OP_SLL, RISCV_OP_SLT, RISCV_OP_SLTU,   /* funct3 order, so */
    RISCV_OP_XOR, RISCV_OP_SRL, RISCV_OP_OR,  RISCV_OP_AND,    /* op = ADD + funct3 */
    RISCV_OP_SUB, RISCV_OP_SRA,                                /* funct7-bit-5 variants */
    /* RV32M: a = x[rs1]; b = x[rs2]  (funct3 order, op = MUL + funct3) */
    RISCV_OP_MUL, RISCV_OP_MULH, RISCV_OP_MULHSU, RISCV_OP_MULHU,
    RISCV_OP_DIV, RISCV_OP_DIVU, RISCV_OP_REM, RISCV_OP_REMU,
    /* loads: rd = mem[x[rs1] + imm] */
    RISCV_OP_LB, RISCV_OP_LH, RISCV_OP_LW, RISCV_OP_LBU, RISCV_OP_LHU,
    /* stores: mem[x[rs1] + imm] = x[rs2] */
    RISCV_OP_SB, RISCV_OP_SH, RISCV_OP_SW,
    /* branches: if cmp(x[rs1], x[rs2]) pc += imm */
    RISCV_OP_BEQ, RISCV_OP_BNE, RISCV_OP_BLT, RISCV_OP_BGE, RISCV_OP_BLTU, RISCV_OP_BGEU,
    /* control / misc */
    RISCV_OP_JAL,    /* rd = pc + width; pc += imm */
    RISCV_OP_JALR,   /* rd = pc + width; pc = (x[rs1] + imm) & ~1 */
    RISCV_OP_LUI,    /* rd = imm */
    RISCV_OP_AUIPC,  /* rd = pc + imm */
    RISCV_OP_NOP,    /* fence / fence.i */
    RISCV_OP_ECALL, RISCV_OP_EBREAK, RISCV_OP_ILLEGAL,  /* -> the matching handler */
    RISCV_OP_COUNT   /* number of leaf ops; sizes the jump table */
};

typedef struct {
    uint32_t pc;            /* guest address of this instruction */
    uint32_t raw;           /* raw encoding (the low 16 bits for a compressed insn) */
    uint32_t imm;           /* immediate / shift amount, pre-extended for the op */
    uint8_t  op;            /* one of RISCV_OP_*: our flat leaf opcode */
    uint8_t  rd, rs1, rs2;  /* register indices (0..31) */
    uint8_t  width;         /* encoded length in bytes: 2 or 4 */
    uint8_t  b_imm;         /* ALU: second operand is imm (1) or x[rs2] (0) */
    uint8_t  ends_block;    /* this insn ends the decoded run (control transfer / trap) */
} RiscvEmulatorDecoded_t;

/* The pluggable evaluator: the engine's value semantics, made swappable.
 *
 * The engine decodes a straight-line run of instructions into an internal buffer,
 * then calls eval ONCE for the whole run. eval threads through the `block` of
 * `count` records, dispatching each on its leaf op, and interprets them however
 * it likes -- the default (RiscvEmulatorDefaultEval) does ordinary RV32 value
 * computation with a computed-goto jump table. eval is the ONE place value
 * semantics live, so an alternate (tag tracking, taint, tracing, symbolic
 * execution, ...) is a drop-in replacement that reuses decode and the run loop.
 *
 * Contract: eval executes the records in order and MUST leave state->pc at the
 * next instruction to execute (pc + width for a sequential op, the computed
 * target for a taken branch/jump). It returns the number of records it actually
 * executed (normally `count`), and sets *halt to nonzero to stop the whole run
 * (a terminal ecall, a breakpoint, an illegal instruction). A control transfer is
 * always the LAST record in a run (decode ends the run there), so eval never has
 * to abandon the buffer mid-stream except on *halt. */
typedef uint32_t (*RiscvEmulatorEvalFn)(RiscvEmulatorState_t *state,
                                        const RiscvEmulatorDecoded_t *block,
                                        uint32_t count, int *halt);

/* The whole VM state. Set it up by writing the fields:
 *   - pc:       the next instruction to execute (the run loop resumes here);
 *   - x[1..31]: registers (x[0] reads as zero; the engine never writes it);
 *   - region[]: the four memory regions above (sp starts at 0, grows down into
 *               region[RISCV_STACK]; instruction fetch reads region[RISCV_CODE]);
 *   - ecall/ebreak/illegal: the trap handlers the engine calls;
 *   - callback: handles a data access that misses the regions or hits read-only
 *               memory (see RiscvEmulatorMemFn); Init installs a no-op default;
 *   - eval: the pluggable per-instruction evaluator (see RiscvEmulatorEvalFn);
 *           Init installs RiscvEmulatorDefaultEval (ordinary RV32 semantics).
 * `inst` is engine-internal scratch: the run loop keeps the live next-pc in a
 * register and writes inst (and pc) to the state only when calling a handler, so
 * they are valid inside ecall/ebreak/illegal. */
struct RiscvEmulatorState {
    uint32_t pc;
    uint32_t inst;                               /* internal: raw word of the faulting insn */
    uint32_t x[32];
    RiscvEmulatorRegion_t region[4];
    RiscvEmulatorTrapFn   ecall, ebreak, illegal;
    RiscvEmulatorMemFn    callback;
    RiscvEmulatorEvalFn   eval;                  /* the value semantics; Init -> default */
};

/* Optional convenience: zero the state, map the four regions, install the default
 * (no-op) memory callback and the default eval, set sp (x[2]) to 0 -- the stack
 * grows down from 2^32 -- and pc to 0 (code is based at 0). Allocate the struct
 * yourself (stack or heap); there is no create/destroy. */
void RiscvEmulatorInit(RiscvEmulatorState_t *state,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t rodata,
                       RiscvEmulatorRegion_t heap, RiscvEmulatorRegion_t stack);

/* ---- The VM ----
 * Execute up to `max` instructions and return the number actually executed.
 * Execution resumes at state->pc and leaves state->pc at the next instruction.
 *
 * Internally each batch runs in two passes: a decode pass lowers a straight-line
 * run of instructions -- from pc up to the end of the code window or the first
 * control transfer -- into an internal buffer of RiscvEmulatorDecoded_t, then the
 * pluggable evaluator (state->eval) runs over that buffer. Decode never reads
 * register values, so the two passes are cleanly separable.
 *
 * Instructions are fetched ONLY from state->code, the window based at 0: a pc
 * outside [0, code.len) ends the batch and is left in state->pc (a short return --
 * fewer than `max` -- with pc outside the window is how you detect it). Data
 * loads/stores go through the regions / the callback; only fetch uses code as
 * instructions.
 *
 * Loop also stops when `max` is reached, or eval returns nonzero (the default
 * eval returns nonzero when an ecall/ebreak/illegal handler does). A halt the
 * host decides by other means (e.g. polling a tohost word it owns) is invisible
 * to the engine; run Loop in bounded slices and check between them. */
uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *state, uint64_t max);

/* The default evaluator installed by Init: ordinary RV32IMC value semantics (the
 * same computation purv has always done), implemented as a threaded computed-goto
 * dispatch over the decoded run. Exposed so callers can reinstall it. See
 * RiscvEmulatorEvalFn for the contract. */
uint32_t RiscvEmulatorDefaultEval(RiscvEmulatorState_t *state,
                                  const RiscvEmulatorDecoded_t *block,
                                  uint32_t count, int *halt);

/* (To read or write guest memory from outside the engine, pick the region from
 * the address: half = addr >> 31, then within the half lo = addr & (RISCV_HALF-1)
 * is region[half*2]'s offset if lo < its len, else region[half*2+1] grows down
 * from RISCV_HALF.) */

#endif /* PURVS_H_ */
