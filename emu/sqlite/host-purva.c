/*
 * host-purva.c - host/driver that runs the freestanding SQLite guest on purva.
 *
 * Same role as host.c, but purva is image-based: there is no ELF at run time, no
 * decode -- this loads a purva IMAGE (../purva/image.h), produced ahead of time by
 * `transcode` from a guest linked with ../purva/purva.ld + --emit-relocs (see the
 * Makefile's *-purva targets). The host-call ABI (hostcalls.h) and the host-side
 * K&R allocator are unchanged from host.c -- the guest's dependency on the host is
 * the same five calls regardless of which engine runs it.
 */
#define _POSIX_C_SOURCE 199309L      /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../purva/purva.h"   /* purv.h + transcode.h + RiscvEmulatorSetProgram */
#include "../purva/image.h"
#include "hostcalls.h"

/* ------------------------------------------------------------------ memory */

#define RAM_BYTES   (256u * 1024 * 1024)
#define STACK_BYTES (16u * 1024 * 1024)
static uint8_t *g_heap, *g_stack;
#define g_arena    g_heap
#define ARENA_BASE RISCV_HALF

static int      g_halt;
static int      g_exit;

/* --------------------------- host-side heap allocator (the malloc group) ----
 * Identical to host.c's: the K&R allocator, blocks and free list in guest RAM,
 * handing back guest addresses -- the guest carries no allocator of its own. */
#define HUNIT 8u
static uint32_t heap_brk, heap_top, freep, basep;

static uint32_t hnext(uint32_t a)            { uint32_t v; memcpy(&v, &g_arena[a - ARENA_BASE], 4); return v; }
static uint32_t hsz(uint32_t a)              { uint32_t v; memcpy(&v, &g_arena[a - ARENA_BASE + 4], 4); return v; }
static void set_hnext(uint32_t a, uint32_t v){ memcpy(&g_arena[a - ARENA_BASE], &v, 4); }
static void set_hsz(uint32_t a, uint32_t v)  { memcpy(&g_arena[a - ARENA_BASE + 4], &v, 4); }

static void host_free(uint32_t ap);

static void heap_init(uint32_t base, uint32_t top) {
    basep = base; set_hnext(basep, basep); set_hsz(basep, 0);
    freep = basep;
    heap_brk = base + HUNIT;
    heap_top = top;
}
static uint32_t morecore(uint32_t nu) {
    if (nu < 4096) nu = 4096;
    uint32_t bytes = nu * HUNIT;
    if ((uint64_t)heap_brk + bytes > heap_top) return 0;
    uint32_t up = heap_brk; heap_brk += bytes;
    set_hsz(up, nu);
    host_free(up + HUNIT);
    return freep;
}
static uint32_t host_malloc(uint32_t nbytes) {
    if (nbytes == 0) return 0;
    uint32_t nunits = (nbytes + HUNIT - 1) / HUNIT + 1;
    uint32_t prevp = freep, p;
    for (p = hnext(prevp); ; prevp = p, p = hnext(p)) {
        if (hsz(p) >= nunits) {
            if (hsz(p) == nunits) set_hnext(prevp, hnext(p));
            else { set_hsz(p, hsz(p) - nunits); p = p + hsz(p) * HUNIT; set_hsz(p, nunits); }
            freep = prevp;
            return p + HUNIT;
        }
        if (p == freep) if ((p = morecore(nunits)) == 0) return 0;
    }
}
static void host_free(uint32_t ap) {
    uint32_t bp = ap - HUNIT, p;
    for (p = freep; !(bp > p && bp < hnext(p)); p = hnext(p))
        if (p >= hnext(p) && (bp > p || bp < hnext(p))) break;
    if (bp + hsz(bp) * HUNIT == hnext(p)) { set_hsz(bp, hsz(bp) + hsz(hnext(p))); set_hnext(bp, hnext(hnext(p))); }
    else set_hnext(bp, hnext(p));
    if (p + hsz(p) * HUNIT == bp) { set_hsz(p, hsz(p) + hsz(bp)); set_hnext(p, hnext(bp)); }
    else set_hnext(p, bp);
    freep = p;
}
static uint32_t host_realloc(uint32_t ap, uint32_t n) {
    if (ap == 0) return host_malloc(n);
    if (n == 0) { host_free(ap); return 0; }
    uint32_t oldbytes = (hsz(ap - HUNIT) - 1) * HUNIT;
    if (oldbytes >= n) return ap;
    uint32_t np = host_malloc(n);
    if (!np) return 0;
    memmove(&g_arena[np - ARENA_BASE], &g_arena[ap - ARENA_BASE], oldbytes < n ? oldbytes : n);
    host_free(ap);
    return np;
}

/* ------------------------------------------------------------- engine hooks */

static int on_illegal(RiscvEmulatorState_t *st) {
    fprintf(stderr, "purva-sqlite: illegal op 0x%08x at pc=0x%08x\n", st->inst, st->pc);
    g_halt = 1; g_exit = 1; return 1;
}
static uint32_t on_oob(RiscvEmulatorState_t *st, int op, uint32_t addr, uint32_t value) {
    (void)value;
    fprintf(stderr, "purva-sqlite: %s fault at addr=0x%08x pc=0x%08x sp=0x%08x\n",
            op == RISCV_MEM_STORE ? "store" : "load", addr, st->pc, st->x[2]);
    g_halt = 1; g_exit = 1; return 0;
}
static int on_ebreak(RiscvEmulatorState_t *st) { (void)st; return 1; }

/* Mirrors purva.c's mem_xlate: two self-describing regions, each { ptr, len, base }.
 * One bounded check per region -- writable first, then read-only rodata. */
static uint8_t gbyte(const RiscvEmulatorState_t *s, uint32_t a) {
    const RiscvEmulatorRegion_t *rw = &s->region[RISCV_WRITABLE];
    uint32_t rel = a - rw->base;
    if ((uint64_t)rel + 1 <= rw->len) return rw->ptr[rel];
    const RiscvEmulatorRegion_t *ro = &s->region[RISCV_READONLY];
    rel = a - ro->base;
    if ((uint64_t)rel + 1 <= ro->len) return ro->ptr[rel];
    return 0;
}

static int on_ecall(RiscvEmulatorState_t *st) {
    uint32_t fn = st->x[17], a0 = st->x[10], a1 = st->x[11], a2 = st->x[12], ret = 0;
    switch (fn) {
    case HOSTCALL_WRITE: {
        uint32_t left = a2, p = a1;
        char buf[4096];
        while (left) {
            uint32_t chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
            for (uint32_t j = 0; j < chunk; j++) buf[j] = (char)gbyte(st, p + j);
            ssize_t wr = write(a0 == 2 ? 2 : 1, buf, chunk);
            if (wr < 0) break;
            p += chunk; left -= chunk;
        }
        ret = a2 - left;
        break;
    }
    case HOSTCALL_EXIT:
        g_exit = (int)a0; g_halt = 1;
        break;
    case HOSTCALL_MALLOC:
        ret = host_malloc(a0);
        break;
    case HOSTCALL_FREE:
        if (a0) host_free(a0);
        break;
    case HOSTCALL_REALLOC:
        ret = host_realloc(a0, a1);
        break;
    default:
        fprintf(stderr, "purva-sqlite: unknown host call %u\n", fn);
        g_halt = 1; g_exit = 1;
        break;
    }
    st->x[10] = ret;
    return g_halt;
}

/* ------------------------------------------------------------------- main */

extern int g_itrace;    /* TEMP debug */
extern uint32_t g_watch; /* TEMP debug */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <guest.img> [--max-insns=N] [--stats]\n", argv[0]); return 2; }
    uint64_t max_insns = 20000ull * 1000 * 1000;
    int stats = 0;
    g_itrace = getenv("ITRACE") != NULL;
    if (getenv("WATCH")) g_watch = (uint32_t)strtoul(getenv("WATCH"), 0, 0);
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--max-insns=", 12)) max_insns = strtoull(argv[i] + 12, 0, 0);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
    }

    Image img;
    if (image_read(argv[1], &img) != 0) { fprintf(stderr, "host-purva: cannot read %s\n", argv[1]); return 2; }

    /* Two contiguous clusters (purv.h / purva.c mem_xlate):
     *
     * read-only: region[RODATA] is img.rodata, guest base 0 - rodata_size -- it grows
     *   down from 0, living at small NEGATIVE addresses. region[CODE] is the packed op
     *   words; purva never data-addresses code (see image.h), so it needs no data map.
     *
     * writable: ONE buffer, stack then heap, back to back. The stack occupies
     *   [RISCV_HALF - STACK_BYTES, RISCV_HALF) (sp starts at RISCV_HALF, grows down);
     *   the heap occupies [RISCV_HALF, RISCV_HALF + heap_len): rwdata, then bss (the
     *   compiled code's globals, baked in at the linker's real offset right after
     *   rwdata), then the malloc arena (RAM_BYTES, same budget as host.c). */
    uint32_t heap_len = img.rwdata_size + img.bss_size + RAM_BYTES;
    uint8_t *writable = calloc(1, (size_t)STACK_BYTES + heap_len);
    g_stack = writable;                       /* stack bottom = guest RISCV_HALF - STACK_BYTES */
    g_heap  = writable + STACK_BYTES;         /* heap base    = guest RISCV_HALF */
    if (img.rwdata_size) memcpy(g_heap, img.rwdata, img.rwdata_size);
    uint32_t heap_base = RISCV_HALF + img.rwdata_size + img.bss_size;
    heap_base = (heap_base + 15u) & ~15u;
    heap_init(heap_base, RISCV_HALF + heap_len);

    RiscvEmulatorState_t state;
    RiscvEmulatorInit(&state,
        (RiscvEmulatorRegion_t){ img.code, img.code_size },
        (RiscvEmulatorRegion_t){ img.rodata, img.rodata_size },
        (RiscvEmulatorRegion_t){ g_heap, heap_len },
        (RiscvEmulatorRegion_t){ g_stack, STACK_BYTES });
    RiscvEmulatorState_t *st = &state;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;

    Transcoded prog = { (uint32_t *)img.code, img.code_size / 4, img.code_size };
    RiscvEmulatorSetProgram(&prog);

    st->pc = 0;            /* entry is always 0 (image.h) */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t i = 0;
    const uint64_t SLICE = 1u << 16;
    while (i < max_insns && !g_halt) {
        uint64_t budget = max_insns - i;
        if (budget > SLICE) budget = SLICE;
        uint64_t ran = RiscvEmulatorLoop(st, budget);
        i += ran;
        if (g_halt) break;
        if (ran < budget) {
            fprintf(stderr, "purva-sqlite: fetch outside code at pc=0x%08x\n", st->pc);
            g_halt = 1; g_exit = 1; break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (i >= max_insns) { fprintf(stderr, "purva-sqlite: instruction cap reached\n"); g_exit = 3; }

    if (stats) {
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "BENCH wall_ms=%.0f insns=%llu mips=%.1f\n",
                ms, (unsigned long long)i, ms > 0 ? i / (ms * 1000.0) : 0.0);
    }
    return g_exit;
}
