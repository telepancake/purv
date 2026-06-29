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


/* ----------------------------------------------- no floating point in this build */
/* SQLite's number parse/format path compiles a little real-`double` code even
 * under SQLITE_OMIT_FLOATING_POINT: the printf "%f" branch (sqlite3FpDecode /
 * dekkerMul2), the float scanner (sqlite3AtoF), and value<->REAL coercion. That
 * code is woven into core functions, so it cannot be cleanly removed -- but an
 * integer/text workload never *executes* it. Rather than carry a soft-float
 * implementation, we trap: these symbols exist only to link, and if a REAL is
 * ever actually parsed or formatted the build stops loudly (illegal instruction)
 * instead of silently computing it. Which exact helpers the compiler emits varies
 * by version, hence the full set. */
#define NOFP(sig) sig { __builtin_trap(); }

NOFP(double   __adddf3(double a, double b))
NOFP(double   __subdf3(double a, double b))
NOFP(double   __muldf3(double a, double b))
NOFP(double   __divdf3(double a, double b))
NOFP(double   __negdf2(double a))
NOFP(int      __eqdf2(double a, double b))
NOFP(int      __nedf2(double a, double b))
NOFP(int      __ltdf2(double a, double b))
NOFP(int      __ledf2(double a, double b))
NOFP(int      __gtdf2(double a, double b))
NOFP(int      __gedf2(double a, double b))
NOFP(int      __cmpdf2(double a, double b))
NOFP(int      __unorddf2(double a, double b))
NOFP(double   __floatsidf(int a))
NOFP(double   __floatunsidf(unsigned a))
NOFP(double   __floatdidf(long long a))
NOFP(double   __floatundidf(unsigned long long a))
NOFP(int      __fixdfsi(double a))
NOFP(unsigned __fixunsdfsi(double a))
NOFP(long long __fixdfdi(double a))
NOFP(unsigned long long __fixunsdfdi(double a))
NOFP(double   __extendsfdf2(float a))
NOFP(float    __truncdfsf2(double a))
