/*
 * host.c - host/driver that runs the freestanding SQLite guest on the purv engine.
 *
 * The host-call ABI is deliberately split the way the task describes:
 *   - the engine's ECALL hook does almost nothing: it just raises a flag, and
 *     the engine resumes after the ecall (ecall is a userspace service request,
 *     so it returns straight to the caller -- nothing vectors anywhere);
 *   - the run loop, after each stepped instruction, checks the flag and *that*
 *     is where host functions are actually serviced (reading a7/a0.. and writing
 *     the result back into a0).
 *
 * The guest's entire dependency on the outside world is the two host calls
 * (write, exit). There is no MMIO, no syscall ABI baked into the engine.
 */
#define _POSIX_C_SOURCE 199309L      /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

/* The same host drives two engines: plain purv, or purvs (the tagged-memory
 * variant) when built -DPURV_TAGGED. purvs has the same public API plus tagged
 * load/store/fetch/control-transfer hook signatures; we implement those
 * permissively here (do the access, ignore the tags) so an ordinary, untagged
 * workload like SQLite runs unchanged and we measure only the engine's
 * per-instruction tag-tracking overhead, not its enforcement. */
#ifdef PURV_TAGGED
#include "../purvs/purvs.h"
#else
#include "../purv.h"
#endif
#include "hostcalls.h"

/* ------------------------------------------------------------------ memory */

#define RAM_BYTES   (256u * 1024 * 1024)
#define STACK_BYTES (16u * 1024 * 1024)
#define STACK_MEM_BASE ((uint32_t)(0u - STACK_BYTES))   /* stack ends at the last address */
static uint8_t *g_ram;                    /* code + data + heap (region 0) */
#ifndef PURV_TAGGED
static uint8_t *g_stack;                  /* stack (top region); purvs keeps its stack in region 0 */
#endif

static int in_ram(uint32_t a, uint32_t n) {
    return a >= RAM_ORIGIN && (uint64_t)a + n <= (uint64_t)RAM_ORIGIN + RAM_BYTES;
}

/* ----------------------------------------------- run state and the ecall flag */

static int      g_halt;
static int      g_exit;
#ifdef PURV_TAGGED
static int      g_ecall_pending;      /* purvs: raised by the ECALL hook, serviced by the loop */
#endif

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

static void service_hostcall(RiscvEmulatorState_t *st);

#ifdef PURV_TAGGED
/* purvs is the older, machine-mode tagged engine: it still reaches the host
 * through link-time memory + trap hooks. Tags are ignored here (we only time the
 * engine's per-instruction tag tracking, not enforce a policy). */
static void mem_read(uint32_t addr, void *dst, uint32_t len) {
    if (in_ram(addr, len)) memcpy(dst, &g_ram[addr - RAM_ORIGIN], len);
    else memset(dst, 0, len);
}
static void mem_write(uint32_t addr, const void *src, uint32_t len) {
    if (in_ram(addr, len)) memcpy(&g_ram[addr - RAM_ORIGIN], src, len);
    else { fprintf(stderr, "purv-sqlite: stray store 0x%08x len %u\n", addr, len); g_halt = 1; g_exit = 1; }
}
uint32_t RiscvEmulatorFetch(uint32_t addr, void *dst, uint8_t len) { mem_read(addr, dst, len); return 0; }
uint32_t RiscvEmulatorLoad(uint32_t addr, uint32_t addr_tag, void *dst, uint8_t len) {
    (void)addr_tag; mem_read(addr, dst, len); return 0;
}
void RiscvEmulatorStore(uint32_t addr, uint32_t addr_tag, const void *src, uint32_t value_tag, uint8_t len) {
    (void)addr_tag; (void)value_tag; mem_write(addr, src, len);
}
void RiscvEmulatorControlTransfer(uint32_t from, uint32_t from_tag, uint32_t to, uint32_t to_tag) {
    (void)from; (void)from_tag; (void)to; (void)to_tag;
}
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *st) {
    fprintf(stderr, "purv-sqlite: illegal instruction 0x%08x at pc=0x%08x\n",
            RiscvEmulatorGetInstruction(st), RiscvEmulatorGetProgramCounter(st));
    g_halt = 1; g_exit = 1;
}
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *st) { (void)st; }
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *st) { (void)st; g_ecall_pending = 1; }
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *st) { RiscvEmulatorRaiseIllegalInstruction(st); }
void *RiscvEmulatorGetUnknownCSR(RiscvEmulatorState_t *st, uint16_t csr) {
    static uint32_t misa = (1u << 30) | (1u << ('I' - 'A')) | (1u << ('M' - 'A')) | (1u << ('C' - 'A'));
    static uint32_t zero;
    (void)st;
    if (csr == 0x301) return &misa;
    zero = 0; return &zero;
}
#else
/* purv: memory lives in the engine (region 0 == g_ram, mapped in main); the trap
 * handlers are assigned to the state. The ecall handler services the call in
 * place and returns whether to stop -- write/malloc/... return 0 and execution
 * continues inside the engine loop; only exit (which sets g_halt) returns 1. */
static int on_illegal(RiscvEmulatorState_t *st) {
    fprintf(stderr, "purv-sqlite: illegal instruction 0x%08x at pc=0x%08x\n",
            st->inst, st->pc);
    g_halt = 1; g_exit = 1; return 1;
}
static int on_overflow(RiscvEmulatorState_t *st) {
    fprintf(stderr, "purv-sqlite: stack overflow at pc=0x%08x (sp=0x%08x)\n",
            st->pc, st->x[2]);
    g_halt = 1; g_exit = 1; return 1;
}
static int on_ebreak(RiscvEmulatorState_t *st) { (void)st; return 1; }
static int on_ecall(RiscvEmulatorState_t *st) { service_hostcall(st); return g_halt; }
#endif

/* Register / pc access. purv exposes the state struct (use fields); purvs keeps
 * the accessor functions. The shared code below goes through these shims. */
#ifdef PURV_TAGGED
static inline uint32_t rget(const RiscvEmulatorState_t *s, int i)   { return RiscvEmulatorGetRegister(s, i); }
static inline void     rset(RiscvEmulatorState_t *s, int i, uint32_t v) { RiscvEmulatorSetRegister(s, i, v); }
static inline void     spc(RiscvEmulatorState_t *s, uint32_t v)    { RiscvEmulatorSetProgramCounter(s, v); }
static inline uint32_t gpc(const RiscvEmulatorState_t *s)          { return RiscvEmulatorGetProgramCounter(s); }
static inline uint32_t gnpc(const RiscvEmulatorState_t *s)         { return RiscvEmulatorGetNextProgramCounter(s); }
#else
static inline uint32_t rget(const RiscvEmulatorState_t *s, int i)   { return s->x[i]; }
static inline void     rset(RiscvEmulatorState_t *s, int i, uint32_t v) { s->x[i] = v; }
static inline void     spc(RiscvEmulatorState_t *s, uint32_t v)    { s->pc = v; }
static inline uint32_t gpc(const RiscvEmulatorState_t *s)          { return s->pc; }
static inline uint32_t gnpc(const RiscvEmulatorState_t *s)         { return s->npc; }
#endif

/* A guest byte for the write syscall -- it may point into any region (the stack
 * is its own region now), so purv walks the region map; purvs reads region 0. */
#ifdef PURV_TAGGED
static inline uint8_t gbyte(const RiscvEmulatorState_t *s, uint32_t a) {
    (void)s; return in_ram(a, 1) ? g_ram[a - RAM_ORIGIN] : 0;
}
#else
static inline uint8_t gbyte(const RiscvEmulatorState_t *s, uint32_t a) {
    if (a >= RISCV_STACK_BASE) {                  /* the stack (top region) */
        uint32_t base = (uint32_t)(0u - s->stack.len);
        return (s->stack.ptr && a >= base) ? s->stack.ptr[a - base] : 0;
    }
    if (a < RAM_ORIGIN) return 0;
    const RiscvEmulatorRegion_t *r = &s->mem[(a - RAM_ORIGIN) / RISCV_REGION_SIZE];
    uint32_t off = (a - RAM_ORIGIN) % RISCV_REGION_SIZE;
    return (r->ptr && off < r->len) ? r->ptr[off] : 0;
}
#endif

/* ------------------------------------------------ host-function service loop */

static void service_hostcall(RiscvEmulatorState_t *st) {
    uint32_t fn = rget(st, 17);    /* a7 */
    uint32_t a0 = rget(st, 10);
    uint32_t a1 = rget(st, 11);
    uint32_t a2 = rget(st, 12);
    uint32_t ret = 0;

    switch (fn) {
    case HOSTCALL_WRITE: {                              /* a0=fd, a1=buf, a2=len */
        uint32_t left = a2;
        uint32_t p = a1;
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
    rset(st, 10, ret);                                 /* result in a0 */
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
    if (argc < 2) { fprintf(stderr, "usage: %s <guest.elf> [--max-insns=N] [--stats]\n", argv[0]); return 2; }
    uint64_t max_insns = 20000ull * 1000 * 1000;       /* generous cap */
    int stats = 0;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--max-insns=", 12)) max_insns = strtoull(argv[i] + 12, 0, 0);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
    }

    g_ram = calloc(1, RAM_BYTES);
    if (!g_ram) { fprintf(stderr, "OOM\n"); return 2; }

    uint32_t entry = load_elf(argv[1]);
    uint32_t heap_base = (g_image_end + 15u) & ~15u;   /* heap: just past the image */

#ifdef PURV_TAGGED
    /* purvs keeps its stack at the top of region 0, so reserve it from the heap. */
    heap_init(heap_base, RAM_ORIGIN + RAM_BYTES - 16u * 1024 * 1024);
    RiscvEmulatorState_t *st = RiscvEmulatorCreate(RAM_ORIGIN + RAM_BYTES);
    if (!st) { fprintf(stderr, "cannot create state\n"); return 2; }
    rset(st, 2, RAM_ORIGIN + RAM_BYTES);   /* sp at top of region 0 */
#else
    /* purv: the stack is its own (top) region, so the heap uses all of region 0.
     * Init wires code + stack + sp; we add region 0 and the handlers. */
    g_stack = calloc(1, STACK_BYTES);
    if (!g_stack) { fprintf(stderr, "OOM\n"); return 2; }
    heap_init(heap_base, RAM_ORIGIN + RAM_BYTES);
    RiscvEmulatorRegion_t code  = { g_ram, RAM_BYTES, 1 };
    RiscvEmulatorRegion_t stack = { g_stack, STACK_BYTES, 1 };
    RiscvEmulatorState_t state;
    RiscvEmulatorInit(&state, code, stack);   /* sp = 0 (top of the stack region) */
    RiscvEmulatorState_t *st = &state;
    st->mem[0] = code;
    st->ecall = on_ecall;
    st->ebreak = on_ebreak;
    st->illegal = on_illegal;
    st->overflow = on_overflow;
#endif
    spc(st, entry);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t i = 0;
#ifdef PURV_TAGGED
    /* purvs (old API): one instruction per call, service the ecall flag after. */
    uint32_t pc = entry;
    for (; i < max_insns && !g_halt; i++) {
        pc = RiscvEmulatorLoop(st, pc);
        if (g_ecall_pending) { g_ecall_pending = 0; service_hostcall(st); pc = gnpc(st); }
    }
#else
    /* purv: the pc lives in the state; run in slices, fetching from the flat RAM
     * image. ecall is serviced in the hook (no per-instruction return); we
     * re-check g_halt -- which the store hook may raise -- between slices. */
    const uint64_t SLICE = 1u << 16;
    while (i < max_insns && !g_halt) {
        uint64_t budget = max_insns - i;
        if (budget > SLICE) budget = SLICE;
        uint64_t ran = RiscvEmulatorLoop(st, budget);
        i += ran;
        if (g_halt) break;
        if (ran < budget) {                  /* pc left RAM: stray fetch */
            fprintf(stderr, "purv-sqlite: fetch outside RAM at pc=0x%08x\n", gpc(st));
            g_halt = 1; g_exit = 1; break;
        }
    }
#endif
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (i >= max_insns) { fprintf(stderr, "purv-sqlite: instruction cap reached\n"); g_exit = 3; }

    if (stats) {
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        /* machine-parseable line for benchmark.sh, plus a human note */
        fprintf(stderr, "BENCH wall_ms=%.0f insns=%llu mips=%.1f\n",
                ms, (unsigned long long)i, ms > 0 ? i / (ms * 1000.0) : 0.0);
    }

#ifdef PURV_TAGGED
    RiscvEmulatorDestroy(st);
#endif
    return g_exit;
}
