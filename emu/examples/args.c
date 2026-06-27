/* Freestanding RV32 userspace program: prints argc and each argv, proving the
 * host sets up a real initial stack. Run: purv --user args.elf hello world */
static long sys(long n, long a, long b, long c) {
    register long a7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a;
    register long x11 __asm__("a1") = b;
    register long x12 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(a7), "r"(x11), "r"(x12) : "memory");
    return x10;
}

static void put(const char *s) {
    long n = 0;
    while (s[n]) n++;
    sys(64, 1, (long)s, n);                /* write(1, s, len) */
}

void cmain(unsigned long *sp) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];
    char d[2] = {(char)('0' + (argc % 10)), '\n'};
    put("argc=");
    sys(64, 1, (long)d, 2);
    for (long i = 0; i < argc; i++) {
        put(argv[i]);
        put("\n");
    }
    sys(93, 0, 0, 0);                      /* exit(0) */
    for (;;) {}
}

__asm__(".global _start\n_start:\n\tmv a0, sp\n\tcall cmain\n");
