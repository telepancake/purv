/*
 * purv.c - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: implementation.
 *
 * A small, hand-written engine. The premise is atoom's: the host owns the memory
 * map and trap policy and the engine reaches the world only through the hooks in
 * purv.h. The whole thing is one fetch/decode/execute step in RiscvEmulatorLoop.
 *
 * Two ideas keep it small:
 *   - Registers are indices into x[32], not pointers. x0 is never written, so it
 *     reads as a hard zero; wr() drops writes to it.
 *   - A 16-bit compressed instruction is *decompressed* into the same decoded
 *     form a 32-bit instruction decodes to, so there is a single executor. All
 *     the per-encoding work lives in the decoders; execute() never sees "C".
 */
#include <stdint.h>
#include <stdlib.h>

#include "purv.h"

#define ROM_ORIGIN 0x20000000u

/* Base opcodes (instruction[6:0]); compressed forms decode into these too. */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

/* Pending-trap flags (a small bitmask; the host can clear it). */
enum { T_ILL = 1, T_IMIS = 2, T_EBRK = 4, T_LMIS = 8, T_SMIS = 16, T_ECALL = 32 };

struct RiscvEmulatorState {
    uint32_t pc;        /* instruction currently executing */
    uint32_t npc;       /* next instruction (jumps/branches/traps rewrite this) */
    uint32_t inst;      /* raw fetched word (16-bit zero-extended when compressed) */
    uint32_t x[32];     /* x0..x31; x0 stays zero */
    uint8_t  trap;      /* T_* bitmask of pending exceptions */
    /* CSRs the engine implements, as plain 32-bit words. Bit-fields are just a
     * view the trap logic applies with masks; reads/writes are raw words. */
    uint32_t mhartid, mstatus, medeleg, mideleg, mie, mtvec, mstatush, mscratch,
             mepc, mcause, mtval, mip, pmpcfg0, pmpaddr0, mnstatus, satp;
};

/* One decoded instruction, in the canonical RV form both encodings produce. */
typedef struct {
    uint8_t op, rd, rs1, rs2, f3, f7;   /* opcode, registers, funct3, funct7 */
    int32_t imm;                        /* sign-extended immediate / CSR number */
} Decoded;

static void *RiscvEmulatorGetCSRAddress(RiscvEmulatorState_t *s, uint16_t csr) {
    switch (csr) {
    case 0xF14: return &s->mhartid;
    case 0x300: return &s->mstatus;
    case 0x302: return &s->medeleg;
    case 0x303: return &s->mideleg;
    case 0x304: return &s->mie;
    case 0x305: return &s->mtvec;
    case 0x310: return &s->mstatush;
    case 0x340: return &s->mscratch;
    case 0x341: return &s->mepc;
    case 0x342: return &s->mcause;
    case 0x343: return &s->mtval;
    case 0x344: return &s->mip;
    case 0x3A0: return &s->pmpcfg0;
    case 0x3B0: return &s->pmpaddr0;
    case 0x744: return &s->mnstatus;
    case 0x180: return &s->satp;
    default: {
        void *a = RiscvEmulatorGetUnknownCSR(s, csr);
        if (!a) { s->trap |= T_ILL; RiscvEmulatorUnknownCSR(s); }
        return a;
    }
    }
}

/* ----------------------------------------------------------------- decoding */

/* Sign-extend the low `bits` of v. */
static int32_t sext(uint32_t v, int bits) {
    int sh = 32 - bits;
    return (int32_t)(v << sh) >> sh;
}

/* Read a memory operand of n bytes, zero-extended into a 32-bit word. */
static uint32_t memload(uint32_t addr, uint8_t n) {
    uint32_t v = 0;
    RiscvEmulatorLoad(addr, &v, n);
    return v;
}

/* Decode a 32-bit instruction word. */
static Decoded decode32(uint32_t w) {
    Decoded d;
    d.op = w & 0x7f;
    d.rd = (w >> 7) & 31;
    d.f3 = (w >> 12) & 7;
    d.rs1 = (w >> 15) & 31;
    d.rs2 = (w >> 20) & 31;
    d.f7 = (w >> 25) & 0x7f;
    switch (d.op) {
    case OPIMM: case LOAD: case JALR:
        d.imm = (int32_t)w >> 20; break;                       /* I-type */
    case STORE:
        d.imm = ((int32_t)w >> 25 << 5) | ((w >> 7) & 0x1f); break;
    case BRANCH:
        d.imm = sext(((w >> 31) & 1) << 12 | ((w >> 7) & 1) << 11 |
                     ((w >> 25) & 0x3f) << 5 | ((w >> 8) & 0xf) << 1, 13); break;
    case JAL:
        d.imm = sext(((w >> 31) & 1) << 20 | ((w >> 12) & 0xff) << 12 |
                     ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3ff) << 1, 21); break;
    case LUI: case AUIPC:
        d.imm = (int32_t)(w & 0xfffff000u); break;             /* U-type */
    case SYSTEM:
        d.imm = (w >> 20) & 0xfff; break;                      /* CSR / funct12 */
    default:
        d.imm = 0;
    }
    return d;
}

/* Decompress a 16-bit instruction into the same decoded form. Reserved/illegal
 * encodings come back with op == 0, which execute() rejects. The popular three-
 * bit register fields name x8..x15, hence the +8. */
static Decoded decode16(uint16_t c) {
    Decoded d = {0};
    uint8_t rdp = ((c >> 2) & 7) + 8;     /* rd'/rs2' at bits 4:2 */
    uint8_t rsp = ((c >> 7) & 7) + 8;     /* rs1'/rd' at bits 9:7 */
    uint8_t rd = (c >> 7) & 31, rs2 = (c >> 2) & 31;
    uint8_t shamt = ((c >> 2) & 0x1f) | ((c >> 12) & 1) << 5;
    int32_t ci = sext(((c >> 2) & 0x1f) | ((c >> 12) & 1) << 5, 6);  /* CI 6-bit */
    switch (((c >> 13) & 7) << 2 | (c & 3)) {
    case 0 << 2 | 0:                      /* C.ADDI4SPN -> addi rd', sp, uimm */
        d.imm = ((c >> 11) & 3) << 4 | ((c >> 7) & 0xf) << 6 |
                ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 3;
        if (d.imm) { d.op = OPIMM; d.rd = rdp; d.rs1 = 2; }
        break;
    case 2 << 2 | 0:                      /* C.LW -> lw rd', uimm(rs1') */
        d.op = LOAD; d.f3 = 2; d.rd = rdp; d.rs1 = rsp;
        d.imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6;
        break;
    case 6 << 2 | 0:                      /* C.SW -> sw rs2', uimm(rs1') */
        d.op = STORE; d.f3 = 2; d.rs1 = rsp; d.rs2 = rdp;
        d.imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6;
        break;
    case 0 << 2 | 1:                      /* C.ADDI / C.NOP -> addi rd, rd, imm */
        d.op = OPIMM; d.rd = rd; d.rs1 = rd; d.imm = ci;
        break;
    case 1 << 2 | 1:                      /* C.JAL -> jal ra, off (RV32) */
        d.op = JAL; d.rd = 1;
        goto cj;
    case 5 << 2 | 1:                      /* C.J -> jal x0, off */
        d.op = JAL;
    cj: d.imm = sext(((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 |
                     ((c >> 9) & 3) << 8 | ((c >> 8) & 1) << 10 |
                     ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                     ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5, 12);
        break;
    case 2 << 2 | 1:                      /* C.LI -> addi rd, x0, imm */
        d.op = OPIMM; d.rd = rd; d.imm = ci;
        break;
    case 3 << 2 | 1:                      /* C.LUI / C.ADDI16SP */
        if (rd == 2) {                    /* addi sp, sp, nzimm */
            d.imm = sext(((c >> 6) & 1) << 4 | ((c >> 2) & 1) << 5 |
                         ((c >> 5) & 1) << 6 | ((c >> 3) & 3) << 7 |
                         ((c >> 12) & 1) << 9, 10);
            if (d.imm) { d.op = OPIMM; d.rd = 2; d.rs1 = 2; }
        } else {                          /* lui rd, nzimm */
            d.imm = sext(((c >> 2) & 0x1f) << 12 | ((c >> 12) & 1) << 17, 18);
            if (d.imm) { d.op = LUI; d.rd = rd; }
        }
        break;
    case 4 << 2 | 1:                      /* C.MISC-ALU */
        d.rd = rsp; d.rs1 = rsp;
        switch ((c >> 10) & 3) {
        case 0: d.op = OPIMM; d.f3 = 5; d.rs2 = shamt; break;            /* SRLI */
        case 1: d.op = OPIMM; d.f3 = 5; d.f7 = 0x20; d.rs2 = shamt; break; /* SRAI */
        case 2: d.op = OPIMM; d.f3 = 7; d.imm = ci; break;              /* ANDI */
        case 3:                                                          /* register ALU */
            if ((c >> 12) & 1) break;     /* C.SUBW/C.ADDW: RV64 only -> illegal */
            d.op = OP; d.rs2 = rdp;
            switch ((c >> 5) & 3) {
            case 0: d.f7 = 0x20; break;   /* SUB */
            case 1: d.f3 = 4; break;      /* XOR */
            case 2: d.f3 = 6; break;      /* OR  */
            case 3: d.f3 = 7; break;      /* AND */
            }
        }
        break;
    case 6 << 2 | 1:                      /* C.BEQZ -> beq rs1', x0, off */
        d.op = BRANCH; d.rs1 = rsp;
        goto cb;
    case 7 << 2 | 1:                      /* C.BNEZ -> bne rs1', x0, off */
        d.op = BRANCH; d.f3 = 1; d.rs1 = rsp;
    cb: d.imm = sext(((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 |
                     ((c >> 5) & 3) << 6 | ((c >> 3) & 3) << 1 |
                     ((c >> 2) & 1) << 5, 9);
        break;
    case 0 << 2 | 2:                      /* C.SLLI -> slli rd, rd, shamt */
        d.op = OPIMM; d.f3 = 1; d.rd = rd; d.rs1 = rd; d.rs2 = shamt;
        break;
    case 2 << 2 | 2:                      /* C.LWSP -> lw rd, uimm(sp) */
        d.op = LOAD; d.f3 = 2; d.rd = rd; d.rs1 = 2;
        d.imm = ((c >> 4) & 7) << 2 | ((c >> 2) & 3) << 6 | ((c >> 12) & 1) << 5;
        break;
    case 4 << 2 | 2:                      /* C.JR / C.JALR / C.MV / C.ADD / C.EBREAK */
        if (!((c >> 12) & 1)) {
            if (rs2 == 0) { d.op = JALR; d.rs1 = rd; }            /* C.JR */
            else { d.op = OP; d.rd = rd; d.rs2 = rs2; }           /* C.MV */
        } else if (rd == 0 && rs2 == 0) {
            d.op = SYSTEM; d.imm = 1;                             /* C.EBREAK */
        } else if (rs2 == 0) {
            d.op = JALR; d.rd = 1; d.rs1 = rd;                   /* C.JALR */
        } else {
            d.op = OP; d.rd = rd; d.rs1 = rd; d.rs2 = rs2;       /* C.ADD */
        }
        break;
    case 6 << 2 | 2:                      /* C.SWSP -> sw rs2, uimm(sp) */
        d.op = STORE; d.f3 = 2; d.rs1 = 2; d.rs2 = rs2;
        d.imm = ((c >> 9) & 0xf) << 2 | ((c >> 7) & 3) << 6;
        break;
    }
    return d;
}

/* ---------------------------------------------------------------- execution */

static void wr(RiscvEmulatorState_t *s, uint8_t i, uint32_t v) {
    if (i) s->x[i] = v;               /* writes to x0 are discarded */
}

/* Execute one decoded instruction. Effects land in s->x / memory / s->npc, and
 * any exception is recorded in s->trap for Loop's epilogue. */
static void execute(RiscvEmulatorState_t *s, Decoded d) {
    uint32_t a = s->x[d.rs1], b = s->x[d.rs2], imm = (uint32_t)d.imm;
    switch (d.op) {
    case OPIMM:
        switch (d.f3) {
        case 0: wr(s, d.rd, a + imm); break;                         /* ADDI  */
        case 2: wr(s, d.rd, (int32_t)a < d.imm); break;              /* SLTI  */
        case 3: wr(s, d.rd, a < imm); break;                         /* SLTIU */
        case 4: wr(s, d.rd, a ^ imm); break;                         /* XORI  */
        case 6: wr(s, d.rd, a | imm); break;                         /* ORI   */
        case 7: wr(s, d.rd, a & imm); break;                         /* ANDI  */
        case 1:                                                      /* SLLI  */
            if (d.f7) s->trap |= T_ILL;
            else wr(s, d.rd, a << (d.rs2 & 31));
            break;
        case 5:                                                      /* SRLI / SRAI */
            if (d.f7 == 0) wr(s, d.rd, a >> (d.rs2 & 31));
            else if (d.f7 == 0x20) wr(s, d.rd, (int32_t)a >> (d.rs2 & 31));
            else s->trap |= T_ILL;
            break;
        }
        break;
    case OP:
        if (d.f7 == 0) switch (d.f3) {
        case 0: wr(s, d.rd, a + b); break;                           /* ADD  */
        case 1: wr(s, d.rd, a << (b & 31)); break;                   /* SLL  */
        case 2: wr(s, d.rd, (int32_t)a < (int32_t)b); break;         /* SLT  */
        case 3: wr(s, d.rd, a < b); break;                           /* SLTU */
        case 4: wr(s, d.rd, a ^ b); break;                           /* XOR  */
        case 5: wr(s, d.rd, a >> (b & 31)); break;                   /* SRL  */
        case 6: wr(s, d.rd, a | b); break;                           /* OR   */
        case 7: wr(s, d.rd, a & b); break;                           /* AND  */
        }
        else if (d.f7 == 0x20 && d.f3 == 0) wr(s, d.rd, a - b);      /* SUB  */
        else if (d.f7 == 0x20 && d.f3 == 5)                         /* SRA  */
            wr(s, d.rd, (int32_t)a >> (b & 31));
        else if (d.f7 == 1) switch (d.f3) {                         /* RV32M */
        case 0: wr(s, d.rd, a * b); break;                           /* MUL    */
        case 1: wr(s, d.rd, (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32)); break;
        case 2: wr(s, d.rd, (uint32_t)(((int64_t)((uint64_t)(int32_t)a * b)) >> 32)); break;
        case 3: wr(s, d.rd, (uint32_t)(((uint64_t)a * b) >> 32)); break;
        case 4: wr(s, d.rd, b == 0 ? 0xffffffffu                     /* DIV  */
                  : (a == 0x80000000u && (int32_t)b == -1) ? a
                  : (uint32_t)((int32_t)a / (int32_t)b)); break;
        case 5: wr(s, d.rd, b == 0 ? 0xffffffffu : a / b); break;    /* DIVU */
        case 6: wr(s, d.rd, b == 0 ? a                               /* REM  */
                  : (a == 0x80000000u && (int32_t)b == -1) ? 0
                  : (uint32_t)((int32_t)a % (int32_t)b)); break;
        case 7: wr(s, d.rd, b == 0 ? a : a % b); break;             /* REMU */
        }
        else s->trap |= T_ILL;
        break;
    case LOAD: {
        uint32_t addr = a + imm, n = d.f3 & 3 ? (d.f3 & 3) == 1 ? 2 : 4 : 1;
        if (d.f3 == 3 || d.f3 > 5) { s->trap |= T_ILL; break; }
        if (n > 1 && (addr & (n - 1))) { s->trap |= T_LMIS; s->mtval = addr; }
        else if (d.rd) {
            uint32_t v = memload(addr, n);
            wr(s, d.rd, d.f3 == 0 ? (uint32_t)(int8_t)v
                      : d.f3 == 1 ? (uint32_t)(int16_t)v : v);       /* LB/LH sign-ext */
        }
        break;
    }
    case STORE: {
        uint32_t addr = a + imm, n = d.f3 == 0 ? 1 : d.f3 == 1 ? 2 : 4;
        if (d.f3 > 2) { s->trap |= T_ILL; break; }
        if (n > 1 && (addr & (n - 1))) { s->trap |= T_SMIS; s->mtval = addr; }
        else RiscvEmulatorStore(addr, &b, n);
        break;
    }
    case BRANCH: {
        int take;
        switch (d.f3) {
        case 0: take = a == b; break;                               /* BEQ  */
        case 1: take = a != b; break;                               /* BNE  */
        case 4: take = (int32_t)a < (int32_t)b; break;              /* BLT  */
        case 5: take = (int32_t)a >= (int32_t)b; break;             /* BGE  */
        case 6: take = a < b; break;                                /* BLTU */
        case 7: take = a >= b; break;                               /* BGEU */
        default: s->trap |= T_ILL; return;
        }
        if (take) s->npc = s->pc + imm;
        break;
    }
    case JAL:  wr(s, d.rd, s->npc); s->npc = s->pc + imm; break;
    case JALR: { uint32_t t = (a + imm) & ~1u; wr(s, d.rd, s->npc); s->npc = t; } break;
    case LUI:   wr(s, d.rd, imm); break;
    case AUIPC: wr(s, d.rd, s->pc + imm); break;
    case MISCMEM:                          /* FENCE / FENCE.I are no-ops here */
        if (d.rd || d.rs1 || (d.f3 != 0 && d.f3 != 1)) s->trap |= T_ILL;
        break;
    case SYSTEM:
        if (d.f3 == 0 && d.rd == 0 && d.rs1 == 0) {
            switch (imm & 0xfff) {
            case 0x000: s->trap |= T_ECALL; RiscvEmulatorHandleECALL(s); break;
            case 0x001: s->trap |= T_EBRK; s->mtval = s->pc; RiscvEmulatorHandleEBREAK(s); break;
            case 0x302:                    /* MRET */
                s->mstatush &= ~(1u << 7);                          /* mpv = 0 */
                s->mstatus &= ~(3u << 11);                          /* mpp = 0 */
                s->mstatus = (s->mstatus & ~(1u << 3)) | ((s->mstatus >> 7 & 1) << 3); /* mie=mpie */
                s->mstatus |= 1u << 7;                              /* mpie = 1 */
                s->npc = s->mepc; break;
            default: s->trap |= T_ILL;
            }
        } else {
            uint32_t *csr = RiscvEmulatorGetCSRAddress(s, imm & 0xfff);
            if (s->trap) break;            /* unknown CSR already raised illegal */
            uint32_t old = *csr, src = d.f3 & 4 ? d.rs1 : a;        /* immediate vs register */
            if (d.rd) wr(s, d.rd, old);
            switch (d.f3 & 3) {
            case 1: *csr = src; break;                             /* CSRRW[I]  */
            case 2: if (src) *csr = old | src; break;              /* CSRRS[I]  */
            case 3: if (src) *csr = old & ~src; break;             /* CSRRC[I]  */
            default: s->trap |= T_ILL;
            }
        }
        break;
    default: s->trap |= T_ILL;
    }
}

/* ----------------------------------------------------------------- the VM */

uint32_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint32_t pc) {
    s->pc = pc;

    /* Fetch: 16 bits, plus another 16 when the low two bits mark a 32-bit op. */
    uint16_t lo = (uint16_t)memload(pc, 2);
    Decoded d;
    if ((lo & 3) == 3) {
        s->inst = lo | (uint32_t)memload(pc + 2, 2) << 16;
        s->npc = pc + 4;
        d = decode32(s->inst);
    } else {
        s->inst = lo;
        s->npc = pc + 2;
        d = decode16(lo);
    }

    execute(s, d);

    if (s->trap) {
        uint32_t code = s->trap & T_EBRK ? 3 : s->trap & T_LMIS ? 4
                      : s->trap & T_SMIS ? 6 : s->trap & T_ECALL ? 11 : 0;
        if (s->trap & T_ILL) { code = 2; s->mtval = s->inst; }
        s->mstatus = (s->mstatus & ~(3u << 11)) | (3u << 11);      /* mpp = M */
        s->mstatus = (s->mstatus & ~(1u << 7)) | ((s->mstatus >> 3 & 1) << 7); /* mpie=mie */
        s->mstatus &= ~(1u << 3);                                  /* mie = 0 */
        s->mepc = s->pc;
        s->mcause = code;
        s->npc = (s->mtvec & 3) == 1 ? 4 * code : s->mtvec & ~3u;  /* vectored vs direct */
        if (s->trap & T_ILL) RiscvEmulatorIllegalInstruction(s);
        s->trap = 0;
    }
    return s->npc;
}

/* ------------------------------------------------------------ public API glue */

void RiscvEmulatorInit(RiscvEmulatorState_t *s, uint32_t initial_sp) {
    for (int i = 0; i < 32; i++) s->x[i] = 0;
    s->x[2] = initial_sp;                 /* sp */
    s->pc = s->npc = ROM_ORIGIN;
    s->inst = 0;
    s->trap = 0;
    s->mhartid = s->mstatus = s->medeleg = s->mideleg = s->mie = s->mtvec = 0;
    s->mstatush = s->mscratch = s->mepc = s->mcause = s->mtval = s->mip = 0;
    s->pmpcfg0 = s->pmpaddr0 = s->mnstatus = s->satp = 0;
}

RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp) {
    RiscvEmulatorState_t *s = calloc(1, sizeof *s);
    if (s) RiscvEmulatorInit(s, initial_sp);
    return s;
}
void RiscvEmulatorDestroy(RiscvEmulatorState_t *s) { free(s); }

uint32_t RiscvEmulatorGetRegister(const RiscvEmulatorState_t *s, int index) {
    return s->x[index & 31];
}
void RiscvEmulatorSetRegister(RiscvEmulatorState_t *s, int index, uint32_t value) {
    if (index & 31) s->x[index & 31] = value;   /* x0 stays hard-wired zero */
}
uint32_t RiscvEmulatorGetProgramCounter(const RiscvEmulatorState_t *s) { return s->pc; }
uint32_t RiscvEmulatorGetNextProgramCounter(const RiscvEmulatorState_t *s) { return s->npc; }
void RiscvEmulatorSetProgramCounter(RiscvEmulatorState_t *s, uint32_t pc) { s->pc = s->npc = pc; }
uint32_t RiscvEmulatorGetInstruction(const RiscvEmulatorState_t *s) { return s->inst; }
uint16_t RiscvEmulatorGetCsrNumber(const RiscvEmulatorState_t *s) { return (s->inst >> 20) & 0xfff; }
uint32_t RiscvEmulatorGetTrapVectorBase(const RiscvEmulatorState_t *s) { return s->mtvec >> 2; }
void RiscvEmulatorRaiseIllegalInstruction(RiscvEmulatorState_t *s) { s->trap |= T_ILL; }
void RiscvEmulatorClearTrap(RiscvEmulatorState_t *s) { s->trap = 0; }
