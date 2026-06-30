/*
 * host.c - self-contained host for the compact demo.
 *
 * Its only dependency is the purv engine (../purv.h, link ../purv.c). It loads a
 * FLAT binary (no ELF), runs it, and services the four host calls the demo uses
 * (exit, write, malloc, free). The ecall hook just raises a flag; the run loop
 * services the call -- the engine has no syscall ABI baked in.
 *
 *   usage: host <flat-image>      (loaded at 0 in the code region, entry there)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "../purv.h"
#include "../purvhost.h"
#include "hostcalls.h"

/* Four engine regions, three host buffers: g_code holds the flat image (text +
 * rodata, read-only lower half at 0, fetched); g_heap is the malloc arena (upper
 * half at RISCV_HALF); g_stack is the stack (top, grows down). purvhost.h does the
 * allocation, flat-image load, and region/state wiring; the K&R allocator below
 * keeps driving g_heap. */
#define CODE_BYTES     (16u * 1024 * 1024)
#define HEAP_BYTES     (48u * 1024 * 1024)
#define STACK_BYTES    (4u * 1024 * 1024)
static uint8_t *g_code;                  /* flat image: [0, CODE_BYTES) */
static uint8_t *g_heap;                  /* malloc arena: [RISCV_HALF, +HEAP_BYTES) */
static uint8_t *g_stack;                 /* stack (grows down) */

static int      g_halt, g_exit;

/* ---- tiny heap over the guest heap region: K&R allocator with guest-address
 * "pointers" (all in [RISCV_HALF, RISCV_HALF+HEAP_BYTES), the read/write half) --- */
#define HUNIT 8u
static uint32_t heap_brk, heap_top, freep, basep;
static uint32_t hnext(uint32_t a)             { uint32_t v; memcpy(&v, &g_heap[a - RISCV_HALF], 4); return v; }
static uint32_t hsz(uint32_t a)               { uint32_t v; memcpy(&v, &g_heap[a - RISCV_HALF + 4], 4); return v; }
static void     set_hnext(uint32_t a, uint32_t v) { memcpy(&g_heap[a - RISCV_HALF], &v, 4); }
static void     set_hsz(uint32_t a, uint32_t v)   { memcpy(&g_heap[a - RISCV_HALF + 4], &v, 4); }
static void heap_init(uint32_t base, uint32_t top) {
    basep = base; set_hnext(basep, basep); set_hsz(basep, 0);
    freep = basep; heap_brk = base + HUNIT; heap_top = top;
}
static void host_free(uint32_t ap);
static uint32_t morecore(uint32_t nu) {
    if (nu < 4096) nu = 4096;
    uint32_t bytes = nu * HUNIT;
    if ((uint64_t)heap_brk + bytes > heap_top) return 0;
    uint32_t up = heap_brk; heap_brk += bytes;
    set_hsz(up, nu); host_free(up + HUNIT);
    return freep;
}
static uint32_t host_malloc(uint32_t nbytes) {
    if (nbytes == 0) return 0;
    uint32_t nunits = (nbytes + HUNIT - 1) / HUNIT + 1, prevp = freep, p;
    for (p = hnext(prevp); ; prevp = p, p = hnext(p)) {
        if (hsz(p) >= nunits) {
            if (hsz(p) == nunits) set_hnext(prevp, hnext(p));
            else { set_hsz(p, hsz(p) - nunits); p = p + hsz(p) * HUNIT; set_hsz(p, nunits); }
            freep = prevp; return p + HUNIT;
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

/* ------------------------------------------- trap handlers (assigned to the state) */

static void service(RiscvEmulatorState_t *st);
static int on_illegal(RiscvEmulatorState_t *st) {
    fprintf(stderr, "compact: illegal instruction 0x%08x at pc=0x%08x\n",
            st->inst, st->pc);
    g_halt = 1; g_exit = 1; return 1;
}
static uint32_t on_oob(RiscvEmulatorState_t *st, int op, uint32_t addr, uint32_t value) {
    (void)value;
    fprintf(stderr, "compact: %s fault at addr=0x%08x pc=0x%08x\n",
            op == RISCV_MEM_STORE ? "store" : "load", addr, st->pc);
    g_halt = 1; g_exit = 1; return 0;
}
static int on_ebreak(RiscvEmulatorState_t *st) { (void)st; return 1; }
/* Service the host call in place and continue unless it halted (exit). */
static int on_ecall(RiscvEmulatorState_t *st) { service(st); return g_halt; }

static void service(RiscvEmulatorState_t *st) {
    uint32_t fn = st->x[17];
    uint32_t a0 = st->x[10];
    uint32_t a1 = st->x[11];
    uint32_t a2 = st->x[12];
    uint32_t ret = 0;
    switch (fn) {
    case HOSTCALL_WRITE:
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t b = purvhost_guest_byte(st, a1 + i);
            if (write(a0 == 2 ? 2 : 1, &b, 1) < 0) break;
        }
        ret = a2;
        break;
    case HOSTCALL_EXIT:   g_exit = (int)a0; g_halt = 1; break;
    case HOSTCALL_MALLOC: ret = host_malloc(a0); break;
    case HOSTCALL_FREE:   if (a0) host_free(a0); ret = 0; break;
    default: fprintf(stderr, "compact: unknown host call %u\n", fn); g_halt = 1; g_exit = 1; break;
    }
    st->x[10] = ret;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <flat-image>\n", argv[0]); return 2; }

    /* purvhost handles the recurring puzzle: allocate the three buffers, load the
     * flat image into the code region at 0 (entry there), and wire the four engine
     * regions + state. We keep pointers for the K&R allocator and install handlers. */
    PurvHost host;
    if (purvhost_alloc(&host, CODE_BYTES, HEAP_BYTES, STACK_BYTES) != 0) return 2;
    g_code = host.code; g_heap = host.heap; g_stack = host.stack;
    purvhost_load_flat(&host, argv[1]);
    heap_init(RISCV_HALF, RISCV_HALF + HEAP_BYTES);   /* malloc arena = all of the heap region */

    RiscvEmulatorState_t state;
    purvhost_init(&host, &state);
    RiscvEmulatorState_t *st = &state;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->callback = on_oob;
    st->pc = host.entry;

    /* pc lives in the state; run in slices off the flat code image. */
    const uint64_t CAP = 1000ull * 1000 * 1000, SLICE = 1u << 16;
    uint64_t i = 0;
    while (i < CAP && !g_halt) {
        uint64_t budget = CAP - i;
        if (budget > SLICE) budget = SLICE;
        uint64_t ran = RiscvEmulatorLoop(st, budget);
        i += ran;
        if (g_halt) break;
        if (ran < budget) {                  /* pc left the code region: stray fetch */
            fprintf(stderr, "compact: fetch outside code at pc=0x%08x\n", st->pc);
            g_halt = 1; g_exit = 1; break;
        }
    }
    return g_exit;
}
