/*
 * host.c - self-contained host for the compact demo.
 *
 * Its only dependency is the purv engine (../purv.h, link ../purv.c). It loads a
 * FLAT binary (no ELF), runs it, and services the four host calls the demo uses
 * (exit, write, malloc, free). The ecall hook just raises a flag; the run loop
 * services the call -- the engine has no syscall ABI baked in.
 *
 *   usage: host <flat-image>      (loaded at RAM_ORIGIN, entry there)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "../purv.h"
#include "hostcalls.h"

#define RAM_BYTES (64u * 1024 * 1024)
static uint8_t *g_ram;

static int in_ram(uint32_t a, uint32_t n) {
    return a >= RAM_ORIGIN && (uint64_t)a + n <= (uint64_t)RAM_ORIGIN + RAM_BYTES;
}

static int      g_halt, g_exit, g_ecall;
static uint32_t g_image_end;

/* ---- tiny heap over guest RAM: K&R allocator with guest-address "pointers" --- */
#define HUNIT 8u
static uint32_t heap_brk, heap_top, freep, basep;
static uint32_t hnext(uint32_t a)             { uint32_t v; memcpy(&v, &g_ram[a - RAM_ORIGIN], 4); return v; }
static uint32_t hsz(uint32_t a)               { uint32_t v; memcpy(&v, &g_ram[a - RAM_ORIGIN + 4], 4); return v; }
static void     set_hnext(uint32_t a, uint32_t v) { memcpy(&g_ram[a - RAM_ORIGIN], &v, 4); }
static void     set_hsz(uint32_t a, uint32_t v)   { memcpy(&g_ram[a - RAM_ORIGIN + 4], &v, 4); }
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

/* ------------------------------------------------------------- engine hooks */

void RiscvEmulatorLoad(uint32_t addr, void *dst, uint8_t len) {
    if (in_ram(addr, len)) memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
    else memset(dst, 0, len);
}
void RiscvEmulatorStore(uint32_t addr, const void *src, uint8_t len) {
    if (in_ram(addr, len)) memcpy(&g_ram[addr - RAM_ORIGIN], src, len);
    else { fprintf(stderr, "compact: stray store 0x%08x\n", addr); g_halt = 1; g_exit = 1; }
}
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *st) {
    fprintf(stderr, "compact: illegal instruction 0x%08x at pc=0x%08x\n",
            RiscvEmulatorGetInstruction(st), RiscvEmulatorGetProgramCounter(st));
    g_halt = 1; g_exit = 1;
}
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *st) { RiscvEmulatorRaiseIllegalInstruction(st); }
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *st, uint16_t csr) {
    static uint32_t z; (void)st; (void)csr; z = 0; return &z;
}
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *st) { (void)st; }
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *st) { g_ecall = 1; RiscvEmulatorClearTrap(st); }

static void service(RiscvEmulatorState_t *st) {
    uint32_t fn = RiscvEmulatorGetRegister(st, 17);
    uint32_t a0 = RiscvEmulatorGetRegister(st, 10);
    uint32_t a1 = RiscvEmulatorGetRegister(st, 11);
    uint32_t a2 = RiscvEmulatorGetRegister(st, 12);
    uint32_t ret = 0;
    switch (fn) {
    case HOSTCALL_WRITE:
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t b = 0;
            if (in_ram(a1 + i, 1)) b = g_ram[(a1 + i) - RAM_ORIGIN];
            if (write(a0 == 2 ? 2 : 1, &b, 1) < 0) break;
        }
        ret = a2;
        break;
    case HOSTCALL_EXIT:   g_exit = (int)a0; g_halt = 1; break;
    case HOSTCALL_MALLOC: ret = host_malloc(a0); break;
    case HOSTCALL_FREE:   if (a0) host_free(a0); ret = 0; break;
    default: fprintf(stderr, "compact: unknown host call %u\n", fn); g_halt = 1; g_exit = 1; break;
    }
    RiscvEmulatorSetRegister(st, 10, ret);
}

/* ----------------------------------------------------------- flat image load */

static uint32_t load_flat(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "compact: cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || !in_ram(RAM_ORIGIN, (uint32_t)n)) { fprintf(stderr, "compact: bad image\n"); exit(2); }
    if (fread(&g_ram[0], 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "compact: read failed\n"); exit(2); }
    fclose(f);
    g_image_end = RAM_ORIGIN + (uint32_t)n;
    return RAM_ORIGIN;                      /* flat: load at RAM_ORIGIN, entry there */
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <flat-image>\n", argv[0]); return 2; }
    g_ram = calloc(1, RAM_BYTES);
    if (!g_ram) { fprintf(stderr, "compact: OOM\n"); return 2; }

    uint32_t entry = load_flat(argv[1]);
    heap_init((g_image_end + 15u) & ~15u, RAM_ORIGIN + RAM_BYTES - 1024u * 1024u);

    RiscvEmulatorState_t *st = RiscvEmulatorCreate(RAM_ORIGIN + RAM_BYTES);
    if (!st) { fprintf(stderr, "compact: cannot create state\n"); return 2; }
    RiscvEmulatorSetRegister(st, 2, RAM_ORIGIN + RAM_BYTES);    /* sp */
    RiscvEmulatorSetProgramCounter(st, entry);

    for (uint64_t i = 0; i < 1000ull * 1000 * 1000 && !g_halt; i++) {
        RiscvEmulatorLoop(st);
        if (g_ecall) { g_ecall = 0; service(st); }
    }
    RiscvEmulatorDestroy(st);
    return g_exit;
}
