/*
 * main.c - reference host/driver for the purv emulator engine.
 *
 * The engine is purv.h (interface) + purv.c (implementation). This file is the
 * "implementation-specific" half of atoom's model: everything the engine
 * reaches out to (memory map, UART, termination) plus a small ELF loader and
 * two ways to drive it:
 *
 *   1. Conformance / run-to-halt  (RISCOF DUT contract + ACT4 SYSCON):
 *        purv [--signature=FILE [--signature-granularity=N]] <elf>
 *      Loads the ELF, runs from its entry point until the program signals
 *      completion (store to the `tohost` symbol, or a poweroff write to
 *      SYSCON), then optionally dumps [begin_signature, end_signature) as one
 *      hex word per line. Exit status is 0 on PASS, non-zero on FAIL.
 *
 *   2. Bare-function call, wasm style:
 *        purv --invoke=SYM [--arg=N ...] <elf>
 *      Calls function SYM (or the ELF entry) with the given integer arguments
 *      in a0.. and prints the returned value (a0). Compile a freestanding
 *      function with clang --target=riscv32 -march=rv32imc and run it directly.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "purv.h"
#ifdef PURV_GDBSTUB
#include "gdbstub.h"
#endif

/* ------------------------------------------------------------------ memory */

/* Three host buffers map the engine's four regions:
 *   g_code  -> code   [0, CODE_BYTES)            lower half: text + rodata (RO, fetched)
 *   g_heap  -> heap   [RISCV_HALF, +heap_size)   upper half: data + bss + heap (RW)
 *   g_stack -> stack  [STACK_MEM_BASE, 2^32)     upper half: the stack, grows down
 * (The engine's separate `rodata` region is left unmapped here; a guest's .rodata
 * rides in the code region.) sp starts at 0 (== 2^32, one past the stack top). */
#define CODE_BYTES        (64u * 1024u * 1024u)
#define PURV_HEAP_DEFAULT (256u * 1024u * 1024u)
#define STACK_BYTES       (16u * 1024u * 1024u)
#define STACK_MEM_BASE    ((uint32_t)(0u - STACK_BYTES))   /* host base of the stack buffer */
static uint8_t *g_code;
static uint8_t *g_heap;
static uint32_t g_heap_size;
static uint8_t *g_stack;

/* Run/termination state shared with the handlers. */
static int      g_halt;                  /* set by a handler/poll to stop the loop */
static int      g_exit = 1;              /* process exit code; 0 == PASS      */
static int      g_user_mode;             /* --user: ecall -> Linux-ish syscall */
static uint32_t g_brk;                    /* program break for the brk syscall  */
#ifdef PURV_GDBSTUB
static int      g_gdb_fd = -1;            /* --gdb=FD: serve gdb on this connected fd */
#endif

/* Symbols resolved from the ELF (conformance contract). */
static int      g_have_tohost, g_have_sig;
static uint32_t g_tohost, g_begin_sig, g_end_sig;

/* Is [addr,addr+len) inside the code (lower half) or heap (upper half) buffer? */
static int in_code(uint32_t addr, uint32_t len) {
    return addr < RISCV_HALF && (uint64_t)addr + len <= CODE_BYTES;
}
static int in_heap(uint32_t addr, uint32_t len) {
    return addr >= RISCV_HALF && (uint64_t)addr + len <= (uint64_t)RISCV_HALF + g_heap_size;
}

/* ------------------------------------------------ trap handlers (assigned to the state) */

static int on_illegal(RiscvEmulatorState_t *state) {
    /* A userspace program cannot self-handle an illegal instruction, so report
     * it and stop. */
    fprintf(stderr, "purv: illegal instruction 0x%08x at pc=0x%08x\n",
            state->inst, state->pc);
    g_halt = 1;
    g_exit = 1;
    return 1;
}

/* Out-of-bounds / read-only data access: report the first one and halt (the
 * engine can't stop mid-instruction, so the run loop notices g_halt next slice).
 * Conformance/invoke paths never trip this; it is a userspace segfault. */
static int g_faulted;
static uint32_t on_oob(RiscvEmulatorState_t *state, int op, uint32_t addr, uint32_t value) {
    (void)value;
    if (!g_faulted) {
        fprintf(stderr, "purv: %s fault at addr=0x%08x pc=0x%08x\n",
                op == RISCV_MEM_STORE ? "store" : "load", addr, state->pc);
        g_faulted = 1; g_halt = 1; g_exit = 1;
    }
    return 0;
}

/* User-mode syscall emulation (Linux/RISC-V ABI subset): a7=number, a0..a5=args,
 * result in a0. Only what single-threaded console programs need; everything else
 * returns -ENOSYS. Enabled by --user; without it an ecall is a nop and execution
 * continues (the signature-dump suites halt via the tohost word we poll). */
/* Read a guest byte the way the engine resolves one: the half picks a pair of
 * regions, the lower grows up from the base, the upper grows down from RISCV_HALF. */
static uint8_t guest_byte(const RiscvEmulatorState_t *st, uint32_t a) {
    const RiscvEmulatorRegion_t *r = &st->region[(a >> 31) << 1];
    uint32_t lo = a & (RISCV_HALF - 1), down = RISCV_HALF - r[1].len;
    if (lo < r[0].len)  return r[0].ptr[lo];
    if (lo >= down && r[1].len) return r[1].ptr[lo - down];
    return 0;
}

static int on_ecall(RiscvEmulatorState_t *state) {
    if (!g_user_mode) return 0;          /* no syscall ABI requested: keep running */
    uint32_t num = state->x[17];         /* a7 */
    uint32_t a0  = state->x[10];
    uint32_t a1  = state->x[11];
    uint32_t a2  = state->x[12];
    uint32_t ret;
    switch (num) {
    case 64:                              /* write(fd, buf, len) */
        for (uint32_t i = 0; i < a2; i++)
            putchar(guest_byte(state, a1 + i));
        ret = a2;
        fflush(stdout);
        (void)a0;                         /* fd ignored: 1/2 both -> stdout */
        break;
    case 93:                              /* exit */
    case 94:                              /* exit_group */
        g_exit = (int)a0;
        g_halt = 1;
        ret = 0;
        break;
    case 214:                             /* brk(addr): grow/query the heap */
        if (a0 >= g_brk && a0 < RISCV_HALF + g_heap_size) g_brk = a0;
        ret = g_brk;
        break;
    default:
        ret = (uint32_t)-38;              /* -ENOSYS */
        break;
    }
    state->x[10] = ret;                  /* result in a0 */
    return g_halt;                        /* exit halts; otherwise keep running in place */
}
static int on_ebreak(RiscvEmulatorState_t *state) { (void)state; return 1; }

/* ------------------------------------------------------------- ELF32 loading */

/* Minimal ELF32 little-endian definitions (avoid depending on <elf.h>). */
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
    e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Ehdr32;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_flags, p_align; } Phdr32;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
    sh_link, sh_info, sh_addralign, sh_entsize; } Shdr32;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other;
    uint16_t st_shndx; } Sym32;

#define PT_LOAD     1
#define SHT_SYMTAB  2
#define EM_RISCV    243

static uint8_t *g_elf;          /* whole file in memory */
static long     g_elf_len;

static uint8_t *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "purv: cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "purv: read failed for %s\n", path); exit(2);
    }
    fclose(f);
    *len = n;
    return buf;
}

/* Load PT_LOAD segments into RAM and return the entry point. */
static uint32_t load_elf(const char *path) {
    g_elf = slurp(path, &g_elf_len);
    if (g_elf_len < (long)sizeof(Ehdr32) || memcmp(g_elf, "\177ELF", 4) != 0) {
        fprintf(stderr, "purv: %s is not an ELF\n", path); exit(2);
    }
    Ehdr32 eh;
    memcpy(&eh, g_elf, sizeof eh);
    if (eh.e_ident[4] != 1 /* ELFCLASS32 */ || eh.e_machine != EM_RISCV) {
        fprintf(stderr, "purv: %s is not a 32-bit RISC-V ELF\n", path); exit(2);
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        Phdr32 ph;
        memcpy(&ph, g_elf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        /* lower-half segments (text/rodata) load into the code buffer at their
         * vaddr; upper-half segments (data/bss) into the heap buffer. */
        uint8_t *dst; uint32_t base;
        if (ph.p_vaddr < RISCV_HALF) {
            if (!in_code(ph.p_vaddr, ph.p_memsz)) {
                fprintf(stderr, "purv: code segment vaddr=0x%08x size=0x%x too big\n",
                        ph.p_vaddr, ph.p_memsz); exit(2);
            }
            dst = g_code; base = 0;
        } else {
            if (!in_heap(ph.p_vaddr, ph.p_memsz)) {
                fprintf(stderr, "purv: data segment vaddr=0x%08x size=0x%x outside heap\n",
                        ph.p_vaddr, ph.p_memsz); exit(2);
            }
            dst = g_heap; base = RISCV_HALF;
            uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (end > g_brk) g_brk = (end + 0xFFF) & ~0xFFFu;   /* heap starts past the data */
        }
        if (ph.p_filesz)
            memcpy(&dst[ph.p_vaddr - base], g_elf + ph.p_offset, ph.p_filesz);
    }
    return eh.e_entry;
}

/* Resolve a symbol by name; returns 1 and sets *out on success. */
static int sym_lookup(const char *want, uint32_t *out) {
    Ehdr32 eh;
    memcpy(&eh, g_elf, sizeof eh);
    for (int i = 0; i < eh.e_shnum; i++) {
        Shdr32 sh;
        memcpy(&sh, g_elf + eh.e_shoff + (long)i * eh.e_shentsize, sizeof sh);
        if (sh.sh_type != SHT_SYMTAB || sh.sh_entsize == 0) continue;
        Shdr32 strsh;
        memcpy(&strsh, g_elf + eh.e_shoff + (long)sh.sh_link * eh.e_shentsize, sizeof strsh);
        const char *strs = (const char *)(g_elf + strsh.sh_offset);
        uint32_t n = sh.sh_size / sh.sh_entsize;
        for (uint32_t s = 0; s < n; s++) {
            Sym32 sym;
            memcpy(&sym, g_elf + sh.sh_offset + (long)s * sh.sh_entsize, sizeof sym);
            if (sym.st_name && strcmp(strs + sym.st_name, want) == 0) {
                *out = sym.st_value;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- signature */

static void dump_signature(const char *path, uint32_t gran) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "purv: cannot write %s\n", path); g_exit = 1; return; }
    if (gran == 0) gran = 4;
    for (uint32_t a = g_begin_sig; a < g_end_sig; a += gran) {
        /* RISCOF format: most-significant byte first within the word, lowercase
         * hex, no 0x prefix, one word per line. */
        for (uint32_t b = gran; b-- > 0; ) {
            uint8_t byte = 0;
            uint32_t addr = a + b;
            if (in_heap(addr, 1)) byte = g_heap[addr - RISCV_HALF];
            fprintf(f, "%02x", byte);
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------------ main */

#define MAGIC_RET 0xdeadbee0u            /* sentinel return address for --invoke */

static void gpoke(uint32_t addr, uint32_t v) {
    memcpy(&g_stack[addr - STACK_MEM_BASE], &v, 4);
}

/* Build the initial process stack (RISC-V Linux ABI): sp points at argc,
 * followed by the argv pointers, a NULL, an empty envp, and an AT_NULL auxv.
 * Built at the top of the stack region (sp starts at 0 == 2^32); argument
 * strings sit just below the top. Returns the new sp. */
static uint32_t setup_user_stack(int argc, char **argv) {
    uint32_t sp = 0;                                 /* 2^32: one past the last address */
    uint32_t ptr[64];
    if (argc > 64) argc = 64;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy(&g_stack[sp - STACK_MEM_BASE], argv[i], len);
        ptr[i] = sp;
    }
    sp &= ~15u;
    uint32_t words = 1 + (uint32_t)argc + 1 + 1 + 2;   /* argc, argv, NULL, envp NULL, auxv 0,0 */
    sp -= words * 4;
    sp &= ~15u;
    uint32_t p = sp;
    gpoke(p, (uint32_t)argc); p += 4;
    for (int i = 0; i < argc; i++) { gpoke(p, ptr[i]); p += 4; }
    gpoke(p, 0); p += 4;                                /* argv terminator */
    gpoke(p, 0); p += 4;                                /* envp terminator */
    gpoke(p, 0); p += 4;                                /* auxv: AT_NULL type */
    gpoke(p, 0);                                        /* auxv: value       */
    return sp;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] <elf> [program args...]\n"
        "  --signature=FILE              dump signature region (RISCOF DUT)\n"
        "  --signature-granularity=N     bytes per signature word (default 4)\n"
        "  --invoke=SYM                  call function SYM, print return (a0)\n"
        "  --arg=N                       integer argument for --invoke (repeatable)\n"
        "  --user                        run as userspace program (ecall -> syscall;\n"
        "                                program args become argv on the stack)\n"
#ifdef PURV_GDBSTUB
        "  --gdb=FD                      serve gdb on connected fd FD (no listening;\n"
        "                                a launcher supplies the socket)\n"
#endif
        "  --ram=BYTES                   heap size at 0x80000000 (default 256 MiB)\n"
        "  --max-insns=N                 instruction cap (default 256M)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *elf = NULL, *sigfile = NULL, *invoke = NULL;
    uint32_t gran = 4;
    uint64_t max_insns = 256ull * 1024 * 1024;
    uint32_t args[8]; int nargs = 0;
    char *gargv[64]; int gargc = 0;       /* program argv (argv[0]=elf, then extras) */
    g_heap_size = PURV_HEAP_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--signature-granularity=", 24)) gran = (uint32_t)strtoul(a + 24, 0, 0);
        else if (!strncmp(a, "--signature=", 12)) sigfile = a + 12;
        else if (!strncmp(a, "--invoke=", 9)) invoke = a + 9;
        else if (!strncmp(a, "--arg=", 6)) {
            if (nargs < 8) args[nargs++] = (uint32_t)strtol(a + 6, 0, 0);
            else { fprintf(stderr, "purv: too many --arg (max 8)\n"); return 2; }
        }
        else if (!strcmp(a, "--user")) g_user_mode = 1;
#ifdef PURV_GDBSTUB
        else if (!strncmp(a, "--gdb=", 6)) g_gdb_fd = (int)strtol(a + 6, 0, 0);
#endif
        else if (!strncmp(a, "--ram=", 6)) g_heap_size = (uint32_t)strtoul(a + 6, 0, 0);
        else if (!strncmp(a, "--max-insns=", 12)) max_insns = strtoull(a + 12, 0, 0);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "purv: unknown option %s\n", a); return 2; }
        else if (gargc < 64) gargv[gargc++] = argv[i];   /* elf, then guest argv */
    }
    if (gargc) elf = gargv[0];
    if (!elf) { usage(argv[0]); return 2; }

    g_code  = calloc(1, CODE_BYTES);
    g_heap  = calloc(1, g_heap_size);
    g_stack = calloc(1, STACK_BYTES);
    if (!g_code || !g_heap || !g_stack) { fprintf(stderr, "purv: cannot allocate RAM\n"); return 2; }

    uint32_t entry = load_elf(elf);

    /* Map the host buffers into the engine's regions: code (text+rodata) at 0,
     * heap (data+bss+heap) at RISCV_HALF, stack at the top. rodata is unmapped
     * (the guest's .rodata rides in the code region). Init wires the regions, sp,
     * pc, and the default callback; we install the host handlers. */
    RiscvEmulatorRegion_t code   = { g_code,  CODE_BYTES };
    RiscvEmulatorRegion_t rodata = { 0, 0 };
    RiscvEmulatorRegion_t heap   = { g_heap,  g_heap_size };
    RiscvEmulatorRegion_t stack  = { g_stack, STACK_BYTES };
    RiscvEmulatorState_t state;
    RiscvEmulatorInit(&state, code, rodata, heap, stack);
    RiscvEmulatorState_t *st = &state;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;

    uint32_t start = entry;
    if (invoke) {
        if (!sym_lookup(invoke, &start)) {
            fprintf(stderr, "purv: symbol '%s' not found\n", invoke); return 2;
        }
        st->x[1] = MAGIC_RET;                                 /* ra: ret -> stop */
        for (int i = 0; i < nargs; i++)
            st->x[10 + i] = args[i];                          /* a0.. */
    } else if (g_user_mode) {
        st->x[2] = setup_user_stack(gargc, gargv);            /* sp */
    } else {
        g_have_tohost   = sym_lookup("tohost", &g_tohost);
        g_have_sig      = sym_lookup("begin_signature", &g_begin_sig) &
                          sym_lookup("end_signature", &g_end_sig);
    }

    st->pc = start;

#ifdef PURV_GDBSTUB
    if (g_gdb_fd >= 0) {
        /* Hand control to gdb: it steps/continues the engine over the RSP. The
         * guest's console still goes to stdout; the RSP rides the provided fd. */
        RiscvEmulatorGdbServe(st, g_gdb_fd, &g_halt, &g_exit);
        return g_exit;
    }
#endif

    /* Run the engine in slices. Loop stops on an ecall/ebreak/illegal handler
     * (which set g_halt), but a conformance test ends by *storing* to the tohost
     * word -- invisible to the engine -- so we poll that word between slices. The
     * slice is large, so the per-instruction call is gone; at worst a test spins
     * one slice past its tohost store before we notice. A pc that leaves the
     * mapped code ends a slice early (ran < budget): the invoke sentinel, or a
     * runaway jump. */
    const uint64_t SLICE = 1u << 16;
    uint64_t i = 0;
    while (i < max_insns && !g_halt) {
        uint64_t budget = max_insns - i;
        if (budget > SLICE) budget = SLICE;
        uint64_t ran = RiscvEmulatorLoop(st, budget);
        i += ran;
        if (g_halt) break;
        if (g_have_tohost) {                      /* HTIF tohost: a nonzero write ends the run */
            uint32_t v = 0;
            if (in_heap(g_tohost, 4)) memcpy(&v, &g_heap[g_tohost - RISCV_HALF], 4);
            if (v) { g_exit = (v == 1) ? 0 : 1; g_halt = 1; break; }  /* 1 -> PASS */
        }
        if (ran < budget) {                      /* pc left the mapped code */
            uint32_t pc = st->pc;
            if (invoke && pc == MAGIC_RET) break;   /* invoked function returned */
            fprintf(stderr, "purv: fetch outside code at pc=0x%08x\n", pc);
            g_halt = 1; g_exit = 1;
            break;
        }
    }

    if (invoke) {
        if (i >= max_insns) { fprintf(stderr, "purv: instruction cap reached\n"); return 2; }
        uint32_t a0 = st->x[10];
        printf("%d (0x%08x)\n", (int32_t)a0, a0);
        return 0;
    }

    if (!g_halt) { fprintf(stderr, "purv: instruction cap reached without halt\n"); return 2; }
    if (sigfile) {
        if (!g_have_sig) { fprintf(stderr, "purv: no begin/end_signature symbols\n"); return 2; }
        dump_signature(sigfile, gran);
    }
    return g_exit;
}
