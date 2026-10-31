#pragma once
#include <stddef.h>
#include <stdarg.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void *malloc(size_t n);
void free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t size);
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
double strtod(const char *s, char **end);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int atoi(const char *s);
long atol(const char *s);
int abs(int x);
long labs(long x);
char *getenv(const char *name);
int system(const char *cmd);
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *));
