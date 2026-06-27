/* Freestanding RV32 program with a loop that writes memory, for exercising the
 * gdb stub's reverse execution (registers and memory both unwound).
 *   make examples/loop.elf && ./gdbserve.py 1234 -- ./purv --user examples/loop.elf */
static long sys(long n, long a0, long a1, long a2) {
    register long a7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(a7), "r"(x11), "r"(x12) : "memory");
    return x10;
}

static volatile int total;
static volatile int arr[8];

void _start(void) {
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        arr[i] = i * i;        /* memory write each iteration */
        sum += arr[i];
    }
    total = sum;               /* 0+1+4+9+16+25+36+49 = 140 */
    sys(93, total, 0, 0);      /* exit(140) */
    for (;;) {}
}

__asm__(".global _start");
