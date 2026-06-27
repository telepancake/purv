/* Freestanding RV32 codelet exercising purvs pointer safety. Pick a case via
 * argv[1]:
 *   ok      - only in-bounds accesses (exits 0)
 *   diff    - same-object pointer difference used as a count (exits 0)
 *   align   - p & ~15 keeps the tag (exits 0)
 *   oob     - write past the end of an allocation (caught)
 *   uaf     - write after free (caught)
 *   subdiff - (b - a) across objects -> bad scalar -> caught
 *   addp    - ptr + ptr -> bad -> caught
 *   rsub    - int - ptr -> bad -> caught
 *   scale   - shifted pointer -> bad -> caught
 *   cross   - pointer built from two objects -> bad -> caught
 *   xdata   - call into a data buffer -> non-executable fetch -> caught
 *   wcode   - write into the code segment -> W^X -> caught */
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
    } else if (streq(mode, "align")) {
        volatile char *a = (volatile char *)((unsigned long)p & ~(unsigned long)15);
        a[0] = 'A';                                /* p & ~15: alignment keeps the tag */
        put("align: (p & ~15) keeps the tag; in-bounds write ok\n");
    } else if (streq(mode, "oob")) {
        put("oob: writing p[20] (past the 16-byte object)...\n");
        p[20] = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "uaf")) {
        xfree((long)p);
        put("uaf: writing through p after free...\n");
        p[0] = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "diff")) {
        volatile char *a = (volatile char *)xmalloc(16);
        volatile char *e = a + 12;                 /* same object */
        long n = (char *)e - (char *)a;            /* same tag -> plain number (NOTAG) */
        for (long i = 0; i < n; i++) a[i] = (char)i;
        put("diff: same-object difference is a plain count; in-bounds loop ok\n");
    } else if (streq(mode, "subdiff")) {
        volatile char *a = (volatile char *)xmalloc(16);
        volatile char *b = (volatile char *)xmalloc(16);  /* different object */
        long d = (char *)b - (char *)a;            /* different tags -> bad scalar */
        volatile char *c = (volatile char *)xmalloc(16);
        put("subdiff: using (b - a) as an index into c...\n");
        c[d & 15] = 'X';                           /* bad provenance poisons the access */
        put("BUG: not caught\n");
    } else if (streq(mode, "addp")) {
        volatile char *q = (volatile char *)xmalloc(16);
        unsigned long s = (unsigned long)p + (unsigned long)q;  /* ptr + ptr -> bad */
        put("addp: dereferencing (p + q)...\n");
        *(volatile char *)s = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "rsub")) {
        unsigned long k = 0x80003000UL;                         /* a plain integer */
        unsigned long s = k - (unsigned long)p;                 /* int - ptr -> bad */
        put("rsub: dereferencing (K - p)...\n");
        *(volatile char *)s = 'X';
        put("BUG: not caught\n");
    } else if (streq(mode, "scale")) {
        volatile char *s = (volatile char *)((unsigned long)p << 1); /* shift, not offset */
        put("scale: dereferencing a shifted pointer...\n");
        *s = 'X';                                  /* shift poisons the tag -> caught */
        put("BUG: not caught\n");
    } else if (streq(mode, "xdata")) {
        volatile unsigned *buf = (volatile unsigned *)xmalloc(16);
        buf[0] = 0x00008067u;                          /* a 'ret', planted as data */
        put("xdata: calling into a data buffer...\n");
        ((void (*)(void))(unsigned long)buf)();        /* execute data -> fetch not code */
        put("BUG: not caught\n");
    } else if (streq(mode, "wcode")) {
        volatile unsigned *code = (volatile unsigned *)(unsigned long)&put;
        put("wcode: writing into the code segment...\n");
        *code = 0;                                     /* store into code -> W^X */
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
