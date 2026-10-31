/*
 * Lantest OS - tiny user-space libc (stdio/string/mem, no host libc)
 */
#pragma once

#include "userlib.h"

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

#ifndef NULL
#define NULL ((void *)0)
#endif

extern int errno;

typedef u64 jmp_buf[8];

static __attribute__((unused, naked, returns_twice)) int
setjmp(jmp_buf env __attribute__((unused)))
{
    __asm__ volatile (
        "movq %rbx, 0(%rdi)\n"
        "movq %rbp, 8(%rdi)\n"
        "movq %r12, 16(%rdi)\n"
        "movq %r13, 24(%rdi)\n"
        "movq %r14, 32(%rdi)\n"
        "movq %r15, 40(%rdi)\n"
        "leaq 8(%rsp), %rax\n"
        "movq %rax, 48(%rdi)\n"
        "movq (%rsp), %rax\n"
        "movq %rax, 56(%rdi)\n"
        "xorl %eax, %eax\n"
        "ret\n"
    );
}

static __attribute__((unused, naked, noreturn)) void
longjmp(jmp_buf env __attribute__((unused)),
        int val __attribute__((unused)))
{
    __asm__ volatile (
        "movq 0(%rdi), %rbx\n"
        "movq 8(%rdi), %rbp\n"
        "movq 16(%rdi), %r12\n"
        "movq 24(%rdi), %r13\n"
        "movq 32(%rdi), %r14\n"
        "movq 40(%rdi), %r15\n"
        "movq 48(%rdi), %rsp\n"
        "movq 56(%rdi), %rdx\n"
        "movl %esi, %eax\n"
        "testl %eax, %eax\n"
        "jne 1f\n"
        "incl %eax\n"
        "1:\n"
        "jmp *%rdx\n"
    );
}

static __attribute__((unused)) u64 strlen(const char *s)
{
    u64 n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

static __attribute__((unused)) int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static __attribute__((unused)) int strncmp(const char *a, const char *b, u64 n)
{
    while (n > 0 && *a != '\0' && *a == *b) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static __attribute__((unused)) char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

static __attribute__((unused)) char *strncpy(char *dst, const char *src, u64 n)
{
    u64 i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    while (i < n) {
        dst[i] = '\0';
        ++i;
    }
    return dst;
}

static __attribute__((unused)) void *memcpy(void *dst, const void *src, u64 n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (u64 i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}

static __attribute__((unused)) void *memset(void *dst, int c, u64 n)
{
    unsigned char *d = (unsigned char *)dst;
    for (u64 i = 0; i < n; ++i)
        d[i] = (unsigned char)c;
    return dst;
}

static __attribute__((unused)) int memcmp(const void *a, const void *b, u64 n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (u64 i = 0; i < n; ++i) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

static __attribute__((unused)) int atoi(const char *s)
{
    int sign = 1;
    int v = 0;

    while (*s == ' ' || *s == '\t')
        ++s;
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return sign * v;
}

struct libc_out {
    char *buf;
    u64 cap;
    u64 len;
};

static __attribute__((unused)) void out_char(struct libc_out *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static __attribute__((unused)) void out_str(struct libc_out *o, const char *s)
{
    while (*s != '\0')
        out_char(o, *s++);
}

static __attribute__((unused)) void out_pad(struct libc_out *o, char c, int n)
{
    while (n-- > 0)
        out_char(o, c);
}

static __attribute__((unused)) void out_uint(struct libc_out *o, unsigned long v, unsigned int base,
                     int width, char pad)
{
    char tmp[32];
    int n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        unsigned int d = (unsigned int)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    out_pad(o, pad, width - n);
    while (n > 0)
        out_char(o, tmp[--n]);
}

static __attribute__((unused)) int vsnprintf(char *buf, u64 cap, const char *fmt, va_list ap)
{
    struct libc_out o;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;

    while (*fmt != '\0') {
        int width = 0;
        char pad = ' ';

        if (*fmt != '%') {
            out_char(&o, *fmt++);
            continue;
        }
        ++fmt;
        if (*fmt == '0') {
            pad = '0';
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            int n = (int)strlen(s);
            out_pad(&o, ' ', width - n);
            out_str(&o, s);
            break;
        }
        case 'c':
            out_char(&o, (char)va_arg(ap, int));
            break;
        case 'd':
            {
                int sv = va_arg(ap, int);
                unsigned int u;
                if (sv < 0) {
                    out_char(&o, '-');
                    u = (unsigned int)(-sv);
                } else {
                    u = (unsigned int)sv;
                }
                out_uint(&o, u, 10, width, pad);
            }
            break;
        case 'u':
            out_uint(&o, va_arg(ap, unsigned int), 10, width, pad);
            break;
        case 'x':
            out_uint(&o, va_arg(ap, unsigned int), 16, width, pad);
            break;
        case 'l':
            ++fmt;
            if (*fmt == 'd') {
                long lv = va_arg(ap, long);
                unsigned long u;
                if (lv < 0) {
                    out_char(&o, '-');
                    u = (unsigned long)(-lv);
                } else {
                    u = (unsigned long)lv;
                }
                out_uint(&o, u, 10, width, pad);
            } else if (*fmt == 'u') {
                out_uint(&o, va_arg(ap, unsigned long), 10, width, pad);
            } else if (*fmt == 'x') {
                out_uint(&o, va_arg(ap, unsigned long), 16, width, pad);
            } else {
                out_char(&o, 'l');
                out_char(&o, *fmt ? *fmt : '?');
            }
            break;
        case 'p':
            out_str(&o, "0x");
            out_uint(&o, (unsigned long)(u64)va_arg(ap, void *), 16,
                     width, '0');
            break;
        case '%':
            out_char(&o, '%');
            break;
        default:
            out_char(&o, '%');
            if (*fmt != '\0')
                out_char(&o, *fmt);
            break;
        }
        if (*fmt != '\0')
            ++fmt;
    }

    if (cap > 0) {
        u64 end = o.len < cap ? o.len : cap - 1;
        buf[end] = '\0';
    }
    return (int)o.len;
}

static __attribute__((unused)) int snprintf(char *buf, u64 cap,
                                              const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

static __attribute__((unused)) int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        u64 len = (u64)n;
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        sys_write(STDOUT_FD, buf, len);
    }
    return n;
}

static __attribute__((unused)) int puts(const char *s)
{
    sys_write_str(s);
    sys_write_str("\n");
    return 0;
}

static __attribute__((unused)) int putchar(int c)
{
    char ch = (char)c;
    sys_write(STDOUT_FD, &ch, 1);
    return c;
}
