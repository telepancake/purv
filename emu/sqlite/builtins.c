/*
 * builtins.c - compiler runtime the riscv32 codegen needs but -nostdlib omits.
 *
 * None of this is a host dependency: it is all pure computation that on a target
 * with a full toolchain would come from compiler-rt/libgcc. We provide it here so
 * the guest stays freestanding.
 *   - 64-bit integer divide/modulo (rv32 has only 32-bit div in hardware)
 *   - __atomic_* for byte/half/word (single-core, single-thread: plain accesses)
 *   - a tiny soft-double shim: SQLite's text-formatting path compiles a little
 *     `double` code even under SQLITE_OMIT_FLOATING_POINT; an integer/text
 *     workload never executes it, but it must link.
 */
#include <stdint.h>

/* ---------------------------------------------------- 64-bit divide/modulo */

static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod(n, d, &r); return r; }
int64_t __divdi3(int64_t a, int64_t b) {
    int neg = (a < 0) ^ (b < 0);
    uint64_t q = udivmod(a < 0 ? -(uint64_t)a : (uint64_t)a, b < 0 ? -(uint64_t)b : (uint64_t)b, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t a, int64_t b) {
    uint64_t r;
    udivmod(a < 0 ? -(uint64_t)a : (uint64_t)a, b < 0 ? -(uint64_t)b : (uint64_t)b, &r);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}

/* --------------------------------------------------------------- atomics */

unsigned char  __atomic_load_1(const volatile void *p, int m) { (void)m; return *(const volatile unsigned char *)p; }
unsigned short __atomic_load_2(const volatile void *p, int m) { (void)m; return *(const volatile unsigned short *)p; }
unsigned int   __atomic_load_4(const volatile void *p, int m) { (void)m; return *(const volatile unsigned int *)p; }
void __atomic_store_1(volatile void *p, unsigned char v,  int m) { (void)m; *(volatile unsigned char *)p = v; }
void __atomic_store_2(volatile void *p, unsigned short v, int m) { (void)m; *(volatile unsigned short *)p = v; }
void __atomic_store_4(volatile void *p, unsigned int v,   int m) { (void)m; *(volatile unsigned int *)p = v; }

/* --------------------------------------------------------- soft double shim */

typedef union { double d; uint64_t u; } DU;
static double  mkd(uint64_t u) { DU x; x.u = u; return x.d; }
static uint64_t ub(double d)   { DU x; x.d = d; return x.u; }

/* magnitude/sign compare; NaN not handled (the formatting path never feeds it). */
static int dcmp(double a, double b) {
    uint64_t ua = ub(a), ub_ = ub(b);
    if (((ua << 1) == 0) && ((ub_ << 1) == 0)) return 0;     /* +0 == -0 */
    int sa = (int)(ua >> 63), sb = (int)(ub_ >> 63);
    if (sa != sb) return sa ? -1 : 1;
    if (ua == ub_) return 0;
    int mag = ua < ub_ ? -1 : 1;
    return sa ? -mag : mag;
}
int __ltdf2(double a, double b) { return dcmp(a, b); }
int __gtdf2(double a, double b) { return dcmp(a, b); }
int __gedf2(double a, double b) { return dcmp(a, b); }

double __floatdidf(int64_t a) {
    if (a == 0) return mkd(0);
    int neg = a < 0;
    uint64_t m = neg ? -(uint64_t)a : (uint64_t)a;
    int e = 63;
    while (!(m & ((uint64_t)1 << 63))) { m <<= 1; e--; }     /* normalize MSB -> bit 63 */
    uint64_t frac = (m >> 11) & (((uint64_t)1 << 52) - 1);   /* 52 fraction bits */
    uint64_t bits = ((uint64_t)neg << 63) | ((uint64_t)(e + 1023) << 52) | frac;
    return mkd(bits);
}
int64_t __fixdfdi(double a) {
    uint64_t bits = ub(a);
    int neg = (int)(bits >> 63);
    int e = (int)((bits >> 52) & 0x7ff) - 1023;
    if (e < 0) return 0;
    uint64_t mant = (bits & (((uint64_t)1 << 52) - 1)) | ((uint64_t)1 << 52);
    int64_t v = e >= 52 ? (int64_t)(mant << (e - 52)) : (int64_t)(mant >> (52 - e));
    return neg ? -v : v;
}
int __fixdfsi(double a) { return (int)__fixdfdi(a); }

double __muldf3(double a, double b) {
    uint64_t ua = ub(a), ub_ = ub(b);
    int sign = (int)((ua ^ ub_) >> 63);
    int ea = (int)((ua >> 52) & 0x7ff), eb = (int)((ub_ >> 52) & 0x7ff);
    uint64_t ma = (ua & (((uint64_t)1 << 52) - 1)), mb = (ub_ & (((uint64_t)1 << 52) - 1));
    if (ea == 0 && ma == 0) return mkd((uint64_t)sign << 63);  /* a == 0 */
    if (eb == 0 && mb == 0) return mkd((uint64_t)sign << 63);  /* b == 0 */
    ma |= (uint64_t)1 << 52; mb |= (uint64_t)1 << 52;          /* implicit 1 */
    int e = ea + eb - 1023;
    /* 53x53 -> 106-bit product via 32-bit halves */
    uint64_t al = ma & 0xffffffff, ah = ma >> 32;
    uint64_t bl = mb & 0xffffffff, bh = mb >> 32;
    uint64_t lo = al * bl;
    uint64_t mid = ah * bl + al * bh + (lo >> 32);
    uint64_t hi = ah * bh + (mid >> 32);
    uint64_t prod_hi = hi;                                     /* top ~64 bits of product */
    /* product is in [2^104, 2^106); we want top 53 bits */
    int shift = 105 - 52;                                      /* bring bit105/104 down to 52 */
    uint64_t mant = prod_hi >> (shift - 32);
    if (mant & ((uint64_t)1 << 53)) { mant >>= 1; e++; }       /* normalize 106-bit case */
    uint64_t frac = (mant >> 1) & (((uint64_t)1 << 52) - 1);
    if (e <= 0) return mkd((uint64_t)sign << 63);              /* underflow -> 0 (shim) */
    if (e >= 0x7ff) return mkd(((uint64_t)sign << 63) | ((uint64_t)0x7ff << 52)); /* inf */
    return mkd(((uint64_t)sign << 63) | ((uint64_t)e << 52) | frac);
}
