/*
 * purv.c - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: implementation.
 *
 * A small, hand-written engine. The premise is atoom's: the host owns the memory
 * map and trap policy and the engine reaches the world only through the hooks in
 * purv.h. The whole thing is one fetch/decode/execute step in RiscvEmulatorLoop.
 *
 * Structure: each opcode -- 32-bit or compressed -- decodes only the operands it
 * needs straight from the instruction, into a handful of locals, then `goto`s a
 * shared action body (alu/load/store/branch/...). So the compressed and base
 * encodings reuse the *same* execution code without any intermediate decoded
 * form, and no instruction decodes a field it won't use. Registers are indices
 * into x[32], not pointers; x0 is never written, so it reads as a hard zero and
 * wr() drops writes to it.
 */
#include <stdint.h>
#include <stdlib.h>

#include "purv.h"

#define ROM_ORIGIN 0x20000000u

/* Base opcodes (instruction[6:0]). */
enum { LOAD = 0x03, MISCMEM = 0x0f, OPIMM = 0x13, AUIPC = 0x17, STORE = 0x23,
       OP = 0x33, LUI = 0x37, BRANCH = 0x63, JALR = 0x67, JAL = 0x6f, SYSTEM = 0x73 };

/* Pending-trap flags (a small bitmask; the host can clear it). */
enum { T_ILL = 1, T_EBRK = 4, T_LMIS = 8, T_SMIS = 16, T_ECALL = 32 };

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
    /* Cached fetch window (see RiscvEmulatorGetFetchWindow). While pc stays in
     * [fbase_lo, fbase_hi) the engine reads instruction half-words straight from
     * host memory at fptr instead of calling the load hook; on a miss it re-asks
     * the host. fptr == 0 means no window is cached (slow path / hook absent). */
    const uint8_t *fptr;
    uint32_t fbase_lo, fbase_hi;
};

/* Weak: a host need not provide it. An undefined weak symbol resolves to NULL,
 * so the engine simply keeps fetching through RiscvEmulatorLoad. Declared here
 * (not in purv.h's required-hook block) because it is an optional accelerator. */
__attribute__((weak)) const uint8_t *RiscvEmulatorGetFetchWindow(uint32_t address, uint32_t *available);

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

/* Fetch one instruction half-word at addr. The common case -- addr inside the
 * cached fetch window -- is two byte reads from a host pointer, no call. On a
 * miss we ask the host for a window covering addr (if it implements the optional
 * hook); failing that we fall back to the load hook. Half-word granularity keeps
 * RVC working and means a 32-bit op straddling the window's end still resolves. */
static inline uint16_t fetch16(RiscvEmulatorState_t *s, uint32_t addr) {
    if (s->fptr && addr >= s->fbase_lo && addr + 2 <= s->fbase_hi) {
        const uint8_t *p = s->fptr + (addr - s->fbase_lo);
        return (uint16_t)(p[0] | p[1] << 8);
    }
    if (RiscvEmulatorGetFetchWindow) {
        uint32_t avail = 0;
        const uint8_t *p = RiscvEmulatorGetFetchWindow(addr, &avail);
        if (p && avail >= 2) {
            s->fptr = p;                   /* p maps fbase_lo (== addr) */
            s->fbase_lo = addr;
            s->fbase_hi = addr + avail;
            return (uint16_t)(p[0] | p[1] << 8);
        }
        s->fptr = 0;                       /* no usable window here */
    }
    return (uint16_t)memload(addr, 2);
}

/* ----------------------------------------------------------------- the VM */

uint32_t RiscvEmulatorLoop(RiscvEmulatorState_t *s, uint32_t pc) {
    s->pc = pc;

    /* Operands the shared action bodies consume. Each decode arm fills only the
     * ones its action reads (defaults keep the rest harmless), then goto's it.
     * For shifts, b is the shift amount; for SUB/SRA, f7 == 0x20. */
    uint32_t rd = 0, rs1 = 0, a = 0, b = 0, f3 = 0, f7 = 0, imm = 0, r, addr, n;

    uint16_t lo = fetch16(s, pc);
    if ((lo & 3) == 3) {                                  /* ---- 32-bit ---- */
        uint32_t w = lo | (uint32_t)fetch16(s, pc + 2) << 16;
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
            a = s->x[rs1]; imm = (w >> 20) & 0xfff; goto system;
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
    if (addr & (n - 1)) { s->trap |= T_LMIS; s->mtval = addr; }
    else if (rd) {
        uint32_t v = memload(addr, n);
        wr(s, rd, f3 & 4 ? v : (uint32_t)sext(v, 8 << (f3 & 3)));  /* sign-ext unless LBU/LHU */
    }
    goto done;
store:
    if (f3 > 2) goto illegal;
    addr = a + imm; n = 1u << f3;
    if (addr & (n - 1)) { s->trap |= T_SMIS; s->mtval = addr; }
    else RiscvEmulatorStore(addr, &b, n);
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
    if (take) s->npc = s->pc + imm;
    goto done;
}
jal:
    wr(s, rd, s->npc); s->npc = s->pc + imm; goto done;
jalr:
    addr = (a + imm) & ~1u; wr(s, rd, s->npc); s->npc = addr; goto done;
lui:
    wr(s, rd, imm); goto done;
auipc:
    wr(s, rd, s->pc + imm); goto done;
ebreak:
    s->trap |= T_EBRK; s->mtval = s->pc; RiscvEmulatorHandleEBREAK(s); goto done;
system:
    if (f3 == 0 && rd == 0 && rs1 == 0) {                          /* ECALL/EBREAK/MRET */
        switch (imm) {
        case 0x000: s->trap |= T_ECALL; RiscvEmulatorHandleECALL(s); break;
        case 0x001: goto ebreak;
        case 0x302:                                                /* MRET */
            s->mstatush &= ~(1u << 7);                             /* mpv = 0 */
            s->mstatus &= ~(3u << 11);                             /* mpp = 0 */
            s->mstatus = (s->mstatus & ~(1u << 3)) | ((s->mstatus >> 7 & 1) << 3); /* mie=mpie */
            s->mstatus |= 1u << 7;                                 /* mpie = 1 */
            s->npc = s->mepc; break;
        default: goto illegal;
        }
        goto done;
    } else {                                                       /* CSR access */
        uint32_t *csr = RiscvEmulatorGetCSRAddress(s, imm);
        if (s->trap) goto done;            /* unknown CSR already raised illegal */
        uint32_t old = *csr, src = f3 & 4 ? rs1 : a;   /* zimm (rs1 field) vs register */
        if (rd) wr(s, rd, old);
        switch (f3 & 3) {
        case 1: *csr = src; break;                                 /* CSRRW[I]  */
        case 2: if (src) *csr = old | src; break;                  /* CSRRS[I]  */
        case 3: if (src) *csr = old & ~src; break;                 /* CSRRC[I]  */
        default: goto illegal;
        }
        goto done;
    }
illegal:
    s->trap |= T_ILL;
done:
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
    s->fptr = 0;                          /* no fetch window cached yet */
    s->fbase_lo = s->fbase_hi = 0;
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
