/*
 * purv.c - reference host for the purv single-header emulator (purv.h).
 *
 * This is the "implementation-specific" half of atoom's model: purv.h is the
 * engine, this file is everything the engine reaches out to (memory map, UART,
 * termination) plus a small ELF loader and two ways to drive it:
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
        /* RISCOF halt: any non-zero tohost write ends the run; the signature
         * is what's actually checked, so PASS the process and let the harness
         * diff the signature against the reference model. */
        g_exit = 0;
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
    if (state->csr.mtvec.base != 0) {
        return;
    }
    fprintf(stderr, "purv: illegal instruction 0x%08x at pc=0x%08x\n",
            state->instruction.value, state->programcounter);
    g_halt = 1;
    g_exit = 1;
}

void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state) {
    /* Unimplemented CSR -> raise illegal-instruction, like real hardware. */
    fprintf(stderr, "purv: unknown CSR 0x%03x at pc=0x%08x\n",
            state->instruction.itypecsr.csr, state->programcounter);
    state->trapflag.illegalinstruction = 1;
}

void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state) { (void)state; }
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

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] <elf>\n"
        "  --signature=FILE              dump signature region (RISCOF DUT)\n"
        "  --signature-granularity=N     bytes per signature word (default 4)\n"
        "  --invoke=SYM                  call function SYM, print return (a0)\n"
        "  --arg=N                       integer argument for --invoke (repeatable)\n"
        "  --ram=BYTES                   RAM size (default 256 MiB)\n"
        "  --max-insns=N                 instruction cap (default 256M)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *elf = NULL, *sigfile = NULL, *invoke = NULL;
    uint32_t gran = 4;
    uint64_t max_insns = 256ull * 1024 * 1024;
    uint32_t args[8]; int nargs = 0;
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
        else if (!strncmp(a, "--ram=", 6)) g_ram_size = (uint32_t)strtoul(a + 6, 0, 0);
        else if (!strncmp(a, "--max-insns=", 12)) max_insns = strtoull(a + 12, 0, 0);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "purv: unknown option %s\n", a); return 2; }
        else elf = a;
    }
    if (!elf) { usage(argv[0]); return 2; }

    g_ram = calloc(1, g_ram_size);
    if (!g_ram) { fprintf(stderr, "purv: cannot allocate %u bytes RAM\n", g_ram_size); return 2; }

    uint32_t entry = load_elf(elf);

    RiscvEmulatorState_t st;
    RiscvEmulatorInit(&st, g_ram_size);

    uint32_t start = entry;
    if (invoke) {
        g_call_mode = 1;
        if (!sym_lookup(invoke, &start)) {
            fprintf(stderr, "purv: symbol '%s' not found\n", invoke); return 2;
        }
        st.reg.ra = MAGIC_RET;                  /* x1: where ret lands -> stop */
        for (int i = 0; i < nargs; i++) st.reg.x[10 + i] = args[i];  /* a0.. */
    } else {
        g_have_tohost   = sym_lookup("tohost", &g_tohost);
        g_have_sig      = sym_lookup("begin_signature", &g_begin_sig) &
                          sym_lookup("end_signature", &g_end_sig);
    }

    st.programcounter = start;
    st.programcounternext = start;

    uint64_t i = 0;
    for (; i < max_insns && !g_halt; i++) {
        if (invoke && st.programcounternext == MAGIC_RET) break;  /* returned */
        RiscvEmulatorLoop(&st);
    }

    if (invoke) {
        if (i >= max_insns) { fprintf(stderr, "purv: instruction cap reached\n"); return 2; }
        int32_t r = (int32_t)st.reg.a0;
        printf("%d (0x%08x)\n", r, st.reg.a0);
        return 0;
    }

    if (!g_halt) { fprintf(stderr, "purv: instruction cap reached without halt\n"); return 2; }
    if (sigfile) {
        if (!g_have_sig) { fprintf(stderr, "purv: no begin/end_signature symbols\n"); return 2; }
        dump_signature(sigfile, gran);
    }
    return g_exit;
}
