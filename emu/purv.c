/*
 * purv.c - RISC-V (RV32IMC + Zifencei) userspace emulator: implementation.
 *
 * A user-level program runner, not a machine: no CSRs, no privileged modes, no
 * trap-to-mtvec. It is self-contained -- no host hooks. The state (defined in
 * purv.h, public) holds everything: the 8 memory regions, evenly spaced 256 MiB
 * apart from RAM_ORIGIN, and the ecall/ebreak/illegal handlers. Data loads/stores
 * resolve straight into the regions (OOB reads give zero, OOB / read-only stores
 * are dropped); the handlers return nonzero to stop the run loop.
 *
 * RiscvEmulatorLoop runs a batch in one call -- the fetch/decode/execute step is
 * the body of an internal loop, so there is no per-instruction call boundary --
 * fetching instructions from a code window the caller hands in.
 *
 * Structure: each opcode -- 32-bit or compressed -- decodes only the operands it
 * needs into a few locals, then `goto`s a shared action body. Registers are
 * indices into x[32]; x0 is never written, so it reads as a hard zero.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "purv.h"

#define ROM_ORIGIN  0x20000000u           /* reset vector if the host sets no pc */

/* Base opcodes (instruction[6:0]). */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

/* ---- memory: resolve a guest address to one of the regions ---- */
/* A load of n bytes, zero-extended; out of range reads zero. */
static uint32_t mem_load(const RiscvEmulatorState_t *s, uint32_t addr, uint32_t n) {
    uint32_t v = 0;
    if (addr >= RAM_ORIGIN) {
        const RiscvEmulatorRegion_t *r = &s->mem[(addr - RAM_ORIGIN) / RISCV_REGION_SIZE];
        uint32_t off = (addr - RAM_ORIGIN) % RISCV_REGION_SIZE;
        if (r->ptr && off + n <= r->len) memcpy(&v, r->ptr + off, n);
    }
    return v;
}
/* A store of the low n bytes of val; dropped if out of range or read-only. */
static void mem_store(RiscvEmulatorState_t *s, uint32_t addr, uint32_t n, uint32_t val) {
    if (addr >= RAM_ORIGIN) {
        RiscvEmulatorRegion_t *r = &s->mem[(addr - RAM_ORIGIN) / RISCV_REGION_SIZE];
        uint32_t off = (addr - RAM_ORIGIN) % RISCV_REGION_SIZE;
        if (r->ptr && r->writable && off + n <= r->len) memcpy(r->ptr + off, &val, n);
    }
}

/* Sign-extend the low `bits` of v. */
static int32_t sext(uint32_t v, int bits) {
    int sh = 32 - bits;
    return (int32_t)(v << sh) >> sh;
}

static void wr(RiscvEmulatorState_t *s, uint32_t i, uint32_t v) {
    if (i) s->x[i] = v;               /* writes to x0 are discarded */
}

/* The M extension: rd = a <op> b, selected by funct3. */
static uint32_t muldiv(uint32_t f3, uint32_t a, uint32_t b) {
    switch (f3) {
    case 0: return a * b;                                                   /* MUL    */
    case 1: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);    /* MULH   */
    case 2: return (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32); /* MULHSU */
    case 3: return (uint32_t)(((uint64_t)a * b) >> 32);                     /* MULHU  */
    case 4: return b == 0 ? 0xffffffffu                                     /* DIV    */
                 : (a == 0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b);
    case 5: return b == 0 ? 0xffffffffu : a / b;                            /* DIVU   */
    case 6: return b == 0 ? a                                               /* REM    */
                 : (a == 0x80000000u && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
    default: return b == 0 ? a : a % b;                                     /* REMU   */
    }
}

/* ----------------------------------------------------------------- the VM */

uint64_t RiscvEmulatorLoop(RiscvEmulatorState_t *s,
                           const uint8_t *code, uint32_t code_len, uint32_t code_base,
                           uint64_t max) {
    uint32_t pc = s->pc;              /* resume where we left off */
    uint64_t k = 0;
    int stop = 0;

    /* Each iteration runs one instruction; the whole step is inlined here, so
     * there is no per-instruction call. */
    for (; k < max && !stop; k++) {
        /* Fetch from the caller's code window. A pc not (fully) inside it ends
         * the batch -- the engine fetches only from the provided code. */
        uint32_t off = pc - code_base;
        if (pc < code_base || off + 2 > code_len) break;

        uint32_t rd = 0, rs1 = 0, a = 0, b = 0, f3 = 0, f7 = 0, imm = 0, r, addr, n;

        uint16_t lo = (uint16_t)(code[off] | code[off + 1] << 8);
        if ((lo & 3) == 3) {                                  /* ---- 32-bit ---- */
            if (off + 4 > code_len) break;                    /* op straddles window end */
            uint32_t w = lo | (uint32_t)(code[off + 2] | code[off + 3] << 8) << 16;
            s->inst = w;
            s->npc = pc + 4;
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
            s->inst = lo;
            s->npc = pc + 2;
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
    mul:
        wr(s, rd, muldiv(f3, a, b));
        goto done;
    load:
        if (f3 == 3 || f3 > 5) goto illegal;
        addr = a + imm; n = 1u << (f3 & 3);
        if (rd) {                             /* a load into x0 has no effect */
            uint32_t v = mem_load(s, addr, n);
            wr(s, rd, f3 & 4 ? v : (uint32_t)sext(v, 8 << (f3 & 3)));  /* sign-ext unless LBU/LHU */
        }
        goto done;
    store:
        if (f3 > 2) goto illegal;
        addr = a + imm; n = 1u << f3;
        mem_store(s, addr, n, b);
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
        if (take) s->npc = pc + imm;
        goto done;
    }
    jal:
        wr(s, rd, s->npc); s->npc = pc + imm; goto done;
    jalr:
        addr = (a + imm) & ~1u; wr(s, rd, s->npc); s->npc = addr; goto done;
    lui:
        wr(s, rd, imm); goto done;
    auipc:
        wr(s, rd, pc + imm); goto done;
    ebreak:
        s->pc = pc;
        if (s->ebreak ? s->ebreak(s) : 1) stop = 1;    /* default: a breakpoint stops */
        goto done;
    system:
        if (f3 == 0 && rd == 0 && rs1 == 0) {
            if (imm == 0x000) {                        /* ECALL -> handler (syscall) */
                s->pc = pc;
                if (s->ecall && s->ecall(s)) stop = 1; /* default: nop, keep running */
                goto done;
            }
            if (imm == 0x001) goto ebreak;             /* EBREAK */
        }
        goto illegal;                                  /* CSR / MRET / WFI: not a userspace engine */
    illegal:
        s->trap = 1;
    done:
        if (s->trap) {
            s->pc = pc;
            if (s->illegal ? s->illegal(s) : 1) stop = 1;   /* default: illegal stops */
            s->trap = 0;
        }
        pc = s->npc;
    }
    s->pc = pc;                          /* leave the resume point in the state */
    return k;
}

/* ----------------------------------------------------------- lifecycle + memory */

void RiscvEmulatorInit(RiscvEmulatorState_t *s, uint32_t initial_sp) {
    memset(s, 0, sizeof *s);              /* clears registers, memory map, handlers */
    s->x[2] = initial_sp;                 /* sp */
    s->pc = s->npc = ROM_ORIGIN;
}

RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp) {
    RiscvEmulatorState_t *s = calloc(1, sizeof *s);
    if (s) RiscvEmulatorInit(s, initial_sp);
    return s;
}
void RiscvEmulatorDestroy(RiscvEmulatorState_t *s) { free(s); }

void RiscvEmulatorReadMemory(const RiscvEmulatorState_t *s, uint32_t addr, void *dst, uint32_t len) {
    uint8_t *d = dst;
    for (uint32_t i = 0; i < len; i++) d[i] = (uint8_t)mem_load(s, addr + i, 1);
}
void RiscvEmulatorWriteMemory(RiscvEmulatorState_t *s, uint32_t addr, const void *src, uint32_t len) {
    const uint8_t *b = src;
    for (uint32_t i = 0; i < len; i++) mem_store(s, addr + i, 1, b[i]);
}
