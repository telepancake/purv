/*
 * rt-cxx.cc - the freestanding C++ runtime for riscv32 guests, on top of rt.c.
 *
 * The guests build -fno-rtti -fno-exceptions -fno-threadsafe-statics (the same
 * freestanding-C++ flavour pvcc uses), so almost nothing is needed: operator
 * new/delete routed to the malloc-group host calls (via rt.c's malloc/free),
 * and the pure-virtual trap. No global constructors: the linked guests avoid
 * them (nothing runs .init_array), and the Makefile builds C++ sources with
 * -Werror=global-constructors so a violation fails the build, not the run.
 */
#include <stddef.h>

extern "C" {
void *malloc(size_t);
void free(void *);
long host_write(int fd, const void *buf, long len);
void host_exit(int code);
}

void *operator new(size_t n)             { return malloc(n ? n : 1); }
void *operator new[](size_t n)           { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept   { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept   { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual(void) {
    static const char msg[] = "pure virtual call\n";
    host_write(2, msg, sizeof msg - 1);
    host_exit(70);
}
