/*
 * purv.c - RISC-V (RV32IMC + Zifencei) userspace emulator: implementation.
 *
 * A user-level program runner, not a machine: no CSRs, no privileged modes, no
 * trap-to-mtvec. It is self-contained -- no host hooks. The state (defined in
 * purv.h, public) holds everything: four memory regions (code/rodata in the
 * read-only lower half, heap/stack in the read/write upper half), the
 * ecall/ebreak/illegal handlers, and a memory callback. Data loads/stores resolve
 * straight into the regions; an access that misses them (or a store to read-only
 * memory) goes to the callback; the handlers return nonzero to stop the loop.
 *
 * RiscvEmulatorLoop runs a batch in one call -- the fetch/decode/execute step is
 * the body of an internal loop, so there is no per-instruction call boundary --
 * fetching instructions from state->code (based at 0).
 *
 * Structure: each opcode -- 32-bit or compressed -- decodes only the operands it
 * needs into a few locals, then `goto`s a shared action body. Registers are
 * indices into x[32]; x0 is never written, so it reads as a hard zero.
 */
#include <stdint.h>
#include <string.h>

#include "purv.h"

/* Base opcodes (instruction[6:0]). */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

/* ---- memory: one address translation, used by every load and store ---- */
/* mem_xlate maps [addr, addr+n) to host storage, or NULL on a miss (and on a
 * write to the read-only lower half). The half (addr >> 31) selects a pair of
 * adjacent regions that grow toward each other: region[half*2] up from the half's
 * base, region[half*2+1] down from RISCV_HALF. The load/store sites read/write the
 * bytes explicitly per width (little-endian, so the byte-or / byte-store idioms
 * compile to a single unaligned sized access). */
static inline __attribute__((always_inline))
uint8_t *mem_xlate(const RiscvEmulatorState_t *s, uint32_t addr, uint32_t n, int write) {
    if (write && addr < RISCV_HALF) return (uint8_t *)0;     /* lower half is read-only */
    const RiscvEmulatorRegion_t *r = &s->region[(addr >> 31) << 1];
    uint32_t lo = addr & (RISCV_HALF - 1);
    if (lo + n <= r[0].len) return r[0].ptr + lo;            /* grows up from the base */
    uint32_t down = RISCV_HALF - r[1].len;                   /* grows down from RISCV_HALF */
    if (lo >= down && lo + n <= RISCV_HALF) return r[1].ptr + (lo - down);
    return (uint8_t *)0;
}

static void wr(RiscvEmulatorState_t *s, uint32_t i, uint32_t v) { if (i) s->x[i] = v; }

/* Sign-extend the low `bits` of v. */
static int32_t sext(uint32_t v, int bits) {
    int sh = 32 - bits;
    return (int32_t)(v << sh) >> sh;
}

/* ----------------------------------------------------------------- the VM */

uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint64_t max) {
    uint32_t pc = s->pc;              /* resume where we left off */
    const uint8_t *code = s->region[RISCV_CODE].ptr;   /* instruction-fetch window... */
    uint32_t code_len = s->region[RISCV_CODE].len;     /* ...based at 0 */
    uint64_t k = 0;

    /* Outer loop: validate pc and size a window of instructions that can be
     * fetched without a bounds check -- inside the window pc only steps forward,
     * so a window sized from code_len keeps every fetch in range. The inner loop
     * runs the window straight-line; a branch/jump assigns pc directly and breaks
     * back here to re-validate the target. (The inner body keeps the old indent so
     * the diff stays the size of the actual change, not a whitespace reflow.) */
    while (k < max) {
        uint32_t base = pc;
        if (!code || base >= code_len || code_len - base < 2) break;
        uint32_t safe;
        if (code_len - base < 4) {                /* tail: only a compressed insn can fit */
            if (((code[base] | code[base + 1] << 8) & 3) == 3) break;  /* 32-bit straddles end */
            safe = 1;
        } else safe = (code_len - base) / 4;      /* worst case (all 32-bit), this many fit */

        for (uint32_t i = 0; i < safe && k < max; i++) {
        k++;
        uint32_t off = pc;                        /* current insn; fetch is in range by the window */

        /* inst/width stay in registers; they reach the state only when a handler
         * needs them (the trap paths below set s->pc/s->inst). */
        uint32_t rd = 0, rs1 = 0, a = 0, b = 0, f3 = 0, f7 = 0, imm = 0, r, addr, n, width, inst;

        uint16_t lo = (uint16_t)(code[off] | code[off + 1] << 8);
        if ((lo & 3) == 3) {                                  /* ---- 32-bit ---- */
            uint32_t w = lo | (uint32_t)(code[off + 2] | code[off + 3] << 8) << 16;
            inst = w;
            width = 4;
            rd = (w >> 7) & 31; rs1 = (w >> 15) & 31; f3 = (w >> 12) & 7;
            switch (w & 0x7f) {
            case OPIMM:                                       /* reg-immediate */
                a = s->x[rs1];
                if (f3 == 1 || f3 == 5) { b = (w >> 20) & 31; f7 = w >> 25; }  /* shamt + funct7 */
                else b = (uint32_t)((int32_t)w >> 20);                          /* I-immediate */
                goto alu;
            case OP:                                          /* reg-register */
                a = s->x[rs1]; b = s->x[(w >> 20) & 31]; f7 = w >> 25;
                if (f7 == 1) goto mul;
                goto alu;
            case LOAD:
                a = s->x[rs1]; imm = (uint32_t)((int32_t)w >> 20); goto load;
            case STORE:
                a = s->x[rs1]; b = s->x[(w >> 20) & 31];
                imm = (uint32_t)(((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f)); goto store;
            case BRANCH:
                a = s->x[rs1]; b = s->x[(w >> 20) & 31];
                imm = sext((w >> 31 & 1) << 12 | (w >> 7 & 1) << 11 |
                           (w >> 25 & 0x3f) << 5 | (w >> 8 & 0xf) << 1, 13); goto branch;
            case JAL:
                imm = sext((w >> 31 & 1) << 20 | (w >> 12 & 0xff) << 12 |
                           (w >> 20 & 1) << 11 | (w >> 21 & 0x3ff) << 1, 21); goto jal;
            case JALR:
                a = s->x[rs1]; imm = (uint32_t)((int32_t)w >> 20); goto jalr;
            case LUI:   imm = w & 0xfffff000; goto lui;
            case AUIPC: imm = w & 0xfffff000; goto auipc;
            case MISCMEM:                                     /* FENCE / FENCE.I -> nop */
                if (rd || rs1 || (f3 != 0 && f3 != 1)) goto illegal;
                goto done;
            case SYSTEM:
                imm = (w >> 20) & 0xfff; goto system;
            default: goto illegal;
            }
        } else {                                              /* ---- compressed ---- */
            uint16_t c = lo;
            uint32_t rdp = ((c >> 2) & 7) + 8, rsp = ((c >> 7) & 7) + 8;  /* x8..x15 fields */
            uint32_t shamt = ((c >> 2) & 0x1f) | ((c >> 12) & 1) << 5;
            int32_t  ci = sext(shamt, 6);                                 /* CI 6-bit imm */
            inst = lo;
            width = 2;
            rd = (c >> 7) & 31;
            switch (((c >> 13) & 7) << 2 | (c & 3)) {
            case 0 << 2 | 0:                          /* C.ADDI4SPN -> addi rd', sp, uimm */
                imm = ((c >> 11) & 3) << 4 | ((c >> 7) & 0xf) << 6 |
                      ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 3;
                if (!imm) goto illegal;               /* reserved */
                rd = rdp; a = s->x[2]; b = imm; goto alu;
            case 2 << 2 | 0:                          /* C.LW -> lw rd', uimm(rs1') */
                rd = rdp; a = s->x[rsp]; f3 = 2;
                imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6; goto load;
            case 6 << 2 | 0:                          /* C.SW -> sw rs2', uimm(rs1') */
                a = s->x[rsp]; b = s->x[rdp]; f3 = 2;
                imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6; goto store;
            case 0 << 2 | 1:                          /* C.ADDI / C.NOP -> addi rd, rd, imm */
                a = s->x[rd]; b = ci; goto alu;
            case 1 << 2 | 1: rd = 1;                  /* C.JAL -> jal ra, off */
                goto cj;
            case 5 << 2 | 1: rd = 0;                  /* C.J -> jal x0, off */
            cj: imm = sext(((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 | ((c >> 9) & 3) << 8 |
                           ((c >> 8) & 1) << 10 | ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                           ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5, 12);
                goto jal;
            case 2 << 2 | 1:                          /* C.LI -> addi rd, x0, imm */
                b = ci; goto alu;                     /* a defaults to 0 */
            case 3 << 2 | 1:                          /* C.LUI / C.ADDI16SP */
                if (rd == 2) {                        /* addi sp, sp, nzimm */
                    imm = sext(((c >> 6) & 1) << 4 | ((c >> 2) & 1) << 5 | ((c >> 5) & 1) << 6 |
                               ((c >> 3) & 3) << 7 | ((c >> 12) & 1) << 9, 10);
                    if (!imm) goto illegal;
                    a = s->x[2]; b = imm; goto alu;
                }
                imm = sext(((c >> 2) & 0x1f) << 12 | ((c >> 12) & 1) << 17, 18);  /* lui rd, nzimm */
                if (!imm) goto illegal;
                goto lui;
            case 4 << 2 | 1:                          /* C.MISC-ALU */
                rd = rsp; a = s->x[rsp];
                switch ((c >> 10) & 3) {
                case 0: f3 = 5; b = shamt; goto alu;              /* C.SRLI */
                case 1: f3 = 5; f7 = 0x20; b = shamt; goto alu;   /* C.SRAI */
                case 2: f3 = 7; b = ci; goto alu;                 /* C.ANDI */
                default:
                    if ((c >> 12) & 1) goto illegal;  /* C.SUBW/C.ADDW: RV64 only */
                    b = s->x[rdp];
                    switch ((c >> 5) & 3) {
                    case 0: f7 = 0x20; goto alu;      /* C.SUB */
                    case 1: f3 = 4; goto alu;         /* C.XOR */
                    case 2: f3 = 6; goto alu;         /* C.OR  */
                    default: f3 = 7; goto alu;        /* C.AND */
                    }
                }
            case 6 << 2 | 1: a = s->x[rsp];           /* C.BEQZ -> beq rs1', x0, off */
                goto cb;
            case 7 << 2 | 1: a = s->x[rsp]; f3 = 1;   /* C.BNEZ -> bne rs1', x0, off */
            cb: imm = sext(((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 | ((c >> 5) & 3) << 6 |
                           ((c >> 3) & 3) << 1 | ((c >> 2) & 1) << 5, 9);
                goto branch;                          /* b defaults to 0 */
            case 0 << 2 | 2:                          /* C.SLLI -> slli rd, rd, shamt */
                a = s->x[rd]; f3 = 1; b = shamt; goto alu;
            case 2 << 2 | 2:                          /* C.LWSP -> lw rd, uimm(sp) */
                a = s->x[2]; f3 = 2;
                imm = ((c >> 4) & 7) << 2 | ((c >> 2) & 3) << 6 | ((c >> 12) & 1) << 5; goto load;
            case 4 << 2 | 2: {                        /* C.JR / C.JALR / C.MV / C.ADD / C.EBREAK */
                uint32_t rs2 = (c >> 2) & 31;
                if (!((c >> 12) & 1)) {
                    if (rs2 == 0) { a = s->x[rd]; rd = 0; goto jalr; }   /* C.JR */
                    b = s->x[rs2]; rd = (c >> 7) & 31; goto alu;          /* C.MV -> add rd, x0, rs2 */
                }
                if (rd == 0 && rs2 == 0) goto ebreak;                    /* C.EBREAK */
                if (rs2 == 0) { a = s->x[rd]; rd = 1; goto jalr; }        /* C.JALR */
                a = s->x[rd]; b = s->x[rs2]; goto alu;                    /* C.ADD */
            }
            case 6 << 2 | 2:                          /* C.SWSP -> sw rs2, uimm(sp) */
                a = s->x[2]; b = s->x[(c >> 2) & 31]; f3 = 2;
                imm = ((c >> 9) & 0xf) << 2 | ((c >> 7) & 3) << 6; goto store;
            default: goto illegal;
            }
        }

        /* ---- shared action bodies (both encodings goto these) ---- */
    alu:
        switch (f3) {
        case 0: r = f7 == 0x20 ? a - b : a + b; break;                 /* ADD / SUB */
        case 1: r = a << (b & 31); break;                              /* SLL       */
        case 2: r = (int32_t)a < (int32_t)b; break;                    /* SLT       */
        case 3: r = a < b; break;                                      /* SLTU      */
        case 4: r = a ^ b; break;                                      /* XOR       */
        case 5: r = f7 == 0x20 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break; /* SRL/SRA */
        case 6: r = a | b; break;                                      /* OR        */
        default: r = a & b; break;                                     /* AND       */
        }
        wr(s, rd, r);
        goto done;
    mul:                                                              /* RV32M: rd = a <op> b */
        switch (f3) {
        case 0: r = a * b; break;                                                   /* MUL    */
        case 1: r = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32); break;    /* MULH   */
        case 2: r = (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32); break; /* MULHSU */
        case 3: r = (uint32_t)(((uint64_t)a * b) >> 32); break;                     /* MULHU  */
        case 4: r = b == 0 ? 0xffffffffu                                            /* DIV    */
                    : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
        case 5: r = b == 0 ? 0xffffffffu : a / b; break;                            /* DIVU   */
        case 6: r = b == 0 ? a                                                      /* REM    */
                    : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
        default: r = b == 0 ? a : a % b; break;                                     /* REMU   */
        }
        wr(s, rd, r);
        goto done;
    load:
        if (f3 == 3 || f3 > 5) goto illegal;
        addr = a + imm; n = 1u << (f3 & 3);
        if (rd) {                             /* a load into x0 has no effect */
            uint8_t *p = mem_xlate(s, addr, n, 0);            /* translate once */
            uint32_t v;
            if (p) switch (f3) {                              /* dispatch by width + sign */
            case 0:  v = (uint32_t)(int32_t)(int8_t)p[0]; break;                        /* LB  */
            case 1:  v = (uint32_t)(int32_t)(int16_t)(p[0] | p[1] << 8); break;         /* LH  */
            case 2:  v = (uint32_t)p[0] | (uint32_t)p[1] << 8
                       | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; break;            /* LW  */
            case 4:  v = p[0]; break;                                                   /* LBU */
            default: v = (uint32_t)p[0] | (uint32_t)p[1] << 8; break;                   /* LHU */
            } else {                                          /* miss -> callback supplies it */
                v = s->callback ? s->callback(s, RISCV_MEM_LOAD, addr, 0) : 0;
                v = f3 & 4 ? (n == 4 ? v : v & ((1u << (8 * n)) - 1))
                           : (uint32_t)sext(v, 8 * n);
            }
            wr(s, rd, v);
        }
        goto done;
    store:
        if (f3 > 2) goto illegal;
        addr = a + imm; n = 1u << f3;
        {
            uint8_t *p = mem_xlate(s, addr, n, 1);            /* translate once (write) */
            if (p) switch (f3) {                              /* dispatch by width */
            case 0:  p[0] = (uint8_t)b; break;                                          /* SB */
            case 1:  p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8); break;                 /* SH */
            default: p[0] = (uint8_t)b;        p[1] = (uint8_t)(b >> 8);
                     p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24); break;        /* SW */
            } else if (s->callback) s->callback(s, RISCV_MEM_STORE, addr, b);
        }
        goto done;
    branch: {
        int take;
        switch (f3) {
        case 0: take = a == b; break;                                  /* BEQ  */
        case 1: take = a != b; break;                                  /* BNE  */
        case 4: take = (int32_t)a < (int32_t)b; break;                 /* BLT  */
        case 5: take = (int32_t)a >= (int32_t)b; break;                /* BGE  */
        case 6: take = a < b; break;                                   /* BLTU */
        case 7: take = a >= b; break;                                  /* BGEU */
        default: goto illegal;
        }
        if (take) { pc = pc + imm; break; }       /* taken: assign pc, leave the window */
        goto done;
    }
    jal:
        wr(s, rd, pc + width); pc = pc + imm; break;
    jalr:
        addr = (a + imm) & ~1u; wr(s, rd, pc + width); pc = addr; break;
    lui:
        wr(s, rd, imm); goto done;
    auipc:
        wr(s, rd, pc + imm); goto done;
    ebreak:
        s->pc = pc; s->inst = inst;
        if (s->ebreak ? s->ebreak(s) : 1) goto out;    /* default: a breakpoint stops */
        goto done;
    system:
        if (f3 == 0 && rd == 0 && rs1 == 0) {
            if (imm == 0x000) {                        /* ECALL -> handler (syscall) */
                s->pc = pc; s->inst = inst;
                if (s->ecall && s->ecall(s)) goto out; /* nonzero (e.g. exit) stops at the ecall */
                goto done;
            }
            if (imm == 0x001) goto ebreak;             /* EBREAK */
        }
        /* CSR / MRET / WFI: not a userspace engine -- fall through to illegal. */
    illegal:                                           /* rare: call the handler directly */
        s->pc = pc; s->inst = inst;
        if (s->illegal ? s->illegal(s) : 1) goto out;  /* default: illegal stops */
    done:                                              /* sequential: advance pc, stay in window */
        pc += width;
        continue;
        }   /* inner: run the safe window straight-line */
    }       /* outer: re-validate pc after each control transfer / window end */
    out:
    s->pc = pc;                          /* leave the resume point in the state */
    return k;
}

/* ------------------------------------------------------------------ init */

/* The default memory callback: a no-op that delivers zero to a missed load. */
static uint32_t default_callback(RiscvEmulatorState_t *s, int op, uint32_t addr, uint32_t value) {
    (void)s; (void)op; (void)addr; (void)value;
    return 0;
}

void RiscvEmulatorInit(RiscvEmulatorState_t *s,
                       RiscvEmulatorRegion_t code, RiscvEmulatorRegion_t rodata,
                       RiscvEmulatorRegion_t heap, RiscvEmulatorRegion_t stack) {
    memset(s, 0, sizeof *s);              /* clears registers, regions, handlers */
    s->region[RISCV_CODE]   = code;       /* [0, len): instruction fetch + read-only data */
    s->region[RISCV_RODATA] = rodata;     /* read-only data, just below RISCV_HALF */
    s->region[RISCV_HEAP]   = heap;       /* read/write, from RISCV_HALF up */
    s->region[RISCV_STACK]  = stack;      /* read/write, grows down, ends at the last address */
    s->callback = default_callback;       /* no-op until the host installs its own */
    s->x[2] = 0;                          /* sp = 0 == 2^32, one past the top of the stack */
    s->pc = 0;                            /* code is based at 0 */
}
