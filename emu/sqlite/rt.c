/*
 * rt.c - freestanding C runtime for the riscv32 guest.
 *
 * Everything the guest needs that is NOT a host call: program entry, the libc
 * subset SQLite links against (mem/str/ctype, malloc), and the ecall stubs that
 * reach the host. Compiled -ffreestanding -nostdlib -fno-builtin so the compiler
 * does not turn these very functions into calls to themselves.
 */
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "hostcalls.h"

/* ------------------------------------------------------------ host calls */

static inline long hostcall(long n, long a0, long a1, long a2) {
    register long x17 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x17), "r"(x11), "r"(x12) : "memory");
    return x10;
}

long host_write(int fd, const void *buf, long len) {
    return hostcall(HOSTCALL_WRITE, fd, (long)buf, len);
}
void host_exit(int code) {
    hostcall(HOSTCALL_EXIT, code, 0, 0);
    for (;;) {}
}

/* exit() from <stdlib.h> routes to the host. */
void exit(int code) { host_exit(code); }
char *getenv(const char *name) { (void)name; return NULL; }
void __assert_fail_msg(const char *m) { host_write(2, m, (long)__builtin_strlen(m)); host_exit(70); }

/* Program entry: the host sets sp and pc=_start. Call cmain, then exit. */
extern int cmain(void);
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  call cmain\n"
    "  li a7, 0\n"          /* HOSTCALL_EXIT */
    "  ecall\n"
    "1: j 1b\n");

/* ------------------------------------------------------------ mem / string */

#ifdef PURV_CUSTOM_MEMOPS
/* purva-only build: memcpy/memset/memmove are each ONE custom instruction
 * (.insn r in the custom-0 opcode space -- see ../purva/transcode.h
 * RISCV_OP_MEMOP), evaluated as a host-side bulk copy. dst rides in the rd
 * slot (read by the evaluator), src/c in rs1, n in rs2. purv/purvs would trap
 * on it -- that engine lock-in is exactly the trade this option measures
 * against the portable word-wise loops below. */
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile(".insn r 0x0b, 0x0, 0x0, %0, %1, %2" : "+r"(r) : "r"(s), "r"(n) : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {   /* the evaluator copy is overlap-safe */
    void *r = d;
    __asm__ volatile(".insn r 0x0b, 0x0, 0x0, %0, %1, %2" : "+r"(r) : "r"(s), "r"(n) : "memory");
    return r;
}
void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile(".insn r 0x0b, 0x1, 0x0, %0, %1, %2" : "+r"(r) : "r"((long)c), "r"(n) : "memory");
    return r;
}
/* The scan/compare group: rd is the first argument in and the RESULT out
 * (purvmemop.h computes the exact rt.c return value, so a custom-op guest and
 * a plain-loop guest are indistinguishable to the program). */
int memcmp(const void *a, const void *b, size_t n) {
    long r = (long)a;
    __asm__ volatile(".insn r 0x0b, 0x2, 0x0, %0, %1, %2" : "+r"(r) : "r"(b), "r"(n));
    return (int)r;
}
void *memchr(const void *s, int c, size_t n) {
    long r = (long)s;
    __asm__ volatile(".insn r 0x0b, 0x3, 0x0, %0, %1, %2" : "+r"(r) : "r"((long)c), "r"(n));
    return (void *)r;
}
size_t strlen(const char *s) {
    long r = (long)s;
    __asm__ volatile(".insn r 0x0b, 0x4, 0x0, %0, %1, %2" : "+r"(r) : "r"(s), "r"(0L));
    return (size_t)r;
}
int strcmp(const char *a, const char *b) {
    long r = (long)a;
    __asm__ volatile(".insn r 0x0b, 0x5, 0x0, %0, %1, %2" : "+r"(r) : "r"(b), "r"(0L));
    return (int)r;
}
char *strchr(const char *s, int c) {
    long r = (long)s;
    __asm__ volatile(".insn r 0x0b, 0x6, 0x0, %0, %1, %2" : "+r"(r) : "r"((long)c), "r"(0L));
    return (char *)r;
}
int strncmp(const char *a, const char *b, size_t n) {
    long r = (long)a;
    __asm__ volatile(".insn r 0x0b, 0x7, 0x0, %0, %1, %2" : "+r"(r) : "r"(b), "r"(n));
    return (int)r;
}
#else
/* memcpy/memset/memmove are word-wise: on an emulated CPU every instruction is
 * ~a dispatch, so the byte loop pays 4x the accesses AND 4x the loop overhead --
 * profile.py showed these three's lbu/sb/addi/bne chains among the hottest
 * pairs in the sqlite bench. Same-alignment inputs (the overwhelming case) copy
 * a word per iteration after a byte head; differing alignment stays bytewise. */
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (n >= 8 && (((uintptr_t)p ^ (uintptr_t)q) & 3) == 0) {
        while ((uintptr_t)p & 3) { *p++ = *q++; n--; }
        uint32_t *pw = (uint32_t *)p; const uint32_t *qw = (const uint32_t *)q;
        for (; n >= 16; n -= 16) { pw[0] = qw[0]; pw[1] = qw[1]; pw[2] = qw[2]; pw[3] = qw[3]; pw += 4; qw += 4; }
        for (; n >= 4; n -= 4) *pw++ = *qw++;
        p = (unsigned char *)pw; q = (const unsigned char *)qw;
    }
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (p == q || n == 0) return d;
    if (p < q) return memcpy(d, s, n);                 /* forward copy is safe */
    p += n; q += n;
    if (n >= 8 && (((uintptr_t)p ^ (uintptr_t)q) & 3) == 0) {   /* backward, word-wise */
        while ((uintptr_t)p & 3) { *--p = *--q; n--; }
        uint32_t *pw = (uint32_t *)p; const uint32_t *qw = (const uint32_t *)q;
        for (; n >= 4; n -= 4) *--pw = *--qw;
        p = (unsigned char *)pw; q = (const unsigned char *)qw;
    }
    while (n--) *--p = *--q;
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    if (n >= 8) {
        uint32_t v = (unsigned char)c * 0x01010101u;
        while ((uintptr_t)p & 3) { *p++ = (unsigned char)c; n--; }
        uint32_t *pw = (uint32_t *)p;
        for (; n >= 16; n -= 16) { pw[0] = v; pw[1] = v; pw[2] = v; pw[3] = v; pw += 4; }
        for (; n >= 4; n -= 4) *pw++ = v;
        p = (unsigned char *)pw;
    }
    while (n--) *p++ = (unsigned char)c;
    return d;
}
#endif /* PURV_CUSTOM_MEMOPS */
#ifndef PURV_CUSTOM_MEMOPS
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
#endif

#ifndef PURV_CUSTOM_MEMOPS
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}
#endif

#ifndef PURV_CUSTOM_MEMOPS
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
#endif

#ifndef PURV_CUSTOM_MEMOPS
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
#endif

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
#ifndef PURV_CUSTOM_MEMOPS
char *strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
#endif

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) return (char *)last; }
}
size_t strspn(const char *s, const char *set) {
    const char *p = s;
    for (; *p; p++) { const char *q = set; while (*q && *q != *p) q++; if (!*q) break; }
    return (size_t)(p - s);
}
size_t strcspn(const char *s, const char *set) {
    const char *p = s;
    for (; *p; p++) { const char *q = set; while (*q && *q != *p) q++; if (*q) break; }
    return (size_t)(p - s);
}
char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}

/* ------------------------------------------------------------ ctype */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }

int abs(int x) { return x < 0 ? -x : x; }
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
    return sign * v;
}
long strtol(const char *s, char **end, int base) {
    long v = 0; int sign = 1;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    if (base == 0) base = 10;
    for (;;) {
        int d, c = (unsigned char)*s;
        if (isdigit(c)) d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (end) *end = (char *)s;
    return sign * v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }
long long strtoll(const char *s, char **end, int base) { return strtol(s, end, base); }

/* ------------------------------------------------------------ date stubs */
/* Datetime functions are omitted (SQLITE_OMIT_DATETIME_FUNCS); these only need
 * to link, never to run. */
time_t time(time_t *t) { if (t) *t = 0; return 0; }
struct tm *gmtime(const time_t *t) { (void)t; static struct tm z; return &z; }
struct tm *localtime(const time_t *t) { return gmtime(t); }
size_t strftime(char *s, size_t n, const char *f, const struct tm *tm) {
    (void)f; (void)tm; if (n) s[0] = 0; return 0;
}

/* ------------------------------------------------------------ malloc (host) */
/* The heap is a host service (the malloc group of host calls). The guest keeps
 * no allocator and no static heap: memory is requested on demand, like write. */
void *malloc(size_t n)            { return (void *)hostcall(HOSTCALL_MALLOC, (long)n, 0, 0); }
void  free(void *p)               { hostcall(HOSTCALL_FREE, (long)p, 0, 0); }
void *realloc(void *p, size_t n)  { return (void *)hostcall(HOSTCALL_REALLOC, (long)p, (long)n, 0); }
void *calloc(size_t a, size_t b)  {
    size_t n = a * b;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* ------------------------------------------------------------ qsort (shell) */

static void qswap(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base_, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    char *a = base_;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap; j -= gap) {
                char *x = a + (j - gap) * sz, *y = a + j * sz;
                if (cmp(x, y) <= 0) break;
                qswap(x, y, sz);
            }
}
