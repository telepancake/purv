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

/* Flat RAM backing the architectural RAM_ORIGIN (0x80000000) region. */
#define PURV_RAM_DEFAULT (256u * 1024u * 1024u)
static uint8_t *g_ram;
static uint32_t g_ram_size;

/* Memory-mapped IO the test environments use (mini-rv32ima conventions). */
#define UART_THR  0x10000000u            /* write a byte -> stdout            */
#define UART_LSR  0x10000005u            /* read -> always transmit-ready     */
#define SYSCON    0x11100000u            /* write 0x5555 -> poweroff/PASS     */

/* Run/termination state shared with the hooks. */
static int      g_halt;                  /* set by a hook to stop the loop    */
static int      g_exit = 1;              /* process exit code; 0 == PASS      */
static int      g_call_mode;             /* in --invoke mode, ignore SYSCON   */
static int      g_user_mode;             /* --user: ecall -> Linux-ish syscall */
static uint32_t g_brk;                    /* program break for the brk syscall  */
#ifdef PURV_GDBSTUB
static int      g_gdb_fd = -1;            /* --gdb=FD: serve gdb on this connected fd */
#endif

/* Symbols resolved from the ELF (conformance contract). */
static int      g_have_tohost, g_have_sig;
static uint32_t g_tohost, g_begin_sig, g_end_sig;

static int in_ram(uint32_t addr, uint32_t len) {
    return addr >= RAM_ORIGIN &&
           (uint64_t)addr + len <= (uint64_t)RAM_ORIGIN + g_ram_size;
}

/* ----------------------------------------------- implementation-specific hooks */

void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length) {
    if (address == UART_LSR) {           /* THR empty + TEMT: always ready */
        *(uint8_t *)destination = 0x60;
        return;
    }
    if (in_ram(address, length)) {
        memcpy(destination, &g_ram[address - RAM_ORIGIN], length);
        return;
    }
    memset(destination, 0, length);      /* unmapped reads as zero */
}

void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length) {
    if (address == UART_THR) {
        putchar(*(const uint8_t *)source);
        fflush(stdout);
        return;
    }
    if (!g_call_mode && address == SYSCON) {
        uint32_t v = 0;
        memcpy(&v, source, length < 4 ? length : 4);
        g_exit = (v == 0x5555u) ? 0 : 1; /* mini-rv32ima poweroff value */
        g_halt = 1;
        return;
    }
    if (g_have_tohost && address == g_tohost) {
        /* HTIF tohost: a write ends the run. Self-checking suites encode the
         * result here (tohost==1 -> pass; odd value >1 -> (testnum<<1)|1 fail);
         * signature-dump suites (RISCOF/arch-test) write 1 and rely on the
         * signature, so the same rule reports PASS for them too. */
        uint32_t v = 0;
        memcpy(&v, source, length < 4 ? length : 4);
        g_exit = (v == 1) ? 0 : 1;
        g_halt = 1;
        return;
    }
    if (in_ram(address, length)) {
        memcpy(&g_ram[address - RAM_ORIGIN], source, length);
        return;
    }
    fprintf(stderr, "purv: stray store addr=0x%08x len=%u\n", address, length);
    g_halt = 1;
    g_exit = 1;
}

void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state) {
    /* If the program installed a trap vector, RiscvEmulatorTrap already
     * redirected execution there; nothing for the host to do. Otherwise the
     * program cannot self-handle, so stop. */
    if (RiscvEmulatorGetTrapVectorBase(state) != 0) {
        return;
    }
    fprintf(stderr, "purv: illegal instruction 0x%08x at pc=0x%08x\n",
            RiscvEmulatorGetInstruction(state), RiscvEmulatorGetProgramCounter(state));
    g_halt = 1;
    g_exit = 1;
}

void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state) {
    /* Unimplemented CSR -> raise illegal-instruction, like real hardware. */
    fprintf(stderr, "purv: unknown CSR 0x%03x at pc=0x%08x\n",
            RiscvEmulatorGetCsrNumber(state), RiscvEmulatorGetProgramCounter(state));
    RiscvEmulatorRaiseIllegalInstruction(state);
}

/* Machine-information CSRs are implementation policy, so the host supplies them
 * rather than the engine. misa advertises the configured ISA (RV32IMC); the
 * vendor/arch/impl IDs read as zero ("not implemented", which is legal). */
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *state, uint16_t csrnum) {
    static uint32_t misa = (1u << 30)            /* MXL = 1 -> XLEN 32 */
                         | (1u << ('I' - 'A'))
                         | (1u << ('M' - 'A'))
                         | (1u << ('C' - 'A'));
    static uint32_t zero;
    (void)state;
    switch (csrnum) {
    case 0x301: return &misa;                    /* misa  */
    case 0xF11:                                  /* mvendorid */
    case 0xF12:                                  /* marchid   */
    case 0xF13:                                  /* mimpid    */
        zero = 0; return &zero;                  /* read-only zero */
    default:    return NULL;                     /* genuinely unimplemented */
    }
}

/* User-mode syscall emulation (Linux/RISC-V ABI subset): a7=number,
 * a0..a5=args, result in a0. Only what single-threaded console programs need;
 * everything else returns -ENOSYS. Enabled by --user; otherwise ecall traps
 * normally (machine-mode conformance is unaffected). */
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state) {
    if (!g_user_mode) return;            /* let it vector to the test's handler */
    uint32_t num = RiscvEmulatorGetRegister(state, 17);   /* a7 */
    uint32_t a0  = RiscvEmulatorGetRegister(state, 10);
    uint32_t a1  = RiscvEmulatorGetRegister(state, 11);
    uint32_t a2  = RiscvEmulatorGetRegister(state, 12);
    uint32_t ret;
    switch (num) {
    case 64:                              /* write(fd, buf, len) */
        ret = 0;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t b = 0;
            RiscvEmulatorLoad(a1 + i, &b, 1);
            putchar(b);
            ret++;
        }
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
        if (a0 >= g_brk && a0 < RAM_ORIGIN + g_ram_size) g_brk = a0;
        ret = g_brk;
        break;
    default:
        ret = (uint32_t)-38;              /* -ENOSYS */
        break;
    }
    RiscvEmulatorSetRegister(state, 10, ret);
    RiscvEmulatorClearTrap(state);        /* consume the ecall; don't vector */
}
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state) { (void)state; }

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
        if (!in_ram(ph.p_paddr, ph.p_memsz)) {
            fprintf(stderr, "purv: segment paddr=0x%08x size=0x%x outside RAM\n",
                    ph.p_paddr, ph.p_memsz); exit(2);
        }
        if (ph.p_filesz)
            memcpy(&g_ram[ph.p_paddr - RAM_ORIGIN], g_elf + ph.p_offset, ph.p_filesz);
        uint32_t end = ph.p_paddr + ph.p_memsz;
        if (end > g_brk) g_brk = (end + 0xFFF) & ~0xFFFu;   /* heap starts past the image */
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
            if (in_ram(addr, 1)) byte = g_ram[addr - RAM_ORIGIN];
            fprintf(f, "%02x", byte);
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------------ main */

#define MAGIC_RET 0xdeadbee0u            /* sentinel return address for --invoke */

static void gpoke(uint32_t addr, uint32_t v) {
    memcpy(&g_ram[addr - RAM_ORIGIN], &v, 4);
}

/* Build the initial process stack (RISC-V Linux ABI): sp points at argc,
 * followed by the argv pointers, a NULL, an empty envp, and an AT_NULL auxv.
 * Argument strings are copied just below the top of RAM. Returns the new sp. */
static uint32_t setup_user_stack(int argc, char **argv) {
    uint32_t sp = RAM_ORIGIN + g_ram_size;
    uint32_t ptr[64];
    if (argc > 64) argc = 64;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy(&g_ram[sp - RAM_ORIGIN], argv[i], len);
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
        "  --ram=BYTES                   RAM size (default 256 MiB)\n"
        "  --max-insns=N                 instruction cap (default 256M)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *elf = NULL, *sigfile = NULL, *invoke = NULL;
    uint32_t gran = 4;
    uint64_t max_insns = 256ull * 1024 * 1024;
    uint32_t args[8]; int nargs = 0;
    char *gargv[64]; int gargc = 0;       /* program argv (argv[0]=elf, then extras) */
    g_ram_size = PURV_RAM_DEFAULT;

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
        else if (!strncmp(a, "--ram=", 6)) g_ram_size = (uint32_t)strtoul(a + 6, 0, 0);
        else if (!strncmp(a, "--max-insns=", 12)) max_insns = strtoull(a + 12, 0, 0);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "purv: unknown option %s\n", a); return 2; }
        else if (gargc < 64) gargv[gargc++] = argv[i];   /* elf, then guest argv */
    }
    if (gargc) elf = gargv[0];
    if (!elf) { usage(argv[0]); return 2; }

    g_ram = calloc(1, g_ram_size);
    if (!g_ram) { fprintf(stderr, "purv: cannot allocate %u bytes RAM\n", g_ram_size); return 2; }

    uint32_t entry = load_elf(elf);

    RiscvEmulatorState_t *st = RiscvEmulatorCreate(g_ram_size);
    if (!st) { fprintf(stderr, "purv: cannot allocate emulator state\n"); return 2; }

    uint32_t start = entry;
    if (invoke) {
        g_call_mode = 1;
        if (!sym_lookup(invoke, &start)) {
            fprintf(stderr, "purv: symbol '%s' not found\n", invoke); return 2;
        }
        RiscvEmulatorSetRegister(st, 1, MAGIC_RET);           /* ra: ret -> stop */
        for (int i = 0; i < nargs; i++)
            RiscvEmulatorSetRegister(st, 10 + i, args[i]);    /* a0.. */
    } else if (g_user_mode) {
        RiscvEmulatorSetRegister(st, 2, setup_user_stack(gargc, gargv));  /* sp */
    } else {
        g_have_tohost   = sym_lookup("tohost", &g_tohost);
        g_have_sig      = sym_lookup("begin_signature", &g_begin_sig) &
                          sym_lookup("end_signature", &g_end_sig);
    }

    RiscvEmulatorSetProgramCounter(st, start);

#ifdef PURV_GDBSTUB
    if (g_gdb_fd >= 0) {
        /* Hand control to gdb: it steps/continues the engine over the RSP. The
         * guest's console still goes to stdout; the RSP rides the provided fd. */
        RiscvEmulatorGdbServe(st, g_gdb_fd, &g_halt, &g_exit);
        RiscvEmulatorDestroy(st);
        return g_exit;
    }
#endif

    uint64_t i = 0;
    for (; i < max_insns && !g_halt; i++) {
        if (invoke && RiscvEmulatorGetNextProgramCounter(st) == MAGIC_RET) break;
        RiscvEmulatorLoop(st);
    }

    if (invoke) {
        if (i >= max_insns) { fprintf(stderr, "purv: instruction cap reached\n"); return 2; }
        uint32_t a0 = RiscvEmulatorGetRegister(st, 10);
        printf("%d (0x%08x)\n", (int32_t)a0, a0);
        RiscvEmulatorDestroy(st);
        return 0;
    }

    if (!g_halt) { fprintf(stderr, "purv: instruction cap reached without halt\n"); return 2; }
    if (sigfile) {
        if (!g_have_sig) { fprintf(stderr, "purv: no begin/end_signature symbols\n"); return 2; }
        dump_signature(sigfile, gran);
    }
    RiscvEmulatorDestroy(st);
    return g_exit;
}
