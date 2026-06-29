/*
 * mandelbrot.c - a compact, freestanding, fixed-address RV32 program for purv.
 *
 * A non-trivial computation (the Mandelbrot set, rendered as ASCII) done in
 * Q16.16 *fixed point* -- the kind of thing you would normally write with
 * `double`, here in pure integers so the compiler emits NO soft-float and NO
 * 64-bit divide. The whole program is one translation unit and uses only the
 * shared host-call ABI: memory (malloc/free), output (write), exit.
 *
 * Built -Os, RVC, --gc-sections and stripped, laid out by link.ld so the ELF is
 * about as small as is reasonable (headers folded into the single load segment).
 * The result reaches the host through exactly the same ecalls as emu/sqlite, but
 * pulls in essentially none of the compiler runtime that SQLite needed.
 */
#include <stdint.h>
#include "hostcalls.h"        /* shared host-call numbers (see emu/sqlite) */

static inline long hostcall(long n, long a0, long a1, long a2) {
    register long x17 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x17), "r"(x11), "r"(x12) : "memory");
    return x10;
}
static void  out(const char *s, long n) { hostcall(HOSTCALL_WRITE, 1, (long)s, n); }
static void  puts_(const char *s) { long n = 0; while (s[n]) n++; out(s, n); }
static void *xmalloc(long n)      { return (void *)hostcall(HOSTCALL_MALLOC, n, 0, 0); }
static void  xfree(void *p)       { hostcall(HOSTCALL_FREE, (long)p, 0, 0); }

/* Q16.16 fixed point. The only "math library" we need: a fixed-point multiply.
 * (int32 * int32 -> int64) is two RV32 instructions (mul/mulh, inlined -- not a
 * libcall), and the >>16 is inlined too. No __muldf3, no __divdi3. */
#define SH  16
#define W   78        /* columns */
#define H   38        /* rows    */
#define MAX 90        /* iteration cap */

static inline int32_t fxmul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> SH);
}

int cmain(void) {
    static const char ramp[] = " .:-=+*#%@";          /* light -> dense */
    const int32_t X0 = -163840, X1 = 65536;           /* -2.5 .. 1.0 in Q16.16 */
    const int32_t Y0 =  -75366, Y1 = 75366;           /* -1.15 .. 1.15         */
    int32_t dx = (X1 - X0) / W, dy = (Y1 - Y0) / H;   /* 32-bit divide = hardware */

    char *buf = (char *)xmalloc(W + 1);               /* one row, via the memory ecall */
    if (!buf) return 1;

    puts_("fixed-point Mandelbrot (Q16.16, no floats) on purv\n");
    for (int r = 0; r < H; r++) {
        int32_t cy = Y0 + (int32_t)r * dy;
        for (int c = 0; c < W; c++) {
            int32_t cx = X0 + (int32_t)c * dx, zx = 0, zy = 0;
            int it;
            for (it = 0; it < MAX; it++) {
                int32_t zx2 = fxmul(zx, zx), zy2 = fxmul(zy, zy);
                if (zx2 + zy2 > (4 << SH)) break;     /* |z|^2 > 4 -> escaped */
                zy = (fxmul(zx, zy) << 1) + cy;       /* 2*zx*zy + cy */
                zx = zx2 - zy2 + cx;
            }
            buf[c] = it >= MAX ? '@' : ramp[it * 10 / MAX];
        }
        buf[W] = '\n';
        out(buf, W + 1);
    }

    xfree(buf);
    return 0;
}

/* Entry: the host sets sp; call cmain, then exit(a0) via the host-call ABI. */
extern int cmain(void);
__asm__(
    ".globl _start\n"
    "_start:\n"
    "  call cmain\n"
    "  mv a7, zero\n"        /* HOSTCALL_EXIT */
    "  ecall\n");
