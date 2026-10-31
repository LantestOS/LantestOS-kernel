/*
 * Lantest OS - Lua port libc (multi-TU, external symbols)
 */
#include "userlib.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <locale.h>

int errno;

/* ------------------------------ allocator ------------------------------- */
struct mem_hdr {
    size_t size;
    struct mem_hdr *next;
    int free;
};

#define MEM_ALIGN 16UL
#define MEM_CHUNK 65536UL
static struct mem_hdr *mem_free;

static size_t mem_align(size_t n)
{
    return (n + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
}

void *malloc(size_t n)
{
    struct mem_hdr *h;
    size_t need = mem_align(n);

    for (h = mem_free; h != NULL; h = h->next) {
        if (h->free && h->size >= need) {
            if (h->size >= need + sizeof(struct mem_hdr) + MEM_ALIGN) {
                struct mem_hdr *rest =
                    (struct mem_hdr *)((unsigned char *)(h + 1) + need);
                rest->size = h->size - need - sizeof(struct mem_hdr);
                rest->free = 1;
                rest->next = mem_free;
                mem_free = rest;
                h->size = need;
            }
            h->free = 0;
            return (void *)(h + 1);
        }
    }

    {
        i64 raw = sys_mmap(MEM_CHUNK);
        if (raw < 0) {
            errno = ENOMEM;
            return NULL;
        }
        h = (struct mem_hdr *)(u64)raw;
        h->size = MEM_CHUNK - sizeof(struct mem_hdr);
        h->free = 1;
        h->next = mem_free;
        mem_free = h;
        return malloc(n);
    }
}

void free(void *p)
{
    struct mem_hdr *h;

    if (p == NULL)
        return;
    h = (struct mem_hdr *)p - 1;
    h->free = 1;
}

void *realloc(void *p, size_t n)
{
    struct mem_hdr *h;
    void *np;
    size_t copy;

    if (p == NULL)
        return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    h = (struct mem_hdr *)p - 1;
    if (h->size >= n)
        return p;
    np = malloc(n);
    if (np == NULL)
        return NULL;
    copy = h->size < n ? h->size : n;
    memcpy(np, p, copy);
    free(p);
    return np;
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p;

    if (size != 0 && total / size != n)
        return NULL;
    p = malloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

void exit(int status)
{
    sys_exit(status);
    for (;;)
        ;
}

void abort(void)
{
    sys_exit(134);
    for (;;)
        ;
}

/* ------------------------------- strings -------------------------------- */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        for (size_t i = 0; i < n; ++i)
            d[i] = s[i];
    } else {
        while (n > 0) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < n; ++i)
        d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
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

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != '\0')
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] != '\0') {
        d[i] = s[i];
        ++i;
    }
    while (i < n)
        d[i++] = '\0';
    return d;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strchr(const char *s, int c)
{
    while (*s != '\0') {
        if (*s == (char)c)
            return (char *)s;
        ++s;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s != '\0') {
        if (*s == (char)c)
            last = s;
        ++s;
    }
    return c == 0 ? (char *)s : (char *)last;
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (nl == 0)
        return (char *)h;
    for (; *h != '\0'; ++h) {
        if (*h == *n && strncmp(h, n, nl) == 0)
            return (char *)h;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s != '\0'; ++s) {
        if (strchr(accept, *s) != NULL)
            return (char *)s;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] != '\0' && strchr(accept, s[n]) != NULL)
        ++n;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] != '\0' && strchr(reject, s[n]) == NULL)
        ++n;
    return n;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

char *strerror(int e)
{
    (void)e;
    return "error";
}

/* ------------------------------ formatting ------------------------------- */
struct outbuf {
    char *buf;
    size_t cap;
    size_t len;
};

static void out_ch(struct outbuf *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static void out_s(struct outbuf *o, const char *s)
{
    while (*s != '\0')
        out_ch(o, *s++);
}

static void out_pad(struct outbuf *o, char c, int n)
{
    while (n-- > 0)
        out_ch(o, c);
}

static void out_uint(struct outbuf *o, unsigned long v, unsigned int base,
                     int width, int prec, char pad)
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
    if (prec > n)
        out_pad(o, '0', prec - n);
    out_pad(o, pad, width - n);
    while (n > 0)
        out_ch(o, tmp[--n]);
}

static void out_double(struct outbuf *o, double v, int width, int prec,
                       int is_g)
{
    char tmp[64];
    int n = 0;
    int neg = 0;
    unsigned long ip;
    double frac;

    if (v < 0) {
        neg = 1;
        v = -v;
    }
    ip = (unsigned long)v;
    frac = v - (double)ip;

    if (neg)
        out_ch(o, '-');

    {
        char ib[24];
        int m = 0;
        if (ip == 0)
            ib[m++] = '0';
        while (ip > 0) {
            ib[m++] = (char)('0' + ip % 10);
            ip /= 10;
        }
        while (m > 0)
            tmp[n++] = ib[--m];
    }

    if (prec > 0) {
        tmp[n++] = '.';
        for (int i = 0; i < prec; ++i) {
            frac *= 10.0;
            int d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            tmp[n++] = (char)('0' + d);
            frac -= (double)d;
        }
        if (is_g) {
            while (n > 0 && tmp[n - 1] == '0')
                --n;
            if (n > 0 && tmp[n - 1] == '.')
                --n;
        }
    }

    out_pad(o, ' ', width - n);
    for (int i = 0; i < n; ++i)
        out_ch(o, tmp[i]);
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct outbuf o = { buf, cap, 0 };

    while (*fmt != '\0') {
        int width = 0;
        int prec = -1;
        char pad = ' ';

        if (*fmt != '%') {
            out_ch(&o, *fmt++);
            continue;
        }
        ++fmt;
        if (*fmt == '0') {
            pad = '0';
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            ++fmt;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) s = "(null)";
            int n = (int)strlen(s);
            if (prec >= 0 && n > prec) n = prec;
            out_pad(&o, ' ', width - n);
            for (int i = 0; i < n; ++i) out_ch(&o, s[i]);
            break;
        }
        case 'c': out_ch(&o, (char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { out_ch(&o, '-'); out_uint(&o, (unsigned int)(-v), 10, width, prec < 0 ? 0 : prec, pad); }
            else out_uint(&o, (unsigned int)v, 10, width, prec < 0 ? 0 : prec, pad);
            break;
        }
        case 'u': out_uint(&o, va_arg(ap, unsigned int), 10, width, prec < 0 ? 0 : prec, pad); break;
        case 'x': out_uint(&o, va_arg(ap, unsigned int), 16, width, prec < 0 ? 0 : prec, pad); break;
        case 'p':
            out_s(&o, "0x");
            out_uint(&o, (unsigned long)(u64)va_arg(ap, void *), 16, width, 0, '0');
            break;
        case 'l':
            ++fmt;
            if (*fmt == 'd') {
                long v = va_arg(ap, long);
                if (v < 0) { out_ch(&o, '-'); out_uint(&o, (unsigned long)(-v), 10, width, prec < 0 ? 0 : prec, pad); }
                else out_uint(&o, (unsigned long)v, 10, width, prec < 0 ? 0 : prec, pad);
            } else if (*fmt == 'u') out_uint(&o, va_arg(ap, unsigned long), 10, width, prec < 0 ? 0 : prec, pad);
            else if (*fmt == 'x') out_uint(&o, va_arg(ap, unsigned long), 16, width, prec < 0 ? 0 : prec, pad);
            else { out_ch(&o, 'l'); out_ch(&o, *fmt); }
            break;
        case 'f': out_double(&o, va_arg(ap, double), width, prec < 0 ? 6 : prec, 0); break;
        case 'g': out_double(&o, va_arg(ap, double), width, prec < 0 ? 6 : prec, 1); break;
        case '%': out_ch(&o, '%'); break;
        default:
            out_ch(&o, '%');
            if (*fmt != '\0') out_ch(&o, *fmt);
            break;
        }
        if (*fmt != '\0') ++fmt;
    }
    if (cap > 0) {
        size_t end = o.len < cap ? o.len : cap - 1;
        buf[end] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, 65536, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        sys_write(STDOUT_FD, buf, n < 511 ? (u64)n : 511);
    return n;
}

/* -------------------------------- FILE ---------------------------------- */
struct _FILE {
    int fd;
    int writable;
    int eof;
    int err;
};

static struct _FILE std_in = { 0, 0, 0, 0 };
static struct _FILE std_out = { 1, 1, 0, 0 };
static struct _FILE std_err = { 2, 1, 0, 0 };
FILE *stdin = &std_in;
FILE *stdout = &std_out;
FILE *stderr = &std_err;

FILE *fopen(const char *path, const char *mode)
{
    unsigned int flags;
    int fd;
    FILE *f;

    if (path == NULL || mode == NULL) return NULL;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0) flags = O_WRONLY | O_CREAT;
    else if (strcmp(mode, "r+") == 0) flags = O_RDWR;
    else if (strcmp(mode, "w+") == 0) flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a+") == 0) flags = O_RDWR | O_CREAT;
    else return NULL;

    fd = (int)sys_open(path, flags);
    if (fd < 0) { errno = ENOENT; return NULL; }
    f = malloc(sizeof(FILE));
    if (f == NULL) { sys_close(fd); return NULL; }
    f->fd = fd;
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    f->eof = 0;
    f->err = 0;
    return f;
}

int fclose(FILE *f)
{
    int rc;
    if (f == NULL || f == stdin || f == stdout || f == stderr)
        return EOF;
    rc = (int)sys_close(f->fd);
    free(f);
    return rc;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total, done = 0;
    if (f == NULL || size == 0 || nmemb == 0) return 0;
    total = size * nmemb;
    while (done < total) {
        i64 n = sys_read(f->fd, (unsigned char *)ptr + done, total - done);
        if (n <= 0) { f->eof = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total, done = 0;
    if (f == NULL || size == 0 || nmemb == 0) return 0;
    total = size * nmemb;
    while (done < total) {
        i64 n = sys_write(f->fd, (const unsigned char *)ptr + done, total - done);
        if (n <= 0) { f->err = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}

int fseek(FILE *f, long off, int whence)
{
    if (f == NULL) return -1;
    return (int)sys_lseek(f->fd, off, whence);
}

long ftell(FILE *f)
{
    if (f == NULL) return -1;
    return (long)sys_lseek(f->fd, 0, SEEK_CUR);
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

int fgetc(FILE *f)
{
    unsigned char c;
    if (f == NULL) return EOF;
    if (sys_read(f->fd, &c, 1) != 1) { f->eof = 1; return EOF; }
    return (int)c;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (f != NULL && f != stdin && f != stdout && f != stderr)
        fclose(f);
    return fopen(path, mode);
}

int ungetc(int c, FILE *f)
{
    (void)c; (void)f;
    return EOF;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    if (f == NULL) return EOF;
    if (sys_write(f->fd, &ch, 1) != 1) { f->err = 1; return EOF; }
    return (int)ch;
}

char *fgets(char *buf, int size, FILE *f)
{
    int i = 0;
    if (buf == NULL || size <= 0 || f == NULL) return NULL;
    while (i + 1 < size) {
        int c = fgetc(f);
        if (c < 0) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int fputs(const char *s, FILE *f)
{
    if (s == NULL || f == NULL) return EOF;
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(buf, 1, n < 511 ? (size_t)n : 511, f);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) fwrite(buf, 1, n < 511 ? (size_t)n : 511, f);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int feof(FILE *f) { return f != NULL && f->eof; }
int ferror(FILE *f) { return f != NULL && f->err; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->err = 0; } }
int fileno(FILE *f) { return f != NULL ? f->fd : -1; }
int setvbuf(FILE *f, char *buf, int mode, size_t size) { (void)f; (void)buf; (void)mode; (void)size; return 0; }
char *tmpnam(char *s) { (void)s; return NULL; }
FILE *tmpfile(void) { return NULL; }
FILE *popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; return NULL; }
int pclose(FILE *f) { (void)f; return -1; }
int remove(const char *path) { return (int)sys_unlink(path); }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

/* ----------------------------- conversion ------------------------------- */
double strtod(const char *s, char **end)
{
    double v = 0.0;
    double scale = 0.1;
    int sign = 1;
    const char *p = s;

    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (*p == '-') { sign = -1; ++p; }
    else if (*p == '+') ++p;

    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); ++p; }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            v += (*p - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }
    if (*p == 'e' || *p == 'E') {
        int es = 1, e = 0;
        ++p;
        if (*p == '-') { es = -1; ++p; }
        else if (*p == '+') ++p;
        while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); ++p; }
        while (e-- > 0) v = es > 0 ? v * 10.0 : v / 10.0;
    }
    if (end) *end = (char *)p;
    return sign * v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    const char *p = s;
    if (base == 0) base = 10;
    while (*p == ' ' || *p == '\t') ++p;
    if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        ++p;
    }
    if (end) *end = (char *)p;
    return v;
}

long strtol(const char *s, char **end, int base)
{
    int sign = 1;
    const char *p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-') { sign = -1; ++p; }
    else if (*p == '+') ++p;
    unsigned long v = strtoul(p, end, base);
    return sign * (long)v;
}

long long strtoll(const char *s, char **end, int base)
{
    return (long long)strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    return (unsigned long long)strtoul(s, end, base);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *))
{
    unsigned char *a = (unsigned char *)base;
    unsigned char tmp[256];

    for (size_t i = 1; i < n; ++i) {
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0) {
            if (size <= sizeof(tmp)) {
                memcpy(tmp, a + (j - 1) * size, size);
                memcpy(a + (j - 1) * size, a + j * size, size);
                memcpy(a + j * size, tmp, size);
            }
            --j;
        }
    }
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *cmd)
{
    (void)cmd;
    return -1;
}

/* -------------------------------- ctype --------------------------------- */
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return c >= 0 && c < 32; }
int ispunct(int c) { return c >= 33 && c <= 126 && !isalnum(c); }
int isprint(int c) { return c >= 32 && c <= 126; }
int isgraph(int c) { return c >= 33 && c <= 126; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* -------------------------------- math ---------------------------------- */
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) { long i = (long)x; return (double)(x < (double)i ? i - 1 : i); }
double ceil(double x) { long i = (long)x; return (double)(x > (double)i ? i + 1 : i); }
double fmod(double x, double y) { return x - floor(x / y) * y; }

double sqrt(double x)
{
    double r;
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double sin(double x)
{
    double r;
    __asm__ volatile ("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double cos(double x)
{
    double r;
    __asm__ volatile ("fldl %1\n\tfcos\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double tan(double x)
{
    double r;
    __asm__ volatile ("fldl %1\n\tfptan\n\tfstpl %0\n\tfstp %%st(0)"
                      : "=m"(r) : "m"(x));
    return r;
}

double atan2(double y, double x)
{
    double r;
    __asm__ volatile ("fldl %1\n\tfldl %2\n\tfpatan\n\tfstpl %0"
                      : "=m"(r) : "m"(y), "m"(x));
    return r;
}

double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt(1.0 - x * x)); }
double acos(double x) { return atan2(sqrt(1.0 - x * x), x); }

double ldexp(double x, int e)
{
    if (e > 0) {
        while (e-- > 0) x *= 2.0;
    } else {
        while (e++ < 0) x /= 2.0;
    }
    return x;
}

double frexp(double x, int *e)
{
    int n = 0;
    double a = fabs(x);

    if (a == 0.0) { if (e) *e = 0; return x; }
    while (a >= 1.0) { a /= 2.0; ++n; }
    while (a < 0.5) { a *= 2.0; --n; }
    if (e) *e = n;
    return x < 0 ? -a : a;
}

double exp(double x)
{
    const double ln2 = 0.6931471805599453;
    double r, term, sum;
    int k;

    if (x > 709.0) return 1.0 / 0.0;
    if (x < -745.0) return 0.0;
    k = (int)(x / ln2 + (x >= 0 ? 0.5 : -0.5));
    r = x - (double)k * ln2;
    term = 1.0;
    sum = 1.0;
    for (int i = 1; i < 24; ++i) {
        term *= r / (double)i;
        sum += term;
    }
    return ldexp(sum, k);
}

double log(double x)
{
    const double ln2 = 0.6931471805599453;
    int e;
    double m, t, t2, sum;

    if (x <= 0.0) return -1.0 / 0.0;
    m = frexp(x, &e);
    t = (m - 1.0) / (m + 1.0);
    t2 = t * t;
    sum = 0.0;
    for (int i = 0; i < 30; ++i) {
        int n = 2 * i + 1;
        sum += t / (double)n;
        t *= t2;
    }
    return 2.0 * sum + (double)e * ln2;
}

double log10(double x) { return log(x) / 2.302585092994046; }

double pow(double x, double y)
{
    long n = (long)y;
    double r = 1.0;

    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x <= 0.0) return 0.0;
    if ((double)n == y && n >= -64 && n <= 64) {
        int neg = n < 0;
        if (neg) n = -n;
        for (long i = 0; i < n; ++i) r *= x;
        return neg ? 1.0 / r : r;
    }
    return exp(y * log(x));
}

int isinf(double x) { return x == (1.0 / 0.0) || x == -(1.0 / 0.0); }
int isnan(double x) { return x != x; }

/* -------------------------------- time ---------------------------------- */
time_t time(time_t *t) { time_t v = (time_t)(sys_time_ms() / 1000); if (t) *t = v; return v; }
clock_t clock(void) { return (clock_t)sys_time_ms(); }
double difftime(time_t a, time_t b) { return (double)(a - b); }
struct tm *localtime(const time_t *t) { (void)t; return NULL; }
struct tm *gmtime(const time_t *t) { (void)t; return NULL; }
time_t mktime(struct tm *tm) { (void)tm; return (time_t)-1; }
size_t strftime(char *b, size_t c, const char *f, const struct tm *tm) { (void)b;(void)c;(void)f;(void)tm; return 0; }

/* ------------------------------- locale --------------------------------- */
static struct lconv lc = { ".", "", "", "", "", "", "", "", "", "", 0, 0, 0, 0, 0, 0, 0, 0 };
char *setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
struct lconv *localeconv(void) { return &lc; }

/* ------------------------------- assert --------------------------------- */
void __assert_fail(const char *expr, const char *file, int line)
{
    printf("assert(%s) failed at %s:%d\n", expr, file, line);
    abort();
}

/* ----------------------------- setjmp/longjmp --------------------------- */
__attribute__((naked, returns_twice)) int setjmp(jmp_buf env)
{
    (void)env;
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

__attribute__((naked, noreturn)) void longjmp(jmp_buf env, int val)
{
    (void)env;
    (void)val;
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
