/*
 * hostcalls.h - the host-call ABI for the compact demo (self-contained).
 *
 * Guest does `ecall` with the function number in a7 and arguments in a0..a2;
 * the host services it and returns the result in a0. This example needs only
 * four: exit, write, and a tiny heap (malloc/free).
 */
#ifndef COMPACT_HOSTCALLS_H
#define COMPACT_HOSTCALLS_H

#define HOSTCALL_EXIT   0   /* a0 = code                 -> does not return       */
#define HOSTCALL_WRITE  1   /* a0 = fd, a1 = buf, a2 = len -> bytes written        */
#define HOSTCALL_MALLOC 2   /* a0 = size                 -> ptr (0 on failure)     */
#define HOSTCALL_FREE   3   /* a0 = ptr                  -> 0                       */

#endif
