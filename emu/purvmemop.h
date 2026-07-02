/*
 * purvmemop.h - the custom-0 bulk memory/string instruction, evaluated once.
 *
 * The guest encoding (.insn r 0x0b, funct3, 0x0, rd, rs1, rs2 -- emitted by
 * the sqlite rt.c bodies) packs one libc call into one instruction.
 * rd is READ as the first argument; the scan/compare subtypes then WRITE the
 * result back into rd (memcpy/memset leave it alone -- it already holds the
 * dst they return). funct3 selects:
 *
 *   0 memcpy   dst=x[rd]      src=x[rs1]  n=x[rs2]   (overlap-safe: memmove)
 *   1 memset   dst=x[rd]      c=x[rs1]    n=x[rs2]
 *   2 memcmp   a=x[rd]->res   b=x[rs1]    n=x[rs2]
 *   3 memchr   s=x[rd]->res   c=x[rs1]    n=x[rs2]   (res: guest ptr or 0)
 *   4 strlen   s=x[rd]->res
 *   5 strcmp   a=x[rd]->res   b=x[rs1]
 *   6 strchr   s=x[rd]->res   c=x[rs1]                (res: guest ptr or 0)
 *   7 strncmp  a=x[rd]->res   b=x[rs1]    n=x[rs2]
 *
 * The compares return the rt.c value exactly (*x - *y at the first mismatch,
 * not just its sign) so a custom-op guest and a plain-loop guest compute
 * identical results everywhere.
 *
 * Shared by BOTH evaluations of the instruction: purva runs it as its own
 * opcode (RISCV_OP_MEMOP, purva.c); purv/purvs service it from the ILLEGAL
 * hook (sqlite/host.c) and resume. Only the public purv.h surface is used
 * (the two regions + the memory callback), so it works against any engine.
 *
 * Ranges are resolved against the regions once and scans stay inside the
 * region their pointer lands in (a guest object never spans regions). A
 * memcpy/memset whose range misses falls back to bytewise callback routing;
 * a scan whose pointer misses reads as an immediate terminator/mismatch --
 * the guest is trusted (it is our own rt.c), this just keeps a bad pointer
 * from touching host memory.
 *
 * The return value is the FUEL charge for the LOOP the instruction replaced
 * (the instruction itself is counted by the caller): n/4 word-copy iterations
 * for the move group, one per byte examined for the scans. purva adds it to
 * its budget; the purv host ignores it (purv already counted the instruction).
 */
#ifndef PURVMEMOP_H_
#define PURVMEMOP_H_

#include <stdint.h>
#include <string.h>

#ifndef RISCV_HALF
#error "include the engine header (purv.h) before purvmemop.h"
#endif

/* Resolve guest address `a` to a host pointer and the bytes available from it
 * to the end of its region (writable first, then read-only). 0 avail = miss. */
static inline const uint8_t *pmo_span(const RiscvEmulatorState_t *s, uint32_t a, uint32_t *avail) {
    uint32_t rel = a - s->writable.base;
    if (rel < s->writable.len) { *avail = s->writable.len - rel; return s->writable.ptr + rel; }
    rel = a - s->readonly.base;
    if (rel < s->readonly.len) { *avail = s->readonly.len - rel; return s->readonly.ptr + rel; }
    *avail = 0;
    return 0;
}
static inline uint8_t *pmo_wspan(const RiscvEmulatorState_t *s, uint32_t a, uint32_t *avail) {
    uint32_t rel = a - s->writable.base;
    if (rel < s->writable.len) { *avail = s->writable.len - rel; return s->writable.ptr + rel; }
    *avail = 0;
    return 0;
}

/* Execute one memop. Fields are pre-extracted (the two callers hold different
 * encodings: purva a packed op word, the purv host the raw instruction). */
static inline uint64_t purv_memop(RiscvEmulatorState_t *s, uint32_t f3,
                                  uint32_t rd, uint32_t rs1, uint32_t rs2) {
    uint32_t A = s->x[rd], B = s->x[rs1], N = s->x[rs2];
    uint32_t avail, i;

    switch (f3) {
    case 0: {                                                    /* memcpy */
        uint32_t da; uint8_t *d = pmo_wspan(s, A, &da);
        uint32_t sa; const uint8_t *q = pmo_span(s, B, &sa);
        if (d && q && N <= da && N <= sa) memmove(d, q, N);
        else for (i = 0; i < N; i++) {                           /* miss: bytewise, overlap-aware */
            uint32_t j = A > B ? N - 1 - i : i, b1, b2;
            const uint8_t *q1 = pmo_span(s, B + j, &b1);
            uint32_t v = q1 ? *q1 : s->callback(s, RISCV_MEM_LOAD, B + j, 0);
            uint8_t *q2 = pmo_wspan(s, A + j, &b2);
            if (q2) *q2 = (uint8_t)v; else s->callback(s, RISCV_MEM_STORE, A + j, v);
        }
        return N >> 2;
    }
    case 1: {                                                    /* memset */
        uint32_t da; uint8_t *d = pmo_wspan(s, A, &da);
        if (d && N <= da) memset(d, (int)B, N);
        else for (i = 0; i < N; i++) {
            uint32_t b2; uint8_t *q2 = pmo_wspan(s, A + i, &b2);
            if (q2) *q2 = (uint8_t)B; else s->callback(s, RISCV_MEM_STORE, A + i, B);
        }
        return N >> 2;
    }
    case 2: {                                                    /* memcmp */
        uint32_t aa; const uint8_t *x = pmo_span(s, A, &aa);
        uint32_t ba; const uint8_t *y = pmo_span(s, B, &ba);
        uint32_t n = N; int32_t res = 0;
        if (n > aa) n = aa;
        if (n > ba) n = ba;
        for (i = 0; i < n; i++)
            if (x[i] != y[i]) { res = (int32_t)x[i] - (int32_t)y[i]; break; }
        s->x[rd] = (uint32_t)res;
        return i < n ? i + 1 : n;
    }
    case 3: {                                                    /* memchr */
        const uint8_t *x = pmo_span(s, A, &avail);
        uint32_t n = N < avail ? N : avail;
        const uint8_t *hit = x ? memchr(x, (int)B, n) : 0;
        s->x[rd] = hit ? A + (uint32_t)(hit - x) : 0;
        return hit ? (uint32_t)(hit - x) + 1 : n;
    }
    case 4: {                                                    /* strlen */
        const uint8_t *x = pmo_span(s, A, &avail);
        const uint8_t *end = x ? memchr(x, 0, avail) : 0;
        uint32_t len = end ? (uint32_t)(end - x) : avail;
        s->x[rd] = len;
        return len + 1;
    }
    case 5: case 7: {                                            /* strcmp / strncmp */
        uint32_t aa; const uint8_t *x = pmo_span(s, A, &aa);
        uint32_t ba; const uint8_t *y = pmo_span(s, B, &ba);
        uint32_t n = f3 == 7 ? N : 0xffffffffu;
        if (n > aa) n = aa;
        if (n > ba) n = ba;
        int32_t res = 0;
        for (i = 0; i < n; i++) {
            if (x[i] != y[i] || x[i] == 0) { res = (int32_t)x[i] - (int32_t)y[i]; break; }
        }
        s->x[rd] = (uint32_t)res;
        return i < n ? i + 1 : n;
    }
    case 6: {                                                    /* strchr */
        const uint8_t *x = pmo_span(s, A, &avail);
        uint32_t res = 0;
        for (i = 0; i < avail; i++) {
            if (x[i] == (uint8_t)B) { res = A + i; break; }
            if (x[i] == 0) break;
        }
        s->x[rd] = res;
        return i < avail ? i + 1 : avail;
    }
    }
    return 0;
}

#endif /* PURVMEMOP_H_ */
