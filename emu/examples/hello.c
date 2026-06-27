/* Freestanding RV32 userspace program: prints via the write syscall and exits.
 * Run with: purv --user hello.elf   (see emu/README "Running userspace code"). */
static long sys(long n, long a0, long a1, long a2) {
    register long a7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(a7), "r"(x11), "r"(x12) : "memory");
    return x10;
}

void _start(void) {
    const char msg[] = "hello from riscv userspace\n";
    sys(64, 1, (long)msg, sizeof(msg) - 1);   /* write(1, msg, len) */
    sys(93, 0, 0, 0);                         /* exit(0)            */
    for (;;) {}
}
