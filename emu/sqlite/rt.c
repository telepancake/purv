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

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
char *strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
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

/* ------------------------------------------------------------ malloc (K&R) */

#define HEAP_SIZE (48u * 1024 * 1024)
static unsigned char g_heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t g_heap_off;

typedef long Align;
typedef union header {
    struct { union header *ptr; size_t size; } s;   /* size in header units */
    Align x;
} Header;

static Header  base;
static Header *freep;

static Header *morecore(size_t nu) {
    if (nu < 4096) nu = 4096;
    size_t bytes = nu * sizeof(Header);
    if (g_heap_off + bytes > HEAP_SIZE) return NULL;
    Header *up = (Header *)(g_heap + g_heap_off);
    g_heap_off += bytes;
    up->s.size = nu;
    void free_internal(void *);
    free_internal((void *)(up + 1));
    return freep;
}

void *malloc(size_t nbytes) {
    Header *p, *prevp;
    size_t nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
    if ((prevp = freep) == NULL) { base.s.ptr = freep = prevp = &base; base.s.size = 0; }
    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) {
            if (p->s.size == nunits) prevp->s.ptr = p->s.ptr;
            else { p->s.size -= nunits; p += p->s.size; p->s.size = nunits; }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep) if ((p = morecore(nunits)) == NULL) return NULL;
    }
}

void free_internal(void *ap) {
    Header *bp = (Header *)ap - 1, *p;
    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr)) break;
    if (bp + bp->s.size == p->s.ptr) { bp->s.size += p->s.ptr->s.size; bp->s.ptr = p->s.ptr->s.ptr; }
    else bp->s.ptr = p->s.ptr;
    if (p + p->s.size == bp) { p->s.size += bp->s.size; p->s.ptr = bp->s.ptr; }
    else p->s.ptr = bp;
    freep = p;
}
void free(void *ap) { if (ap) free_internal(ap); }

void *realloc(void *ptr, size_t n) {
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return NULL; }
    Header *h = (Header *)ptr - 1;
    size_t oldsz = (h->s.size - 1) * sizeof(Header);
    if (oldsz >= n) return ptr;
    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, ptr, oldsz < n ? oldsz : n);
    free(ptr);
    return np;
}
void *calloc(size_t a, size_t b) {
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
