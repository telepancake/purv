#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void  free(void *);
void *realloc(void *, size_t);
void *calloc(size_t, size_t);
void  qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int   abs(int);
long  strtol(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int   atoi(const char *);
void  exit(int);
char *getenv(const char *);
#define RAND_MAX 0x7fffffff
#endif
