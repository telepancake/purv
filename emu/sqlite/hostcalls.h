/*
 * hostcalls.h - the entire ABI between the freestanding guest and the host.
 *
 * The guest issues `ecall` with a host-function number in a7 and arguments in
 * a0..a2; the host services it and returns a result in a0. These few numbers
 * are the ONLY thing the guest depends on from the outside world.
 */
#ifndef PURV_HOSTCALLS_H
#define PURV_HOSTCALLS_H

#define HOSTCALL_EXIT    0  /* a0 = code               -> does not return          */
#define HOSTCALL_WRITE   1  /* a0 = fd, a1 = buf, a2 = len -> bytes written         */
/* The memory group: the heap is a host service, so the guest carries no
 * allocator and reserves no static heap. Addresses are guest RAM addresses. */
#define HOSTCALL_MALLOC  2  /* a0 = size               -> ptr (0 on failure)        */
#define HOSTCALL_FREE    3  /* a0 = ptr                -> 0                          */
#define HOSTCALL_REALLOC 4  /* a0 = ptr, a1 = size     -> ptr (0 on failure)        */

#endif
