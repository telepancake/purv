#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif
#undef assert
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
extern void __assert_fail_msg(const char *);
#define assert(x) ((x) ? (void)0 : __assert_fail_msg(#x))
#endif
#ifdef __cplusplus
}
#endif

#endif
