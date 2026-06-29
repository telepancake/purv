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

/* Exactly which of these the compiler emits varies by version, so provide the
 * whole comparison + conversion set (and multiply). They are never *executed* by
 * an integer/text workload -- SQLite only reaches its double code when parsing or
 * formatting a REAL -- so they only need to link.
 *
 * SOFTFP = optnone: without it the optimizer recognizes the integer bit-tricks
 * below as float ops and lowers them right back into __eqdf2/__nedf2/... calls
 * (which then fail to link, or recurse). optnone keeps them as plain integer code. */
#if defined(__clang__)
#  define SOFTFP __attribute__((optnone, noinline))
#else
#  define SOFTFP __attribute__((optimize("O0"), noinline))
#endif

typedef union { double d; uint64_t u; } DU;
static uint64_t d2u(double d)   { DU x; x.d = d; return x.u; }
static double   u2d(uint64_t u) { DU x; x.u = u; return x.d; }

static SOFTFP int dcmp(double a, double b) {                 /* -1 / 0 / +1 */
    uint64_t ua = d2u(a), ub = d2u(b);
    if (((ua << 1) == 0) && ((ub << 1) == 0)) return 0;      /* +0 == -0 */
    int sa = (int)(ua >> 63), sb = (int)(ub >> 63);
    if (sa != sb) return sa ? -1 : 1;
    if (ua == ub) return 0;
    int mag = ua < ub ? -1 : 1;
    return sa ? -mag : mag;
}
static SOFTFP int dnan(double a) {
    uint64_t u = d2u(a);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u & (((uint64_t)1 << 52) - 1)) != 0;
}

/* Comparisons (libgcc ABI: caller tests the sign/zeroness of the result). */
SOFTFP int __eqdf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __nedf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __ltdf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __ledf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __gtdf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __gedf2(double a, double b)    { return dcmp(a, b); }
SOFTFP int __unorddf2(double a, double b) { return dnan(a) || dnan(b); }

/* unsigned/signed integer -> double. */
SOFTFP double __floatundidf(uint64_t a) {
    if (a == 0) return u2d(0);
    int e = 63;
    while (!(a & ((uint64_t)1 << 63))) { a <<= 1; e--; }     /* normalize MSB -> bit 63 */
    uint64_t frac = (a >> 11) & (((uint64_t)1 << 52) - 1);
    return u2d(((uint64_t)(e + 1023) << 52) | frac);
}
SOFTFP double __floatdidf(int64_t a) {
    if (a == 0) return u2d(0);
    int neg = a < 0;
    uint64_t m = neg ? (uint64_t)(-(uint64_t)a) : (uint64_t)a; /* |a|, incl. INT64_MIN */
    uint64_t bits = d2u(__floatundidf(m));
    return u2d(bits | ((uint64_t)neg << 63));
}
SOFTFP double __floatsidf(int32_t a)    { return __floatdidf(a); }
SOFTFP double __floatunsidf(uint32_t a) { return __floatundidf(a); }

/* double -> unsigned/signed integer (truncating). */
SOFTFP uint64_t __fixunsdfdi(double a) {
    uint64_t bits = d2u(a);
    if (bits >> 63) return 0;                                /* negative -> 0 */
    int e = (int)((bits >> 52) & 0x7ff) - 1023;
    if (e < 0) return 0;
    uint64_t m = (bits & (((uint64_t)1 << 52) - 1)) | ((uint64_t)1 << 52);
    return e >= 52 ? (m << (e - 52)) : (m >> (52 - e));
}
SOFTFP int64_t __fixdfdi(double a) {
    uint64_t bits = d2u(a);
    int64_t v = (int64_t)__fixunsdfdi(u2d(bits & ~((uint64_t)1 << 63)));  /* of |a| */
    return (bits >> 63) ? -v : v;
}
SOFTFP int      __fixdfsi(double a)     { return (int)__fixdfdi(a); }
SOFTFP unsigned __fixunsdfsi(double a)  { return (unsigned)__fixunsdfdi(a); }

SOFTFP double __muldf3(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    int sign = (int)((ua ^ ub) >> 63);
    int ea = (int)((ua >> 52) & 0x7ff), eb = (int)((ub >> 52) & 0x7ff);
    uint64_t ma = (ua & (((uint64_t)1 << 52) - 1)), mb = (ub & (((uint64_t)1 << 52) - 1));
    if (ea == 0 && ma == 0) return u2d((uint64_t)sign << 63);  /* a == 0 */
    if (eb == 0 && mb == 0) return u2d((uint64_t)sign << 63);  /* b == 0 */
    ma |= (uint64_t)1 << 52; mb |= (uint64_t)1 << 52;          /* implicit 1 */
    int e = ea + eb - 1023;
    uint64_t al = ma & 0xffffffff, ah = ma >> 32;             /* 53x53 -> 106 bits */
    uint64_t bl = mb & 0xffffffff, bh = mb >> 32;
    uint64_t lo = al * bl;
    uint64_t mid = ah * bl + al * bh + (lo >> 32);
    uint64_t hi = ah * bh + (mid >> 32);
    uint64_t mant = hi >> (105 - 52 - 32);                    /* top 53 bits */
    if (mant & ((uint64_t)1 << 53)) { mant >>= 1; e++; }
    uint64_t frac = (mant >> 1) & (((uint64_t)1 << 52) - 1);
    if (e <= 0) return u2d((uint64_t)sign << 63);                                 /* underflow */
    if (e >= 0x7ff) return u2d(((uint64_t)sign << 63) | ((uint64_t)0x7ff << 52)); /* inf */
    return u2d(((uint64_t)sign << 63) | ((uint64_t)e << 52) | frac);
}
