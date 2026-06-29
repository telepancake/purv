/*
 * host.c - host/driver that runs the freestanding SQLite guest on the purv engine.
 *
 * The host-call ABI is deliberately split the way the task describes:
 *   - the engine's ECALL hook does almost nothing: it just raises a flag and
 *     consumes the trap (RiscvEmulatorClearTrap), so the engine resumes after
 *     the ecall;
 *   - the run loop, after each stepped instruction, checks the flag and *that*
 *     is where host functions are actually serviced (reading a7/a0.. and writing
 *     the result back into a0).
 *
 * The guest's entire dependency on the outside world is the two host calls
 * (write, exit). There is no MMIO, no syscall ABI baked into the engine.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "../purv.h"
#include "hostcalls.h"

/* ------------------------------------------------------------------ memory */

#define RAM_BYTES (256u * 1024 * 1024)
static uint8_t *g_ram;

static int in_ram(uint32_t a, uint32_t n) {
    return a >= RAM_ORIGIN && (uint64_t)a + n <= (uint64_t)RAM_ORIGIN + RAM_BYTES;
}

/* ----------------------------------------------- run state and the ecall flag */

static int      g_halt;
static int      g_exit;
static int      g_ecall_pending;      /* raised by the ECALL hook, serviced by the loop */

/* --------------------------- host-side heap allocator (the malloc group) ----
 * The allocator runs here, but the blocks and its free list live in guest RAM;
 * it hands back guest addresses. This is the K&R allocator with guest-address
 * "pointers", so the guest carries no allocator and reserves no static heap --
 * memory grows on demand within the RAM the host already has. */
#define HUNIT 8u                              /* bytes per header and allocation unit */
static uint32_t heap_brk, heap_top, freep, basep;

static uint32_t hnext(uint32_t a)            { uint32_t v; memcpy(&v, &g_ram[a - RAM_ORIGIN], 4); return v; }
static uint32_t hsz(uint32_t a)              { uint32_t v; memcpy(&v, &g_ram[a - RAM_ORIGIN + 4], 4); return v; }
static void set_hnext(uint32_t a, uint32_t v){ memcpy(&g_ram[a - RAM_ORIGIN], &v, 4); }
static void set_hsz(uint32_t a, uint32_t v)  { memcpy(&g_ram[a - RAM_ORIGIN + 4], &v, 4); }

static void host_free(uint32_t ap);

static void heap_init(uint32_t base, uint32_t top) {
    basep = base; set_hnext(basep, basep); set_hsz(basep, 0);   /* circular base sentinel */
    freep = basep;
    heap_brk = base + HUNIT;                                    /* sentinel occupies first 8 bytes */
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
    uint32_t nunits = (nbytes + HUNIT - 1) / HUNIT + 1;        /* +1 for the header */
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
        if (p >= hnext(p) && (bp > p || bp < hnext(p))) break;  /* edge of the arena */
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
    memmove(&g_ram[np - RAM_ORIGIN], &g_ram[ap - RAM_ORIGIN], oldbytes < n ? oldbytes : n);
    host_free(ap);
    return np;
}

/* ------------------------------------------------------------- engine hooks */

void RiscvEmulatorLoad(uint32_t addr, void *dst, uint8_t len) {
    if (in_ram(addr, len)) memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
    else memset(dst, 0, len);
}
void RiscvEmulatorStore(uint32_t addr, const void *src, uint8_t len) {
    if (in_ram(addr, len)) memcpy(&g_ram[addr - RAM_ORIGIN], src, len);
    else { fprintf(stderr, "purv-sqlite: stray store 0x%08x len %u\n", addr, len); g_halt = 1; g_exit = 1; }
}
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *st) {
    fprintf(stderr, "purv-sqlite: illegal instruction 0x%08x at pc=0x%08x\n",
            RiscvEmulatorGetInstruction(st), RiscvEmulatorGetProgramCounter(st));
    g_halt = 1; g_exit = 1;
}
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *st) { RiscvEmulatorRaiseIllegalInstruction(st); }
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *st, uint16_t csr) {
    static uint32_t misa = (1u << 30) | (1u << ('I' - 'A')) | (1u << ('M' - 'A')) | (1u << ('C' - 'A'));
    static uint32_t zero;
    (void)st;
    if (csr == 0x301) return &misa;
    zero = 0; return &zero;
}
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *st) { (void)st; }

/* The whole hook: mark that an ecall happened and let the engine continue. The
 * actual work is done by the run loop below. */
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *st) {
    g_ecall_pending = 1;
    RiscvEmulatorClearTrap(st);
}

/* ------------------------------------------------ host-function service loop */

/* Copy len bytes out of guest RAM into a host buffer (for write). */
static void guest_read(uint32_t addr, void *dst, uint32_t len) {
    if (in_ram(addr, len)) memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
    else memset(dst, 0, len);
}

static void service_hostcall(RiscvEmulatorState_t *st) {
    uint32_t fn = RiscvEmulatorGetRegister(st, 17);    /* a7 */
    uint32_t a0 = RiscvEmulatorGetRegister(st, 10);
    uint32_t a1 = RiscvEmulatorGetRegister(st, 11);
    uint32_t a2 = RiscvEmulatorGetRegister(st, 12);
    uint32_t ret = 0;

    switch (fn) {
    case HOSTCALL_WRITE: {                              /* a0=fd, a1=buf, a2=len */
        uint32_t left = a2;
        uint32_t p = a1;
        char buf[4096];
        while (left) {
            uint32_t chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
            guest_read(p, buf, chunk);
            ssize_t wr = write(a0 == 2 ? 2 : 1, buf, chunk);
            if (wr < 0) break;
            p += chunk; left -= chunk;
        }
        ret = a2 - left;
        break;
    }
    case HOSTCALL_EXIT:                                 /* a0=code */
        g_exit = (int)a0; g_halt = 1;
        break;
    case HOSTCALL_MALLOC:                               /* a0=size */
        ret = host_malloc(a0);
        break;
    case HOSTCALL_FREE:                                 /* a0=ptr */
        if (a0) host_free(a0);
        ret = 0;
        break;
    case HOSTCALL_REALLOC:                              /* a0=ptr, a1=size */
        ret = host_realloc(a0, a1);
        break;
    default:
        fprintf(stderr, "purv-sqlite: unknown host call %u\n", fn);
        g_halt = 1; g_exit = 1;
        break;
    }
    RiscvEmulatorSetRegister(st, 10, ret);             /* result in a0 */
}

/* ------------------------------------------------------------- ELF32 loader */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
    e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Ehdr32;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_flags, p_align; } Phdr32;
#define PT_LOAD 1
#define EM_RISCV 243

static uint32_t g_image_end;          /* highest guest address used by the loaded image */

static uint32_t load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); exit(2); }
    fclose(f);
    if (n < (long)sizeof(Ehdr32) || memcmp(buf, "\177ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); exit(2); }
    Ehdr32 eh; memcpy(&eh, buf, sizeof eh);
    if (eh.e_ident[4] != 1 || eh.e_machine != EM_RISCV) { fprintf(stderr, "not RV32 ELF\n"); exit(2); }
    for (int i = 0; i < eh.e_phnum; i++) {
        Phdr32 ph; memcpy(&ph, buf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (!in_ram(ph.p_paddr, ph.p_memsz)) { fprintf(stderr, "segment outside RAM\n"); exit(2); }
        if (ph.p_filesz) memcpy(&g_ram[ph.p_paddr - RAM_ORIGIN], buf + ph.p_offset, ph.p_filesz);
        if (ph.p_paddr + ph.p_memsz > g_image_end) g_image_end = ph.p_paddr + ph.p_memsz;
    }
    free(buf);
    return eh.e_entry;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <guest.elf> [--max-insns=N]\n", argv[0]); return 2; }
    uint64_t max_insns = 20000ull * 1000 * 1000;       /* generous cap */
    for (int i = 2; i < argc; i++)
        if (!strncmp(argv[i], "--max-insns=", 12)) max_insns = strtoull(argv[i] + 12, 0, 0);

    g_ram = calloc(1, RAM_BYTES);
    if (!g_ram) { fprintf(stderr, "OOM\n"); return 2; }

    uint32_t entry = load_elf(argv[1]);

    /* Heap arena: from just past the loaded image up to a stack reserve below
     * the top of RAM. Grows on demand via the malloc host calls. */
    #define STACK_RESERVE (16u * 1024 * 1024)
    uint32_t heap_base = (g_image_end + 15u) & ~15u;
    heap_init(heap_base, RAM_ORIGIN + RAM_BYTES - STACK_RESERVE);

    RiscvEmulatorState_t *st = RiscvEmulatorCreate(RAM_ORIGIN + RAM_BYTES);
    if (!st) { fprintf(stderr, "cannot create state\n"); return 2; }
    RiscvEmulatorSetRegister(st, 2, RAM_ORIGIN + RAM_BYTES);   /* sp at top of RAM */
    RiscvEmulatorSetProgramCounter(st, entry);

    uint64_t i = 0;
    for (; i < max_insns && !g_halt; i++) {
        RiscvEmulatorLoop(st);
        if (g_ecall_pending) { g_ecall_pending = 0; service_hostcall(st); }
    }
    if (i >= max_insns) { fprintf(stderr, "purv-sqlite: instruction cap reached\n"); g_exit = 3; }

    RiscvEmulatorDestroy(st);
    return g_exit;
}
