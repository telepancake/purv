/*
 * main.c - secure host for purvs: tagged-memory pointer-safety on top of the
 * tag-oblivious purvs engine (purvs.c/.h, an unmodified copy of purv).
 *
 * The engine runs the program normally; in parallel this host carries a shadow
 * TAG for every register and every memory word that the CPU never sees. Tags:
 *   NOTAG   - value the CPU made up (immediates, arithmetic of untagged, sp..)
 *   <id>    - a pointer into object <id> (handed out by the malloc syscall)
 *   BADTAG  - corrupted provenance (e.g. mixing two objects' pointers)
 * Propagation per instruction (dataflow, decoded in parallel here):
 *   tag op NOTAG     -> tag        (pointer +/- integer offset stays in-object)
 *   tag op other-tag -> BADTAG     (cross-object arithmetic)
 *   BADTAG op _      -> BADTAG     (sticky)
 * On every load/store through a tagged pointer the access is checked against the
 * object's [base,size): out-of-bounds, cross-object, use-after-free, or a BADTAG
 * pointer are all caught before the engine touches memory.
 *
 *   usage: purvs <elf> [args...]      (runs as a tagged userspace program)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "purvs.h"

/* ------------------------------------------------------------------ memory */

#define PURV_RAM_DEFAULT (256u * 1024u * 1024u)
static uint8_t *g_ram;
static uint32_t g_ram_size;

#define UART_THR 0x10000000u
#define UART_LSR 0x10000005u

static int      g_halt;
static int      g_exit = 1;
static uint32_t g_brk;                    /* bump heap for malloc / brk */

static int in_ram(uint32_t a, uint32_t n) {
    return a >= RAM_ORIGIN && (uint64_t)a + n <= (uint64_t)RAM_ORIGIN + g_ram_size;
}

/* ------------------------------------------------------------------- tags */

typedef uint32_t tag_t;
#define NOTAG  0u
#define BADTAG 0xFFFFFFFFu

/* A reserved high range of tags marks memory as executable code. Data object
 * tags are small (1..); code tags are CODE_TAG_BASE.. (one per loaded executable
 * segment). A fetch is valid only from a cell carrying a code tag. */
#define CODE_TAG_BASE 0xC0000000u
static int is_code_tag(tag_t t) { return t >= CODE_TAG_BASE && t != BADTAG; }
static tag_t g_code_tag = CODE_TAG_BASE;   /* next code tag to hand out */

static tag_t    g_reg_tag[32];            /* shadow registers (the driver's datapath) */
static tag_t   *g_mem_tag;                /* tagged memory: one tag per word, owned by the
                                           * memory system -- touched ONLY in the load/store
                                           * callbacks, never by the driver loop */

/* Object table: tag <id> (1-based) describes the allocation it points into. */
#define MAX_OBJ 4096
static struct { uint32_t base, size; int live; } g_obj[MAX_OBJ];
static uint32_t g_nobj;

/* ----------------------------------------------------- per-operation tag rules
 *
 * A value's tag is N (plain integer), P (a pointer into a specific object), or
 * B (corrupted provenance, sticky). For each ALU op we reason through every
 * operand combination. P+P below means two *different* objects unless noted.
 *
 *   op          N,N | P,N        | N,P        | P,P same | P,P diff | any B
 *   ----------- ----+------------+------------+----------+----------+------
 *   add          N  | P          | P          | B        | B        | B
 *   sub (a-b)    N  | P          | B          | N        | B        | B
 *   slt/cmp      N  | N          | N          | N        | B        | B
 *   and/or/xor   N  | B          | B          | B        | B        | B
 *   sll/srl/sra  N  | B          | B          | B        | B        | B
 *   mul/div/rem  N  | B          | B          | B        | B        | B
 *
 * Rationale: only adding/subtracting an integer offset keeps a pointer; adding
 * two pointers, subtracting a pointer from an integer, or running a pointer
 * through multiply/shift/bitwise yields something that is not a valid pointer
 * into the object. A same-object difference is a scalar (N); a comparison is a
 * boolean (N) unless it crosses objects (meaningless -> B).
 */
static int is_ptr(tag_t t) { return t != NOTAG && t != BADTAG; }

static tag_t alu_add(tag_t a, tag_t b) {       /* commutative: ptr + offset */
    if (a == BADTAG || b == BADTAG) return BADTAG;
    if (is_ptr(a) && is_ptr(b)) return BADTAG;          /* ptr + ptr is meaningless */
    return is_ptr(a) ? a : b;                            /* ptr+int->ptr ; int+int->N */
}
static tag_t alu_sub(tag_t a, tag_t b) {       /* a - b, NOT commutative */
    if (a == BADTAG || b == BADTAG) return BADTAG;
    if (is_ptr(a) && is_ptr(b)) return (a == b) ? NOTAG : BADTAG; /* same->scalar, diff->bad */
    if (is_ptr(b)) return BADTAG;                        /* int - ptr -> bad */
    return a;                                            /* ptr-int->ptr ; int-int->N */
}
static tag_t alu_cmp(tag_t a, tag_t b) {       /* result is a boolean scalar */
    if (a == BADTAG || b == BADTAG) return BADTAG;
    if (is_ptr(a) && is_ptr(b) && a != b) return BADTAG; /* cross-object compare */
    return NOTAG;
}
static tag_t alu_other(tag_t a, tag_t b) {     /* mul/div/rem/shift */
    return (a == NOTAG && b == NOTAG) ? NOTAG : BADTAG;  /* any provenance -> bad */
}
/* and/or/xor: masking a pointer with a constant. Keep the tag when the constant
 * only touches the low (page-local) bits -- i.e. alignment (and ~k), or setting/
 * toggling low bits (or/xor k) -- since that just adjusts the address within the
 * object and the eventual load/store is still bounds-checked. Touching high bits
 * is forging, and mixing two pointers' bits is meaningless: both -> BAD.
 * f3: 7=and, 6=or, 4=xor. va/vb are the operand values (for the constant). */
#define PAGE_BITS 12
static tag_t alu_bitwise(uint8_t f3, tag_t ta, uint32_t va, tag_t tb, uint32_t vb) {
    if (ta == BADTAG || tb == BADTAG) return BADTAG;
    int pa = is_ptr(ta), pb = is_ptr(tb);
    if (pa && pb) return BADTAG;                /* two addresses mixed */
    if (!pa && !pb) return NOTAG;               /* plain integers */
    uint32_t k = pa ? vb : va;                  /* the non-pointer constant */
    int page_local = (f3 == 7) ? ((~k >> PAGE_BITS) == 0)   /* and: clears only low bits */
                               : (( k >> PAGE_BITS) == 0);   /* or/xor: sets only low bits */
    return page_local ? (pa ? ta : tb) : BADTAG;
}

/* ---- tag side channel: the one handoff between the register shadow (kept by
 * the driver loop) and the memory system (these callbacks). The driver sets the
 * base-pointer tag and (for a store) the value tag before a step; a load reads
 * the cell's tag back out. The engine itself passes no tags -- it stays oblivious
 * and only ever moves bytes. */
static tag_t    g_mem_ptr_tag;    /* tag of the base pointer being dereferenced  */
static tag_t    g_mem_val_tag;    /* store: value tag in; load: cell tag out     */
static uint32_t g_cur_pc;         /* instruction in flight, for the message      */

/* Validate an access made through pointer-tag t against its object. */
static int tag_check(tag_t t, uint32_t addr, uint32_t len, int write) {
    const char *why = 0;
    if (t == NOTAG) return 1;             /* untracked memory (stack/globals/MMIO): allow */
    if (t == BADTAG) {
        why = "bad-provenance pointer";
    } else if (t > g_nobj || !g_obj[t - 1].live) {
        why = "use-after-free / dead object";
    } else {
        uint32_t b = g_obj[t - 1].base, s = g_obj[t - 1].size;
        if (addr < b || (uint64_t)addr + len > (uint64_t)b + s) why = "out of bounds";
        else return 1;                    /* in-bounds: ok */
    }
    fprintf(stderr,
        "\n*** purvs: pointer-safety violation ***\n"
        "  %s %s of %u byte(s) at 0x%08x (pc=0x%08x)\n",
        why, write ? "store" : "load", len, addr, g_cur_pc);
    if (t == BADTAG) fprintf(stderr, "  pointer tag: BAD\n");
    else             fprintf(stderr, "  pointer tag: object %u\n", t);
    if (t != BADTAG && t <= g_nobj)
        fprintf(stderr, "  object %u: [0x%08x, 0x%08x)%s\n", t,
                g_obj[t - 1].base, g_obj[t - 1].base + g_obj[t - 1].size,
                g_obj[t - 1].live ? "" : " (freed)");
    g_halt = 1;
    g_exit = 134;
    return 0;
}

/* ----------------------------------------------- implementation-specific hooks */

/* Instruction fetch: valid only from a cell tagged executable code. This is the
 * same tagged memory data uses; here we require the cell's own tag to be a code
 * tag, so jumping into data, the heap, or the stack faults. */
void RiscvEmulatorFetch(uint32_t address, void *destination, uint8_t length) {
    tag_t t = in_ram(address, 4) ? g_mem_tag[(address - RAM_ORIGIN) >> 2] : NOTAG;
    if (!is_code_tag(t)) {
        fprintf(stderr,
            "\n*** purvs: control-flow violation ***\n"
            "  fetch of non-executable memory at 0x%08x (pc=0x%08x)\n",
            address, g_cur_pc);
        g_halt = 1; g_exit = 134;
        memset(destination, 0, length);
        return;
    }
    if (in_ram(address, length)) memcpy(destination, &g_ram[address - RAM_ORIGIN], length);
    else memset(destination, 0, length);
}

/* Data read: check the base pointer, then return the bytes AND the memory cell's
 * tag (a full-word read recovers a stored pointer's tag; sub-word reads data). */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length) {
    g_mem_val_tag = NOTAG;
    if (!tag_check(g_mem_ptr_tag, address, length, 0)) { memset(destination, 0, length); return; }
    if (address == UART_LSR) { *(uint8_t *)destination = 0x60; return; }
    if (in_ram(address, length)) {
        memcpy(destination, &g_ram[address - RAM_ORIGIN], length);
        if (length == 4) {                /* recover a stored pointer's tag... */
            tag_t cell = g_mem_tag[(address - RAM_ORIGIN) >> 2];
            g_mem_val_tag = is_code_tag(cell) ? NOTAG : cell;  /* ...but code isn't data */
        }
        return;
    }
    memset(destination, 0, length);       /* unmapped reads as zero (NOTAG) */
}

/* Data write: check the base pointer, then store the bytes AND the value's tag
 * into the cell (a full-word store records the pointer; a sub-word store leaves
 * a value that can't be a clean pointer, so the cell's tag is cleared). */
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length) {
    if (!tag_check(g_mem_ptr_tag, address, length, 1)) return;
    if (address == UART_THR) { putchar(*(const uint8_t *)source); fflush(stdout); return; }
    if (in_ram(address, length)) {
        memcpy(&g_ram[address - RAM_ORIGIN], source, length);
        g_mem_tag[(address - RAM_ORIGIN) >> 2] = (length == 4) ? g_mem_val_tag : NOTAG;
        return;
    }
    fprintf(stderr, "purvs: stray store 0x%08x\n", address); g_halt = 1; g_exit = 1;
}
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state) {
    if (g_halt) return;                   /* already faulted (e.g. a bad fetch): don't double-report */
    fprintf(stderr, "purvs: illegal instruction 0x%08x at pc=0x%08x\n",
            RiscvEmulatorGetInstruction(state), RiscvEmulatorGetProgramCounter(state));
    g_halt = 1; g_exit = 1;
}
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state) {
    fprintf(stderr, "purvs: unknown CSR 0x%03x\n", RiscvEmulatorGetCsrNumber(state));
    RiscvEmulatorRaiseIllegalInstruction(state);
}
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *state, uint16_t csrnum) {
    static uint32_t misa = (1u << 30) | (1u << ('I' - 'A')) | (1u << ('M' - 'A'));
    static uint32_t zero;
    (void)state;
    switch (csrnum) {
    case 0x301: return &misa;
    case 0xF11: case 0xF12: case 0xF13: zero = 0; return &zero;
    default: return NULL;
    }
}
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state) { (void)state; }

/* Syscalls: write/exit/brk plus the tag-aware malloc/free that mint and retire
 * object handles. a7=number, a0..=args, result in a0 (and its tag). */
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state) {
    uint32_t num = RiscvEmulatorGetRegister(state, 17);
    uint32_t a0 = RiscvEmulatorGetRegister(state, 10);
    uint32_t a1 = RiscvEmulatorGetRegister(state, 11);
    uint32_t a2 = RiscvEmulatorGetRegister(state, 12);
    uint32_t ret = (uint32_t)-38;         /* -ENOSYS */
    tag_t ret_tag = NOTAG;
    switch (num) {
    case 64:                              /* write(fd, buf, len) */
        ret = 0;
        for (uint32_t i = 0; i < a2; i++) { uint8_t b = 0; RiscvEmulatorLoad(a1 + i, &b, 1); putchar(b); ret++; }
        fflush(stdout);
        break;
    case 93: case 94:                     /* exit */
        g_exit = (int)a0; g_halt = 1; ret = 0;
        break;
    case 214:                             /* brk */
        if (a0 >= g_brk && a0 < RAM_ORIGIN + g_ram_size) g_brk = a0;
        ret = g_brk;
        break;
    case 1000:                            /* malloc(size) -> tagged pointer */
        if (g_nobj < MAX_OBJ && a0 > 0) {
            uint32_t base = (g_brk + 15u) & ~15u;
            uint32_t end = base + a0 + 16u;            /* +guard gap between objects */
            if (end < RAM_ORIGIN + g_ram_size) {
                g_obj[g_nobj].base = base; g_obj[g_nobj].size = a0; g_obj[g_nobj].live = 1;
                ret = base; ret_tag = ++g_nobj;        /* tag = 1-based id */
                g_brk = end;
            } else ret = 0;
        } else ret = 0;
        break;
    case 1001:                            /* free(ptr): retire ptr's object */
        ret = 0;
        { tag_t t = g_reg_tag[10]; if (t && t != BADTAG && t <= g_nobj) g_obj[t - 1].live = 0; }
        break;
    default: break;
    }
    RiscvEmulatorSetRegister(state, 10, ret);
    g_reg_tag[10] = ret_tag;              /* shadow result tag (malloc hands out the id) */
    RiscvEmulatorClearTrap(state);
}

/* ------------------------------------------------------------- ELF32 loading */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
    e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Ehdr32;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_flags, p_align; } Phdr32;
#define PT_LOAD 1
#define EM_RISCV 243

static uint32_t load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "purvs: cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "purvs: read failed\n"); exit(2); }
    fclose(f);
    if (n < (long)sizeof(Ehdr32) || memcmp(buf, "\177ELF", 4) != 0) { fprintf(stderr, "purvs: not an ELF\n"); exit(2); }
    Ehdr32 eh; memcpy(&eh, buf, sizeof eh);
    if (eh.e_ident[4] != 1 || eh.e_machine != EM_RISCV) { fprintf(stderr, "purvs: not RV32 ELF\n"); exit(2); }
    for (int i = 0; i < eh.e_phnum; i++) {
        Phdr32 ph; memcpy(&ph, buf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (!in_ram(ph.p_paddr, ph.p_memsz)) { fprintf(stderr, "purvs: segment outside RAM\n"); exit(2); }
        if (ph.p_filesz) memcpy(&g_ram[ph.p_paddr - RAM_ORIGIN], buf + ph.p_offset, ph.p_filesz);
        uint32_t end = ph.p_paddr + ph.p_memsz;
        if (ph.p_flags & 1) {                          /* PF_X: executable -> tag cells as code */
            for (uint32_t a = ph.p_paddr & ~3u; a < end; a += 4)
                if (in_ram(a, 4)) g_mem_tag[(a - RAM_ORIGIN) >> 2] = g_code_tag;
            g_code_tag++;                               /* next exec segment: distinct code tag */
        }
        if (end > g_brk) g_brk = (end + 0xFFF) & ~0xFFFu;
    }
    free(buf);
    return eh.e_entry;
}

static void gpoke(uint32_t addr, uint32_t v) { memcpy(&g_ram[addr - RAM_ORIGIN], &v, 4); }

static uint32_t setup_stack(int argc, char **argv) {
    uint32_t sp = RAM_ORIGIN + g_ram_size, ptr[64];
    if (argc > 64) argc = 64;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len; memcpy(&g_ram[sp - RAM_ORIGIN], argv[i], len); ptr[i] = sp;
    }
    sp &= ~15u;
    sp -= (1 + (uint32_t)argc + 3) * 4; sp &= ~15u;
    uint32_t p = sp;
    gpoke(p, (uint32_t)argc); p += 4;
    for (int i = 0; i < argc; i++) { gpoke(p, ptr[i]); p += 4; }
    gpoke(p, 0); p += 4; gpoke(p, 0); p += 4; gpoke(p, 0); p += 4; gpoke(p, 0);
    return sp;
}

/* ------------------------------------------------- the parallel tag datapath */

static uint32_t mem_read32(uint32_t addr) {   /* driver peek, to decode the instruction */
    uint32_t v = 0; if (in_ram(addr, 4)) memcpy(&v, &g_ram[addr - RAM_ORIGIN], 4); return v;
}
static void settag(uint8_t rd, tag_t t) { if (rd) g_reg_tag[rd] = t; }

int main(int argc, char **argv) {
    char *gargv[64]; int gargc = 0;
    uint64_t max_insns = 256ull * 1024 * 1024;
    g_ram_size = PURV_RAM_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: %s <elf> [args...]\n", argv[0]); return 0;
        } else if (gargc < 64) gargv[gargc++] = argv[i];
    }
    if (!gargc) { fprintf(stderr, "usage: %s <elf> [args...]\n", argv[0]); return 2; }

    g_ram = calloc(1, g_ram_size);
    g_mem_tag = calloc(g_ram_size / 4, sizeof *g_mem_tag);
    if (!g_ram || !g_mem_tag) { fprintf(stderr, "purvs: OOM\n"); return 2; }

    uint32_t entry = load_elf(gargv[0]);
    RiscvEmulatorState_t *st = RiscvEmulatorCreate(g_ram_size);
    if (!st) { fprintf(stderr, "purvs: cannot create state\n"); return 2; }
    RiscvEmulatorSetRegister(st, 2, setup_stack(gargc, gargv));   /* sp (NOTAG) */
    RiscvEmulatorSetProgramCounter(st, entry);

    for (uint64_t i = 0; i < max_insns && !g_halt; i++) {
        uint32_t pc = RiscvEmulatorGetNextProgramCounter(st);
        uint32_t insn = mem_read32(pc);
        g_cur_pc = pc;
        if ((insn & 3) != 3) {            /* compressed: 32-bit tag datapath only */
            g_mem_ptr_tag = NOTAG;        /* don't check/forge tags for these */
            RiscvEmulatorLoop(st);
            continue;
        }
        uint8_t op = insn & 0x7f, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7;
        uint8_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, f7 = (insn >> 25) & 0x7f;
        tag_t t1 = g_reg_tag[rs1], t2 = g_reg_tag[rs2];
        uint32_t v1 = RiscvEmulatorGetRegister(st, rs1);   /* operand values, for */
        uint32_t v2 = RiscvEmulatorGetRegister(st, rs2);   /* and/or/xor constant checks */
        uint32_t immI = (uint32_t)((int32_t)insn >> 20);   /* I-type sign-extended imm */

        /* Hand the memory system the base-pointer tag (it checks bounds against
         * it) and, for a store, the value's tag (it records it on the cell). A
         * load reads the cell's tag back into g_mem_val_tag. The engine computes
         * the real effective address itself; we never recompute it here. */
        g_mem_ptr_tag = t1;
        g_mem_val_tag = t2;

        RiscvEmulatorLoop(st);
        if (g_halt) break;

        switch (op) {                     /* propagate the shadow tag to rd */
        case 0x33:                                               /* OP (reg, reg) */
            if (f7 == 0x01)              settag(rd, alu_other(t1, t2));  /* M: mul/div/rem */
            else if (f3 == 0)            settag(rd, f7 == 0x20 ? alu_sub(t1, t2)
                                                              : alu_add(t1, t2)); /* sub / add */
            else if (f3 == 2 || f3 == 3) settag(rd, alu_cmp(t1, t2));    /* slt/sltu */
            else if (f3 == 4 || f3 == 6 || f3 == 7)
                                         settag(rd, alu_bitwise(f3, t1, v1, t2, v2)); /* xor/or/and */
            else                         settag(rd, alu_other(t1, t2));  /* sll/srl/sra */
            break;
        case 0x13:                                               /* OP-IMM (reg, imm: imm is N) */
            if (f3 == 0)                 settag(rd, alu_add(t1, NOTAG)); /* addi: ptr+offset */
            else if (f3 == 2 || f3 == 3) settag(rd, alu_cmp(t1, NOTAG)); /* slti/sltiu */
            else if (f3 == 4 || f3 == 6 || f3 == 7)
                                         settag(rd, alu_bitwise(f3, t1, v1, NOTAG, immI)); /* xori/ori/andi */
            else                         settag(rd, alu_other(t1, NOTAG)); /* slli/srli/srai */
            break;
        case 0x03: settag(rd, g_mem_val_tag); break;  /* LOAD: tag the memory cell returned */
        case 0x23: break;                             /* STORE: cell tag set in the callback */
        case 0x37: case 0x17:                                     /* LUI/AUIPC */
        case 0x6f: case 0x67: settag(rd, NOTAG); break;          /* JAL/JALR  */
        case 0x73: if (f3 != 0) settag(rd, NOTAG); break;        /* CSR (not ecall) */
        default: break;                                          /* BRANCH/FENCE: no rd */
        }
        g_reg_tag[0] = NOTAG;
    }

    RiscvEmulatorDestroy(st);
    return g_exit;
}
