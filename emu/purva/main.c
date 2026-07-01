/*
 * main.c - host/driver for the purva evaluator.
 *
 * Runs a purva IMAGE (image.h) directly: no ELF loading, no decoding, no separate
 * --ops file. The image already carries everything execution needs (the op array,
 * read-only data, initial writable data, minimum bss/stack sizes); main.c just maps
 * those into the engine's regions and drives the run loop, exactly as the
 * purv/purvs drivers do.
 *
 * The image has no symbol table by design (image.h) -- the engine never had one
 * (same as purv). A mode that needs a name (--invoke=SYM, or the conformance
 * tohost/begin_signature/end_signature symbols) resolves it from the ORIGINAL ELF
 * via --symbols=, read only for its symbol table; it is never used for memory or
 * execution.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purva.h"        /* purv.h + transcode.h + RiscvEmulatorSetProgram */
#include "image.h"
#include "../purvhost.h"  /* PurvHost / purvhost_slurp / purvhost_sym (symbols only) */

#define PURV_HEAP_DEFAULT (256u * 1024u * 1024u)
/* Stack now lives just BELOW RISCV_HALF (grows down from it), back to back with the
 * heap above it -- one writable cluster (see purva.c's mem_xlate). */
#define STACK_MEM_BASE(stack_len) ((uint32_t)(RISCV_HALF - (stack_len)))
static uint8_t *g_heap, *g_stack;
static uint32_t g_heap_size;      /* the DYNAMIC (brk-grown) budget alone, not bss/rwdata */
static uint32_t g_heap_region_len; /* the full region[HEAP] length: rwdata + bss + g_heap_size */
static uint32_t g_stack_size;

static int      g_halt;
static int      g_exit = 1;
static int      g_user_mode;
static uint32_t g_brk;

static PurvHost  g_symhost;       /* --symbols=: ELF bytes kept ONLY for name lookup */
static int       g_have_symbols;
static int       g_have_tohost, g_have_sig;
static uint32_t  g_tohost, g_begin_sig, g_end_sig;

static int in_heap(uint32_t addr, uint32_t len) {
    return addr >= RISCV_HALF && (uint64_t)addr + len <= (uint64_t)RISCV_HALF + g_heap_region_len;
}

/* One guest byte, through purva's two-cluster layout (mirrors purva.c's mem_xlate;
 * purvhost_guest_byte still speaks the old four-region half-split that purv/purvs
 * use, so purva reads its own memory here). Writable cluster is stack-then-heap
 * around RISCV_HALF; rodata sits at small NEGATIVE addresses below 0. */
static uint8_t gbyte(const RiscvEmulatorState_t *s, uint32_t a) {
    const RiscvEmulatorRegion_t *stack = &s->region[RISCV_STACK];
    uint32_t wrel = a - (RISCV_HALF - stack->len);
    if (wrel < stack->len + s->region[RISCV_HEAP].len) return stack->ptr[wrel];
    const RiscvEmulatorRegion_t *rodata = &s->region[RISCV_RODATA];
    int32_t ro = (int32_t)a;
    if (ro >= -(int32_t)rodata->len && ro < 0) return rodata->ptr[(uint32_t)(ro + (int32_t)rodata->len)];
    return 0;
}

/* ------------------------------------------------ trap handlers */

static int on_illegal(RiscvEmulatorState_t *state) {
    /* state->inst is OUR packed op word (set by the evaluator), not a RISC-V
     * instruction -- the image carries no raw RISC-V bytes to recover one from. */
    fprintf(stderr, "purva: illegal op 0x%08x at pc=0x%08x\n", state->inst, state->pc);
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
        for (uint32_t i = 0; i < a2; i++) putchar(gbyte(state, a1 + i));
        ret = a2; fflush(stdout); (void)a0;
        break;
    case 93: case 94:
        g_exit = (int)a0; g_halt = 1; ret = 0;
        break;
    case 214:
        if (a0 >= g_brk && a0 < RISCV_HALF + g_heap_region_len) g_brk = a0;
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

/* ------------------------------------------------------------------- symbols */

/* Resolve `name` from --symbols=<elf>, if one was given. 0 if no name found / no
 * --symbols= was passed. Reads only the symbol table -- never touches memory. */
static int sym(const char *name, uint32_t *out) {
    return g_have_symbols && purvhost_sym(&g_symhost, name, out);
}

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

static void gpoke(uint32_t addr, uint32_t v) { memcpy(&g_stack[addr - STACK_MEM_BASE(g_stack_size)], &v, 4); }

static uint32_t setup_user_stack(int argc, char **argv) {
    uint32_t sp = RISCV_HALF, ptr[64] = {0};      /* top of the stack cluster */
    if (argc > 64) argc = 64;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy(&g_stack[sp - STACK_MEM_BASE(g_stack_size)], argv[i], len);
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

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s <prog.img> [options] [program args...]\n"
        "  --symbols=ELF                  resolve --invoke/signature symbols from this ELF\n"
        "  --signature=FILE               dump signature region (RISCOF DUT)\n"
        "  --signature-granularity=N      bytes per signature word (default 4)\n"
        "  --invoke=SYM                   call function SYM, print return (a0)\n"
        "  --arg=N                        integer argument for --invoke (repeatable)\n"
        "  --user                         run as userspace program (ecall -> syscall)\n"
        "  --ram=BYTES                    heap size at 0x80000000 (default 256 MiB)\n"
        "  --max-insns=N                  instruction cap (default 256M)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *imgpath = NULL, *symelf = NULL, *sigfile = NULL, *invoke = NULL;
    uint32_t gran = 4;
    uint64_t max_insns = 256ull * 1024 * 1024;
    uint32_t args[8]; int nargs = 0;
    char *gargv[64]; int gargc = 0;
    g_heap_size = PURV_HEAP_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--symbols=", 10)) symelf = a + 10;
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
    if (gargc) imgpath = gargv[0];
    if (!imgpath) { usage(argv[0]); return 2; }

    Image img;
    if (image_read(imgpath, &img) != 0) { fprintf(stderr, "purva: cannot read %s\n", imgpath); return 2; }

    if (symelf) {
        g_symhost.elf = purvhost_slurp(symelf, &g_symhost.elf_len);
        g_have_symbols = 1;
    }

    /* region[CODE] and region[RODATA] are genuinely separate regions -- purva.c's
     * mem_xlate never resolves a data access into region[CODE] at all (purva's
     * "code" is packed op words, not real RISC-V bytes; there is nothing sane
     * for a data load to read there); img.code (already exactly code_size bytes,
     * from image_read) IS region[CODE], no copy needed. img.rodata is placed at
     * 0 - img.rodata_size, purv.h's formula for read-only data that grows down
     * from 0 (mem_xlate derives it the same way from the length, no separate base
     * needed) -- at small negative addresses, not derived from code_size, so a
     * fusion pass shrinking or growing the op array never moves it. */

    /* The writable cluster is ONE buffer: the stack just below RISCV_HALF (grows
     * down from it), then the heap from RISCV_HALF up -- rwdata, then bss (the
     * compiled code's globals, baked in at the linker's real offset right after
     * rwdata, same as purv), then the dynamic (brk-grown) heap. g_heap_size is the
     * dynamic budget alone (--ram=, default below); bss is additional and must not
     * be folded into it, or brk's start address overlaps live bss variables. */
    g_heap_region_len = img.rwdata_size + img.bss_size + g_heap_size;
    g_stack_size = img.stack_size > (16u * 1024 * 1024) ? img.stack_size : (16u * 1024 * 1024);
    uint8_t *writable = calloc(1, (size_t)g_stack_size + g_heap_region_len);
    g_stack = writable;                               /* [RISCV_HALF - g_stack_size, RISCV_HALF) */
    g_heap  = writable + g_stack_size;                /* [RISCV_HALF, RISCV_HALF + g_heap_region_len) */
    if (img.rwdata_size) memcpy(g_heap, img.rwdata, img.rwdata_size);
    g_brk = RISCV_HALF + img.rwdata_size + img.bss_size;

    RiscvEmulatorState_t state;
    RiscvEmulatorInit(&state,
        (RiscvEmulatorRegion_t){ img.code, img.code_size },
        (RiscvEmulatorRegion_t){ img.rodata, img.rodata_size },
        (RiscvEmulatorRegion_t){ g_heap, g_heap_region_len },
        (RiscvEmulatorRegion_t){ g_stack, g_stack_size });
    RiscvEmulatorState_t *st = &state;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;

    Transcoded prog = { (uint32_t *)img.code, img.code_size / 4, img.code_size };
    RiscvEmulatorSetProgram(&prog);

    uint32_t start = 0;            /* entry is always 0 (image.h) */
    if (invoke) {
        if (!sym(invoke, &start)) {
            fprintf(stderr, "purva: symbol '%s' not found (pass --symbols=ELF?)\n", invoke); return 2;
        }
        st->x[1] = MAGIC_RET;
        for (int i = 0; i < nargs; i++) st->x[10 + i] = args[i];
    } else if (g_user_mode) {
        st->x[2] = setup_user_stack(gargc, gargv);   /* gargv[0] (the image path) is argv[0] */
    } else {
        g_have_tohost = sym("tohost", &g_tohost);
        g_have_sig    = sym("begin_signature", &g_begin_sig) & sym("end_signature", &g_end_sig);
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
