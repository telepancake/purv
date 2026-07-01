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
#include "purvhost.h"
#ifdef PURV_GDBSTUB
#include "gdbstub.h"
#endif

/* ------------------------------------------------------------------ memory */

/* Host buffers map the engine's two regions:
 *   g_code  -> readonly [0, CODE_BYTES)              text + rodata (RO, also fetched)
 *   g_stack -> writable [STACK_MEM_BASE, RISCV_HALF) the stack, grows down from RISCV_HALF
 *   g_heap  -> writable [RISCV_HALF, +heap_size)     data + bss + heap, one buffer with the stack
 * (g_stack and g_heap are two views into purvhost's single writable buffer.) sp starts
 * at RISCV_HALF. */
#define CODE_BYTES        (64u * 1024u * 1024u)
#define PURV_HEAP_DEFAULT (256u * 1024u * 1024u)
#define STACK_BYTES       (16u * 1024u * 1024u)
#define STACK_MEM_BASE    (RISCV_HALF - STACK_BYTES)       /* guest base of the stack buffer */
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

/* Is [addr,addr+len) inside the heap (upper half) buffer? (Image placement and the
 * region/state wiring live in purvhost.h now.) */
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
            putchar(purvhost_guest_byte(state, a1 + i));
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
 * Built at the top of the stack region (sp starts at RISCV_HALF); argument
 * strings sit just below the top. Returns the new sp. */
static uint32_t setup_user_stack(int argc, char **argv) {
    uint32_t sp = RISCV_HALF;                         /* top of the stack, grows down */
    uint32_t ptr[64] = {0};
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

    /* purvhost does the recurring setup: allocate the three buffers, load+place the
     * ELF (text/rodata -> code at 0, data/bss -> heap at RISCV_HALF), and wire the
     * four engine regions + state. We keep the buffer pointers (handlers and the
     * signature dump reach g_heap) and install our trap handlers. */
    PurvHost host;
    if (purvhost_alloc(&host, CODE_BYTES, g_heap_size, STACK_BYTES) != 0) return 2;
    g_code = host.code; g_heap = host.heap; g_stack = host.stack;
    purvhost_load_elf(&host, elf);
    uint32_t entry = host.entry;
    g_brk = (host.data_end + 0xFFFu) & ~0xFFFu;               /* brk: past the data, page-aligned */

    RiscvEmulatorState_t state;
    purvhost_init(&host, &state);
    RiscvEmulatorState_t *st = &state;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;

    uint32_t start = entry;
    if (invoke) {
        if (!purvhost_sym(&host, invoke, &start)) {
            fprintf(stderr, "purv: symbol '%s' not found\n", invoke); return 2;
        }
        st->x[1] = MAGIC_RET;                                 /* ra: ret -> stop */
        for (int i = 0; i < nargs; i++)
            st->x[10 + i] = args[i];                          /* a0.. */
    } else if (g_user_mode) {
        st->x[2] = setup_user_stack(gargc, gargv);            /* sp */
    } else {
        g_have_tohost   = purvhost_sym(&host, "tohost", &g_tohost);
        g_have_sig      = purvhost_sym(&host, "begin_signature", &g_begin_sig) &
                          purvhost_sym(&host, "end_signature", &g_end_sig);
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
