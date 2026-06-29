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

static tag_t   *g_mem_tag;                /* tagged memory: one tag per word; the memory
                                           * system's, touched only in the hooks below */

/* Object table: tag <id> (1-based) describes the allocation it points into. The
 * per-operation tag rules now live in the emulator (purvs.c); main.c is only the
 * memory system + policy. */
#define MAX_OBJ 4096
static struct { uint32_t base, size; int live; } g_obj[MAX_OBJ];
static uint32_t g_nobj;

static uint32_t g_cur_pc;         /* instruction in flight, for messages */

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

/* ===================== the memory system (the engine's hooks) =====================
 * The emulator carries the register tags and propagates them; at a memory access
 * it calls these with the tags as explicit arguments. All *memory* policy lives
 * here: bounds/provenance on data, executability on fetch, W^X on store. */

/* Fetch: read the instruction bytes and return the cell's tag (its code tag, so
 * the engine knows which code object it is executing in). Where execution may go
 * is policed at the control-transfer hook below, not here. */
uint32_t RiscvEmulatorFetch(uint32_t addr, void *dst, uint8_t len) {
    tag_t cell = in_ram(addr, 4) ? g_mem_tag[(addr - RAM_ORIGIN) >> 2] : NOTAG;
    if (in_ram(addr, len)) memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
    else memset(dst, 0, len);
    return cell;
}

/* Every taken jump/call/branch/return arrives here as a pair of tagged
 * addresses. An indirect target is only allowed through a code pointer: NOTAG (a
 * return address or PC-relative target) or a code tag (a function pointer). A
 * data object tag or BADTAG means the program is about to execute data -- reject
 * it. (Plain PC-relative flow within a code object carries the code tag and is
 * always fine.) */
void RiscvEmulatorControlTransfer(uint32_t from, uint32_t from_tag,
                                  uint32_t to, uint32_t to_tag) {
    (void)from_tag;
    if (to_tag == NOTAG || is_code_tag(to_tag)) return;
    fprintf(stderr,
        "\n*** purvs: control-flow violation ***\n"
        "  indirect transfer to 0x%08x through a non-code pointer (pc=0x%08x)\n", to, from);
    fprintf(stderr, to_tag == BADTAG ? "  target tag: BAD\n"
                                     : "  target tag: object %u\n", to_tag);
    g_halt = 1; g_exit = 134;
}

/* Data read through a pointer tagged addr_tag: check it, return the bytes and
 * the loaded value's tag (a full-word read recovers a stored pointer's tag;
 * sub-word reads, and code read as data, are NOTAG). */
uint32_t RiscvEmulatorLoad(uint32_t addr, uint32_t addr_tag, void *dst, uint8_t len) {
    if (!tag_check(addr_tag, addr, len, 0)) { memset(dst, 0, len); return NOTAG; }
    if (addr == UART_LSR) { *(uint8_t *)dst = 0x60; return NOTAG; }
    if (in_ram(addr, len)) {
        memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
        if (len == 4) {
            tag_t cell = g_mem_tag[(addr - RAM_ORIGIN) >> 2];
            return is_code_tag(cell) ? NOTAG : cell;
        }
        return NOTAG;
    }
    memset(dst, 0, len);                   /* unmapped reads as zero */
    return NOTAG;
}

/* Data write through a pointer tagged addr_tag of a value tagged val_tag: check
 * the pointer, enforce W^X, store the bytes, record the value's tag on the cell
 * (a sub-word store leaves no clean pointer, so the cell's tag is cleared). */
void RiscvEmulatorStore(uint32_t addr, uint32_t addr_tag, const void *src, uint32_t val_tag, uint8_t len) {
    if (!tag_check(addr_tag, addr, len, 1)) return;
    if (addr == UART_THR) { putchar(*(const uint8_t *)src); fflush(stdout); return; }
    uint32_t lo = addr, hi = addr + len - 1;
    if ((in_ram(lo, 1) && is_code_tag(g_mem_tag[(lo - RAM_ORIGIN) >> 2])) ||
        (in_ram(hi, 1) && is_code_tag(g_mem_tag[(hi - RAM_ORIGIN) >> 2]))) {
        fprintf(stderr,
            "\n*** purvs: W^X violation ***\n"
            "  store of %u byte(s) into executable memory at 0x%08x (pc=0x%08x)\n",
            len, addr, g_cur_pc);
        g_halt = 1; g_exit = 134;
        return;
    }
    if (in_ram(addr, len)) {
        memcpy(&g_ram[addr - RAM_ORIGIN], src, len);
        g_mem_tag[(addr - RAM_ORIGIN) >> 2] = (len == 4) ? val_tag : NOTAG;
        return;
    }
    fprintf(stderr, "purvs: stray store 0x%08x\n", addr); g_halt = 1; g_exit = 1;
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
    uint32_t a1_tag = RiscvEmulatorGetRegisterTag(state, 11);   /* buffer pointer's tag */
    uint32_t ret = (uint32_t)-38;         /* -ENOSYS */
    tag_t ret_tag = NOTAG;
    switch (num) {
    case 64:                              /* write(fd, buf, len) */
        ret = 0;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t b = 0;
            RiscvEmulatorLoad(a1 + i, a1_tag, &b, 1);   /* checked through the buffer's tag */
            putchar(b); ret++;
        }
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
    case 1001:                            /* free(ptr): retire the object ptr's tag names */
        ret = 0;
        { uint32_t t = RiscvEmulatorGetRegisterTag(state, 10);
          if (t && t != BADTAG && t <= g_nobj) g_obj[t - 1].live = 0; }
        break;
    default: break;
    }
    RiscvEmulatorSetRegister(state, 10, ret);
    RiscvEmulatorSetRegisterTag(state, 10, ret_tag);   /* malloc hands out the object's tag */
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
    RiscvEmulatorState_t *st = RiscvEmulatorCreate(RAM_ORIGIN + g_ram_size);  /* sp = top of RAM */
    if (!st) { fprintf(stderr, "purvs: cannot create state\n"); return 2; }
    RiscvEmulatorSetRegister(st, 2, setup_stack(gargc, gargv));   /* sp (NOTAG) */
    RiscvEmulatorSetProgramCounter(st, entry);

    /* The emulator does all the work -- executing instructions and propagating
     * register tags. The host just steps it and records the pc for messages. */
    uint32_t pc = entry;
    for (uint64_t i = 0; i < max_insns && !g_halt; i++) {
        g_cur_pc = pc;
        pc = RiscvEmulatorLoop(st, pc);
    }

    RiscvEmulatorDestroy(st);
    return g_exit;
}
