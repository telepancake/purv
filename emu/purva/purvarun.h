/*
 * purvarun.h - embed a purva image in a host program, one header.
 *
 * This is the packaged shape of the whole pipeline: build the guest with pvcc
 * (C/C++/asm -> compact .img, `pvcc app.c --rt -o app.img`), then in the host:
 *
 *     PurvaRun r;
 *     PurvaCallbacks cb = { .write = my_write, .user = my_ctx };
 *     purva_run_init(&r, image_bytes, image_len, 0, 0, &cb);   // 0: default sizes
 *     for (;;) {
 *         int st = purva_run(&r, 1000000, NULL);      // run a 1M-instruction slice
 *         if (st == PURVA_RUN_FUEL) { do host things; continue; }
 *         break;                                      // DONE (r.exit_code) or TRAP
 *     }
 *     purva_run_free(&r);
 *
 * The image is self-contained (packed ops + rodata + rwdata, see image.h); the
 * guest's entire view of the outside world is the hostcall ABI serviced here:
 *
 *     exit(code) / write(fd,buf,len)      -> the `write` callback (fd 1/2)
 *     malloc/free/realloc                 -> serviced internally: a K&R first-fit
 *                                            allocator over the guest's own RAM
 *                                            (the guest carries no allocator)
 *     anything else (a7 >= PURVA_HOSTCALL_USER) -> the `hostcall` callback
 *
 * plus the custom-0 bulk mem/str instruction (purvmemop.h), which the engine
 * evaluates natively. Fuel is semi-accurate guest-instruction counting: fused
 * ops charge what they replaced (a prologue counts its stores, a memcpy op
 * counts ~n/4), so a fuel budget means "about this many RV32 instructions".
 * purva_run may overshoot a budget by one straight-line run (budget checks sit
 * at jumps) and never splits a hostcall or a bulk op.
 *
 * Multiple PurvaRun instances may coexist; the engine's program pointer is
 * re-armed on every purva_run call. Everything here is plain portable C11 --
 * same 32/64-bit host support as the engine itself.
 */
#ifndef PURVARUN_H_
#define PURVARUN_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "purva.h"      /* purv.h (state, regions, Loop) + transcode.h + SetProgram */

/* The guest<->host call numbers (a7; args a0..a2, result a0). Must match the
 * guest runtime's hostcalls.h -- the values are the ABI. */
#define PURVA_HOSTCALL_EXIT    0
#define PURVA_HOSTCALL_WRITE   1
#define PURVA_HOSTCALL_MALLOC  2
#define PURVA_HOSTCALL_FREE    3
#define PURVA_HOSTCALL_REALLOC 4
#define PURVA_HOSTCALL_USER    16   /* first number routed to cb->hostcall */

typedef struct {
    /* write(fd, buf, len) -> bytes written. NULL: output is dropped. */
    long (*write)(void *user, int fd, const void *buf, unsigned long len);
    /* service a guest hostcall with a7 >= PURVA_HOSTCALL_USER. Return the a0
     * result; set *stop nonzero to end the run (purva_run returns TRAP).
     * NULL: unknown hostcalls stop the run. */
    uint32_t (*hostcall)(void *user, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, int *stop);
    void *user;
} PurvaCallbacks;

enum {
    PURVA_RUN_DONE = 0,     /* guest called exit -- see run->exit_code */
    PURVA_RUN_FUEL = 1,     /* fuel exhausted; call purva_run again to continue */
    PURVA_RUN_TRAP = 2,     /* illegal op / bad fetch / stopped hostcall */
};

typedef struct {
    RiscvEmulatorState_t st;      /* MUST stay first: trap hooks recover the
                                   * PurvaRun from the state pointer by cast. */
    Transcoded prog;
    uint8_t  *code, *rodata, *writable;   /* owned copies / buffers */
    uint32_t  rodata_len;
    uint32_t  stack_len, heap_len;        /* writable = stack then heap (purv.h layout) */
    PurvaCallbacks cb;
    int  status, exit_code, pending;      /* pending: ecall flag set by the hook */
    /* the K&R allocator over guest RAM: block headers live in guest memory,
     * these are guest addresses (see host-purva.c, where this scheme grew up) */
    uint32_t heap_brk, heap_top, freep, basep;
} PurvaRun;

/* ---- guest heap (the malloc group): K&R first-fit over the guest arena ---- */
#define PURVA_HUNIT 8u
static inline uint8_t *purva__arena(PurvaRun *r, uint32_t a) {   /* guest addr -> host ptr */
    return r->writable + (a - (RISCV_HALF - r->stack_len));
}
static inline uint32_t purva__ld(PurvaRun *r, uint32_t a) { uint32_t v; memcpy(&v, purva__arena(r, a), 4); return v; }
static inline void purva__st(PurvaRun *r, uint32_t a, uint32_t v) { memcpy(purva__arena(r, a), &v, 4); }
#define purva__next(r, a)        purva__ld(r, a)
#define purva__size(r, a)        purva__ld(r, (a) + 4)
#define purva__set_next(r, a, v) purva__st(r, a, v)
#define purva__set_size(r, a, v) purva__st(r, (a) + 4, v)

static inline void purva__free(PurvaRun *r, uint32_t ap) {
    uint32_t bp = ap - PURVA_HUNIT, p;
    for (p = r->freep; !(bp > p && bp < purva__next(r, p)); p = purva__next(r, p))
        if (p >= purva__next(r, p) && (bp > p || bp < purva__next(r, p))) break;
    if (bp + purva__size(r, bp) * PURVA_HUNIT == purva__next(r, p)) {
        purva__set_size(r, bp, purva__size(r, bp) + purva__size(r, purva__next(r, p)));
        purva__set_next(r, bp, purva__next(r, purva__next(r, p)));
    } else purva__set_next(r, bp, purva__next(r, p));
    if (p + purva__size(r, p) * PURVA_HUNIT == bp) {
        purva__set_size(r, p, purva__size(r, p) + purva__size(r, bp));
        purva__set_next(r, p, purva__next(r, bp));
    } else purva__set_next(r, p, bp);
    r->freep = p;
}
static inline uint32_t purva__morecore(PurvaRun *r, uint32_t nu) {
    if (nu < 4096) nu = 4096;
    uint32_t bytes = nu * PURVA_HUNIT;
    if ((uint64_t)r->heap_brk + bytes > r->heap_top) return 0;
    uint32_t up = r->heap_brk; r->heap_brk += bytes;
    purva__set_size(r, up, nu);
    purva__free(r, up + PURVA_HUNIT);
    return r->freep;
}
static inline uint32_t purva__malloc(PurvaRun *r, uint32_t nbytes) {
    if (nbytes == 0) return 0;
    uint32_t nunits = (nbytes + PURVA_HUNIT - 1) / PURVA_HUNIT + 1;
    uint32_t prevp = r->freep, p;
    for (p = purva__next(r, prevp); ; prevp = p, p = purva__next(r, p)) {
        if (purva__size(r, p) >= nunits) {
            if (purva__size(r, p) == nunits) purva__set_next(r, prevp, purva__next(r, p));
            else { purva__set_size(r, p, purva__size(r, p) - nunits);
                   p += purva__size(r, p) * PURVA_HUNIT; purva__set_size(r, p, nunits); }
            r->freep = prevp;
            return p + PURVA_HUNIT;
        }
        if (p == r->freep && (p = purva__morecore(r, nunits)) == 0) return 0;
    }
}
static inline uint32_t purva__realloc(PurvaRun *r, uint32_t ap, uint32_t n) {
    if (ap == 0) return purva__malloc(r, n);
    if (n == 0) { purva__free(r, ap); return 0; }
    uint32_t oldbytes = (purva__size(r, ap - PURVA_HUNIT) - 1) * PURVA_HUNIT;
    if (oldbytes >= n) return ap;
    uint32_t np = purva__malloc(r, n);
    if (!np) return 0;
    memmove(purva__arena(r, np), purva__arena(r, ap), oldbytes < n ? oldbytes : n);
    purva__free(r, ap);
    return np;
}

/* ---- engine hooks (the state is the first member, so cast back) ---- */
static inline uint8_t purva__gbyte(const RiscvEmulatorState_t *s, uint32_t a) {
    uint32_t rel = a - s->writable.base;
    if (rel < s->writable.len) return s->writable.ptr[rel];
    rel = a - s->readonly.base;
    if (rel < s->readonly.len) return s->readonly.ptr[rel];
    return 0;
}
static inline int purva__on_ecall(RiscvEmulatorState_t *st) {
    PurvaRun *r = (PurvaRun *)st;
    uint32_t fn = st->x[17], a0 = st->x[10], a1 = st->x[11], a2 = st->x[12], ret = 0;
    switch (fn) {
    case PURVA_HOSTCALL_EXIT:
        r->exit_code = (int)a0; r->status = PURVA_RUN_DONE;
        st->x[10] = 0;
        return 1;
    case PURVA_HOSTCALL_WRITE:
        if (r->cb.write) {
            /* the buffer may span regions in theory; copy through gbyte in chunks */
            char buf[512]; uint32_t left = a2, p = a1; long done = 0;
            while (left) {
                uint32_t c = left > sizeof buf ? (uint32_t)sizeof buf : left;
                for (uint32_t i = 0; i < c; i++) buf[i] = (char)purva__gbyte(st, p + i);
                long w = r->cb.write(r->cb.user, (int)a0, buf, c);
                if (w <= 0) break;
                done += w; p += (uint32_t)w; left -= (uint32_t)w;
            }
            ret = (uint32_t)done;
        } else ret = a2;                                  /* dropped, but "written" */
        break;
    case PURVA_HOSTCALL_MALLOC:  ret = purva__malloc(r, a0); break;
    case PURVA_HOSTCALL_FREE:    if (a0) purva__free(r, a0); break;
    case PURVA_HOSTCALL_REALLOC: ret = purva__realloc(r, a0, a1); break;
    default: {
        int stop = 0;
        if (fn >= PURVA_HOSTCALL_USER && r->cb.hostcall)
            ret = r->cb.hostcall(r->cb.user, fn, a0, a1, a2, &stop);
        else { r->status = PURVA_RUN_TRAP; return 1; }
        if (stop) { r->status = PURVA_RUN_TRAP; st->x[10] = ret; return 1; }
        break;
    }
    }
    st->x[10] = ret;
    return 0;
}
static inline int purva__on_trap(RiscvEmulatorState_t *st) {
    ((PurvaRun *)st)->status = PURVA_RUN_TRAP;
    return 1;
}

/* ---- the API ---- */

static inline void purva_run_free(PurvaRun *r);

/* Load an in-memory image (the bytes of a .img file) and set up the guest.
 * ram_bytes / stack_bytes of 0 pick defaults (16 MiB / 1 MiB) -- both are
 * grown to the image's declared minimums. Returns 0, or -1 (bad image / OOM). */
static inline int purva_run_init(PurvaRun *r, const void *image, size_t image_len,
                                 uint32_t ram_bytes, uint32_t stack_bytes,
                                 const PurvaCallbacks *cb) {
    memset(r, 0, sizeof *r);
    if (cb) r->cb = *cb;
    const uint8_t *b = (const uint8_t *)image;
    if (image_len < 20) return -1;
    uint32_t hdr[5];                              /* code, rodata, rwdata, bss, stack (image.h) */
    memcpy(hdr, b, 20);
    uint64_t need = 20ull + hdr[0] + hdr[1] + hdr[2];
    if (hdr[0] % 4 || need > image_len) return -1;
    if (ram_bytes == 0) ram_bytes = 16u * 1024 * 1024;
    if (stack_bytes == 0) stack_bytes = 1024u * 1024;
    if (stack_bytes < hdr[4]) stack_bytes = hdr[4];
    uint32_t heap_len = hdr[2] + hdr[3] + ram_bytes;

    r->code = (uint8_t *)malloc(hdr[0] ? hdr[0] : 1);
    r->rodata = (uint8_t *)malloc(hdr[1] ? hdr[1] : 1);
    r->writable = (uint8_t *)calloc(1, (size_t)stack_bytes + heap_len);
    if (!r->code || !r->rodata || !r->writable) { purva_run_free(r); return -1; }
    memcpy(r->code, b + 20, hdr[0]);
    memcpy(r->rodata, b + 20 + hdr[0], hdr[1]);
    memcpy(r->writable + stack_bytes, b + 20 + hdr[0] + hdr[1], hdr[2]);   /* rwdata at RISCV_HALF */
    r->rodata_len = hdr[1]; r->stack_len = stack_bytes; r->heap_len = heap_len;
    r->prog = (Transcoded){ (uint32_t *)r->code, hdr[0] / 4, hdr[0] };

    RiscvEmulatorRegion_t ro = { r->rodata, hdr[1], 0u - hdr[1] };          /* rodata below 0 */
    RiscvEmulatorRegion_t rw = { r->writable, stack_bytes + heap_len, RISCV_HALF - stack_bytes };
    RiscvEmulatorInit(&r->st, ro, rw);
    r->st.ecall = purva__on_ecall;
    r->st.ebreak = purva__on_trap;
    r->st.illegal = purva__on_trap;
    r->st.pc = 0;                                                          /* image entry is 0 */

    uint32_t base = (RISCV_HALF + hdr[2] + hdr[3] + 15u) & ~15u;           /* arena after data+bss */
    r->basep = base; purva__set_next(r, base, base); purva__set_size(r, base, 0);
    r->freep = base; r->heap_brk = base + PURVA_HUNIT;
    r->heap_top = RISCV_HALF + heap_len;
    r->status = PURVA_RUN_FUEL;                                            /* i.e. runnable */
    return 0;
}

/* Run up to `fuel` guest instructions (may overshoot by one straight-line run).
 * Returns PURVA_RUN_DONE / _FUEL / _TRAP; *used (optional) gets the count. */
static inline int purva_run(PurvaRun *r, uint64_t fuel, uint64_t *used) {
    uint64_t total = 0;
    if (r->status != PURVA_RUN_FUEL) { if (used) *used = 0; return r->status; }
    RiscvEmulatorSetProgram(&r->prog);            /* re-arm: instances may interleave */
    while (total < fuel) {
        uint64_t ran = RiscvEmulatorLoop(&r->st, fuel - total);
        total += ran;
        if (r->status != PURVA_RUN_FUEL) break;                  /* exit or trap */
        if (r->st.pc >= r->prog.code_len) { r->status = PURVA_RUN_TRAP; break; }
        if (ran == 0) { r->status = PURVA_RUN_TRAP; break; }     /* wedged */
    }
    if (used) *used = total;
    return r->status;
}

static inline void purva_run_free(PurvaRun *r) {
    free(r->code); free(r->rodata); free(r->writable);
    r->code = r->rodata = r->writable = 0;
}

#endif /* PURVARUN_H_ */
