/*
 * embed-app.c - the guest half of the embedding demo (see embed.c, purvarun.h).
 *
 * Built with `pvcc embed-app.c --rt -o embed-app.img`: the --rt runtime brings
 * _start (which calls cmain), the libc subset (malloc via the hostcall ABI,
 * single-instruction mem/str ops), and host_write/host_exit. A guest can also
 * define its OWN hostcalls -- anything >= 16 reaches the host's `hostcall`
 * callback -- as a three-line ecall stub, like `roll` below.
 */
#include <string.h>
#include <stdlib.h>
#include "purvguest.h"

/* a user hostcall: fn 16, one argument, result in a0 -- the host decides what
 * it means (embed.c: "roll a die with n sides") */
static long roll(long sides) { return purv_hostcall(16, sides, 0, 0); }

static void put(const char *s) { host_write(1, s, (long)strlen(s)); }
static void putnum(long v) {
    char b[12]; int i = 11; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(b + i);
}

static long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int cmain(void) {
    put("guest: hello from inside the image\n");

    char *heap = malloc(64);                 /* the heap is a host service */
    memset(heap, '.', 63); heap[63] = 0;
    put("guest: malloc'd and memset: "); put(heap); put("\n");
    free(heap);

    put("guest: fib(24) = "); putnum(fib(24)); put("\n");
    put("guest: the host rolled "); putnum(roll(6)); put(" on a d6\n");
    return 7;                                /* becomes the exit code the host sees */
}
