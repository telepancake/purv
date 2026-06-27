/* Freestanding RV32 codelet exercising purvs pointer safety.
 * Pick a case via argv[1]: ok | oob | uaf | cross.
 *   ok    - only in-bounds accesses (exits 0)
 *   oob   - write past the end of an allocation (caught)
 *   uaf   - write after free (caught)
 *   cross - mix two objects' pointers, then dereference (caught) */
static long sys(long n, long a, long b, long c) {
    register long a7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a;
    register long x11 __asm__("a1") = b;
    register long x12 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(a7), "r"(x11), "r"(x12) : "memory");
    return x10;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys(64, 1, (long)s, n); }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static long xmalloc(long n) { return sys(1000, n, 0, 0); }
static void xfree(long p)   { sys(1001, p, 0, 0); }

void cmain(unsigned long *sp) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];
    const char *mode = argc > 1 ? argv[1] : "ok";

    volatile char *p = (volatile char *)xmalloc(16);
    if (streq(mode, "ok")) {
        p[0] = 'A'; p[15] = 'Z';
        put("ok: in-bounds writes succeeded\n");
    } else if (streq(mode, "oob")) {
        put("oob: writing p[20] (past the 16-byte object)...\n");
        p[20] = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "uaf")) {
        xfree((long)p);
        put("uaf: writing through p after free...\n");
        p[0] = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "cross")) {
        volatile char *q = (volatile char *)xmalloc(16);
        unsigned long pi = (unsigned long)p, qi = (unsigned long)q;
        /* Build a pointer from two objects' bits: high bits of p, low bits of q.
         * The compiler can't fold this, so the tags genuinely mix -> bad tag. */
        volatile char *mixed = (volatile char *)((pi & 0xfffff000UL) | (qi & 0x00000fffUL));
        put("cross: writing through a pointer built from two objects...\n");
        *mixed = 'X';
        put("BUG: not caught\n");
    }
    sys(93, 0, 0, 0);
    for (;;) {}
}

__asm__(".global _start\n_start:\n\tmv a0, sp\n\tcall cmain\n");
