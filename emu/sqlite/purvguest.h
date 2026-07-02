/*
 * purvguest.h - the guest-side API, one include for C and C++.
 *
 * For code built with `pvcc ... --rt`: the runtime (rt.c, and rt-cxx for C++)
 * provides everything below plus the libc subset (<string.h>/<stdlib.h> work:
 * malloc and the mem/str functions are real -- single custom instructions
 * serviced by the engine). What a guest can do:
 *
 *   int cmain(void);                     you implement this; its return value
 *                                        is the exit code the host sees
 *   host_write(fd, buf, len)             bytes to the host's `write` callback
 *   host_exit(code)                      end the run early
 *   purv_hostcall(fn, a0, a1, a2)        call a host hook (fn >= 16 -- see
 *                                        purvarun.h's PurvaCallbacks.hostcall)
 *
 * There is deliberately nothing else: no files, no clock, no environment --
 * anything of the sort is a hostcall you define on both sides.
 */
#ifndef PURVGUEST_H_
#define PURVGUEST_H_

#ifdef __cplusplus
extern "C" {
#endif

long host_write(int fd, const void *buf, long len);
void host_exit(int code);

int cmain(void);                         /* the guest entry point (you write it) */

/* Reach a host-defined hook: a7 = fn, args in a0..a2, result back in a0. */
static inline long purv_hostcall(long fn, long a0, long a1, long a2) {
    register long x17 __asm__("a7") = fn;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x17), "r"(x11), "r"(x12) : "memory");
    return x10;
}

#ifdef __cplusplus
}
#endif

#endif /* PURVGUEST_H_ */
