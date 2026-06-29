/*
 * hostcalls.h - the entire ABI between the freestanding guest and the host.
 *
 * The guest issues `ecall` with a host-function number in a7 and arguments in
 * a0..a2; the host services it and returns a result in a0. These few numbers
 * are the ONLY thing the guest depends on from the outside world.
 */
#ifndef PURV_HOSTCALLS_H
#define PURV_HOSTCALLS_H

#define HOSTCALL_EXIT   0   /* a0 = exit code           -> does not return        */
#define HOSTCALL_WRITE  1   /* a0 = fd, a1 = buf, a2 = len -> bytes written        */

#endif
