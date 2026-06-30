/*
 * main.c - host/driver for the purva evaluator.
 *
 * The upper layer, identical in shape to the purv/purvs driver: it loads the ELF
 * (memory image, symbols, entry) and services syscalls. The only difference is that
 * execution runs over a pre-transcoded op array supplied with --ops (produced by the
 * standalone `transcode` tool) instead of decoding instructions. The evaluator never
 * sees the ELF; the host never decodes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purva.h"        /* purv.h + transcode.h + RiscvEmulatorSetProgram */
#include "../purvhost.h"

#define CODE_BYTES        (64u * 1024u * 1024u)
#define PURV_HEAP_DEFAULT (256u * 1024u * 1024u)
#define STACK_BYTES       (16u * 1024u * 1024u)
#define STACK_MEM_BASE    ((uint32_t)(0u - STACK_BYTES))
static uint8_t *g_code, *g_heap, *g_stack;
static uint32_t g_heap_size;

static int      g_halt;
static int      g_exit = 1;
static int      g_user_mode;
static uint32_t g_brk;

static int      g_have_tohost, g_have_sig;
static uint32_t g_tohost, g_begin_sig, g_end_sig;

static int in_heap(uint32_t addr, uint32_t len) {
    return addr >= RISCV_HALF && (uint64_t)addr + len <= (uint64_t)RISCV_HALF + g_heap_size;
}

/* Highest lower-half (code+rodata) byte the image occupies: the data window for
 * loads. It differs from the op array's extent now that auipc spills a word, so the
 * host sets it from the ELF rather than from the op count. */
static uint32_t lower_half_extent(const PurvHost *h) {
    PurvElf32_Ehdr eh; memcpy(&eh, h->elf, sizeof eh);
    uint32_t end = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        PurvElf32_Phdr ph;
        memcpy(&ph, h->elf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PURV_PT_LOAD || ph.p_memsz == 0) continue;
        if (ph.p_vaddr < RISCV_HALF) { uint32_t e = ph.p_vaddr + ph.p_memsz; if (e > end) end = e; }
    }
    return (end + 3u) & ~3u;
}

/* ------------------------------------------------ trap handlers */

static int on_illegal(RiscvEmulatorState_t *state) {
    /* The evaluator does not carry raw instruction words; recover this one from the
     * code region (the host has the bytes) just for the diagnostic. */
    uint32_t pc = state->pc, raw = 0;
    if (pc + 4 <= state->region[RISCV_CODE].len) {
        const uint8_t *c = state->region[RISCV_CODE].ptr + pc;
        raw = (uint32_t)c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    }
    fprintf(stderr, "purva: illegal instruction 0x%08x at pc=0x%08x\n", raw, pc);
    g_halt = 1; g_exit = 1;
    return 1;
}

static int g_faulted;
static uint32_t on_oob(RiscvEmulatorState_t *state, int op, uint32_t addr, uint32_t value) {
    (void)value;
    if (!g_faulted) {
        fprintf(stderr, "purva: %s fault at addr=0x%08x pc=0x%08x\n",
                op == RISCV_MEM_STORE ? "store" : "load", addr, state->pc);
        g_faulted = 1; g_halt = 1; g_exit = 1;
    }
    return 0;
}

static int on_ecall(RiscvEmulatorState_t *state) {
    if (!g_user_mode) return 0;
    uint32_t num = state->x[17], a0 = state->x[10], a1 = state->x[11], a2 = state->x[12], ret;
    switch (num) {
    case 64:
        for (uint32_t i = 0; i < a2; i++) putchar(purvhost_guest_byte(state, a1 + i));
        ret = a2; fflush(stdout); (void)a0;
        break;
    case 93: case 94:
        g_exit = (int)a0; g_halt = 1; ret = 0;
        break;
    case 214:
        if (a0 >= g_brk && a0 < RISCV_HALF + g_heap_size) g_brk = a0;
        ret = g_brk;
        break;
    default:
        ret = (uint32_t)-38;
        break;
    }
    state->x[10] = ret;
    return g_halt;
}
static int on_ebreak(RiscvEmulatorState_t *state) { (void)state; return 1; }

/* ------------------------------------------------------------------- signature */

static void dump_signature(const char *path, uint32_t gran) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "purva: cannot write %s\n", path); g_exit = 1; return; }
    if (gran == 0) gran = 4;
    for (uint32_t a = g_begin_sig; a < g_end_sig; a += gran) {
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

#define MAGIC_RET 0xdeadbee0u

static void gpoke(uint32_t addr, uint32_t v) { memcpy(&g_stack[addr - STACK_MEM_BASE], &v, 4); }

static uint32_t setup_user_stack(int argc, char **argv) {
    uint32_t sp = 0, ptr[64] = {0};
    if (argc > 64) argc = 64;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy(&g_stack[sp - STACK_MEM_BASE], argv[i], len);
        ptr[i] = sp;
    }
    sp &= ~15u;
    uint32_t words = 1 + (uint32_t)argc + 1 + 1 + 2;
    sp -= words * 4;
    sp &= ~15u;
    uint32_t p = sp;
    gpoke(p, (uint32_t)argc); p += 4;
    for (int i = 0; i < argc; i++) { gpoke(p, ptr[i]); p += 4; }
    gpoke(p, 0); p += 4; gpoke(p, 0); p += 4; gpoke(p, 0); p += 4; gpoke(p, 0);
    return sp;
}

/* Read a .ops file: n_ops = size/4. Appends an ILLEGAL sentinel so a run that falls
 * off the end traps instead of reading past the array. */
static uint32_t *load_ops(const char *path, uint32_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "purva: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t n = (uint32_t)(sz / 4);
    uint32_t *ops = malloc(((size_t)n + 1) * 4);
    if (fread(ops, 4, n, f) != n) { fprintf(stderr, "purva: short read on %s\n", path); exit(2); }
    fclose(f);
    ops[n] = (uint32_t)RISCV_OP_ILLEGAL << 26;            /* off-the-end sentinel */
    *n_out = n;
    return ops;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --ops=FILE [options] <elf> [program args...]\n"
        "  --ops=FILE                    transcoded op array (from the transcode tool)\n"
        "  --signature=FILE              dump signature region (RISCOF DUT)\n"
        "  --signature-granularity=N     bytes per signature word (default 4)\n"
        "  --invoke=SYM                  call function SYM, print return (a0)\n"
        "  --arg=N                       integer argument for --invoke (repeatable)\n"
        "  --user                        run as userspace program (ecall -> syscall)\n"
        "  --ram=BYTES                   heap size at 0x80000000 (default 256 MiB)\n"
        "  --max-insns=N                 instruction cap (default 256M)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *elf = NULL, *sigfile = NULL, *invoke = NULL, *opsfile = NULL;
    uint32_t gran = 4;
    uint64_t max_insns = 256ull * 1024 * 1024;
    uint32_t args[8]; int nargs = 0;
    char *gargv[64]; int gargc = 0;
    g_heap_size = PURV_HEAP_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--ops=", 6)) opsfile = a + 6;
        else if (!strncmp(a, "--signature-granularity=", 24)) gran = (uint32_t)strtoul(a + 24, 0, 0);
        else if (!strncmp(a, "--signature=", 12)) sigfile = a + 12;
        else if (!strncmp(a, "--invoke=", 9)) invoke = a + 9;
        else if (!strncmp(a, "--arg=", 6)) {
            if (nargs < 8) args[nargs++] = (uint32_t)strtol(a + 6, 0, 0);
            else { fprintf(stderr, "purva: too many --arg (max 8)\n"); return 2; }
        }
        else if (!strcmp(a, "--user")) g_user_mode = 1;
        else if (!strncmp(a, "--ram=", 6)) g_heap_size = (uint32_t)strtoul(a + 6, 0, 0);
        else if (!strncmp(a, "--max-insns=", 12)) max_insns = strtoull(a + 12, 0, 0);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "purva: unknown option %s\n", a); return 2; }
        else if (gargc < 64) gargv[gargc++] = argv[i];
    }
    if (gargc) elf = gargv[0];
    if (!elf || !opsfile) { usage(argv[0]); return 2; }

    PurvHost host;
    if (purvhost_alloc(&host, CODE_BYTES, g_heap_size, STACK_BYTES) != 0) return 2;
    g_code = host.code; g_heap = host.heap; g_stack = host.stack;
    purvhost_load_elf(&host, elf);
    g_brk = (host.data_end + 0xFFFu) & ~0xFFFu;

    /* Install the transcoded program; its extent is the code window. */
    uint32_t n_ops;
    uint32_t *ops = load_ops(opsfile, &n_ops);
    Transcoded prog = { ops, n_ops, n_ops * 4 };

    RiscvEmulatorState_t state;
    purvhost_init(&host, &state);
    RiscvEmulatorState_t *st = &state;
    st->region[RISCV_CODE].len = lower_half_extent(&host);  /* data window (original bytes) */
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;
    RiscvEmulatorSetProgram(&prog);

    uint32_t start = host.entry;
    if (invoke) {
        if (!purvhost_sym(&host, invoke, &start)) {
            fprintf(stderr, "purva: symbol '%s' not found\n", invoke); return 2;
        }
        st->x[1] = MAGIC_RET;
        for (int i = 0; i < nargs; i++) st->x[10 + i] = args[i];
    } else if (g_user_mode) {
        st->x[2] = setup_user_stack(gargc, gargv);
    } else {
        g_have_tohost = purvhost_sym(&host, "tohost", &g_tohost);
        g_have_sig    = purvhost_sym(&host, "begin_signature", &g_begin_sig) &
                        purvhost_sym(&host, "end_signature", &g_end_sig);
    }

    st->pc = start;

    const uint64_t SLICE = 1u << 16;
    uint64_t i = 0;
    while (i < max_insns && !g_halt) {
        uint64_t budget = max_insns - i;
        if (budget > SLICE) budget = SLICE;
        uint64_t ran = RiscvEmulatorLoop(st, budget);
        i += ran;
        if (g_halt) break;
        if (g_have_tohost) {
            uint32_t v = 0;
            if (in_heap(g_tohost, 4)) memcpy(&v, &g_heap[g_tohost - RISCV_HALF], 4);
            if (v) { g_exit = (v == 1) ? 0 : 1; g_halt = 1; break; }
        }
        if (ran < budget) {
            uint32_t pc = st->pc;
            if (invoke && pc == MAGIC_RET) break;
            fprintf(stderr, "purva: fetch outside code at pc=0x%08x\n", pc);
            g_halt = 1; g_exit = 1;
            break;
        }
    }

    if (invoke) {
        if (i >= max_insns) { fprintf(stderr, "purva: instruction cap reached\n"); return 2; }
        uint32_t a0 = st->x[10];
        printf("%d (0x%08x)\n", (int32_t)a0, a0);
        return 0;
    }

    if (!g_halt) { fprintf(stderr, "purva: instruction cap reached without halt\n"); return 2; }
    if (sigfile) {
        if (!g_have_sig) { fprintf(stderr, "purva: no begin/end_signature symbols\n"); return 2; }
        dump_signature(sigfile, gran);
    }
    return g_exit;
}
