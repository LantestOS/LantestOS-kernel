/*
 * Lantest OS - tiny user-space FILE streams with read buffering
 */
#pragma once

#include "libc.h"
#include "malloc.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE {
    int fd;
    int writable;
    int eof;
    int err;
    int unbuffered;
    unsigned char rbuf[512];
    u64 rpos;
    u64 rlen;
    unsigned char wbuf[512];
    u64 wpos;
    struct FILE *next;
} FILE;

static FILE *libc_file_list;
static FILE libc_stdin_file = { STDIN_FD, 0, 0, 0, 0, { 0 }, 0, 0, { 0 }, 0, 0 };
static FILE libc_stdout_file = { STDOUT_FD, 1, 0, 0, 1, { 0 }, 0, 0, { 0 }, 0, 0 };
static FILE libc_stderr_file = { STDERR_FD, 1, 0, 0, 1, { 0 }, 0, 0, { 0 }, 0, 0 };

#define stdin  (&libc_stdin_file)
#define stdout (&libc_stdout_file)
#define stderr (&libc_stderr_file)

static __attribute__((unused)) u64 file_fill(FILE *f)
{
    i64 n;

    if (f->rpos < f->rlen)
        return f->rlen - f->rpos;

    n = sys_read(f->fd, f->rbuf, sizeof(f->rbuf));
    if (n <= 0) {
        f->rlen = 0;
        f->rpos = 0;
        f->eof = 1;
        return 0;
    }
    f->rpos = 0;
    f->rlen = (u64)n;
    return f->rlen;
}

static __attribute__((unused)) int file_flush(FILE *f)
{
    u64 done = 0;

    if (f == NULL)
        return -1;
    if (f->wpos == 0)
        return 0;

    while (done < f->wpos) {
        i64 n = sys_write(f->fd, f->wbuf + done, f->wpos - done);

        if (n <= 0) {
            f->err = 1;
            return -1;
        }
        done += (u64)n;
    }
    f->wpos = 0;
    return 0;
}

static __attribute__((unused)) FILE *fopen(const char *path, const char *mode)
{
    FILE *f;
    int fd;
    unsigned int flags = 0;
    int append = 0;

    if (path == NULL || mode == NULL)
        return NULL;

    if (strcmp(mode, "r") == 0) {
        flags = O_RDONLY;
    } else if (strcmp(mode, "w") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "a") == 0) {
        flags = O_WRONLY | O_CREAT;
        append = 1;
    } else if (strcmp(mode, "r+") == 0) {
        flags = O_RDWR;
    } else if (strcmp(mode, "w+") == 0) {
        flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "a+") == 0) {
        flags = O_RDWR | O_CREAT;
        append = 1;
    } else {
        return NULL;
    }

    fd = (int)sys_open(path, flags);
    if (fd < 0)
        return NULL;

    f = (FILE *)malloc(sizeof(FILE));
    if (f == NULL) {
        sys_close(fd);
        return NULL;
    }

    f->fd = fd;
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    f->eof = 0;
    f->err = 0;
    f->unbuffered = 0;
    f->rpos = 0;
    f->rlen = 0;
    f->wpos = 0;
    f->next = NULL;

    if (append) {
        if (sys_lseek(fd, 0, SEEK_END) < 0) {
            sys_close(fd);
            free(f);
            return NULL;
        }
    }
    f->next = libc_file_list;
    libc_file_list = f;
    return f;
}

static __attribute__((unused)) int fclose(FILE *f)
{
    int rc;

    if (f == NULL || f == stdin || f == stdout || f == stderr)
        return -1;
    (void)file_flush(f);
    {
        FILE **link = &libc_file_list;

        while (*link != NULL) {
            if (*link == f) {
                *link = f->next;
                break;
            }
            link = &(*link)->next;
        }
    }
    rc = (int)sys_close(f->fd);
    free(f);
    return rc;
}

static __attribute__((unused)) u64 fread(void *ptr, u64 size, u64 nmemb,
                                         FILE *f)
{
    u64 total;
    u64 done = 0;
    unsigned char *dst = (unsigned char *)ptr;

    if (f == NULL || ptr == NULL || size == 0 || nmemb == 0)
        return 0;
    if (f->wpos != 0 && file_flush(f) != 0)
        return 0;

    total = size * nmemb;
    while (done < total) {
        if (f->rpos < f->rlen) {
            u64 take = f->rlen - f->rpos;

            if (take > total - done)
                take = total - done;
            memcpy(dst + done, f->rbuf + f->rpos, take);
            f->rpos += take;
            done += take;
            continue;
        }

        if (total - done >= sizeof(f->rbuf)) {
            i64 n = sys_read(f->fd, dst + done, total - done);

            if (n <= 0) {
                f->eof = 1;
                break;
            }
            done += (u64)n;
            continue;
        }

        if (file_fill(f) == 0)
            break;
    }
    return done / size;
}

static __attribute__((unused)) u64 fwrite(const void *ptr, u64 size, u64 nmemb,
                                          FILE *f)
{
    u64 total;
    u64 done = 0;
    const unsigned char *p = (const unsigned char *)ptr;

    if (f == NULL || ptr == NULL || size == 0 || nmemb == 0)
        return 0;

    total = size * nmemb;

    if (f->unbuffered) {
        while (done < total) {
            i64 n = sys_write(f->fd, p + done, total - done);

            if (n <= 0) {
                f->err = 1;
                break;
            }
            done += (u64)n;
        }
        return done / size;
    }

    while (done < total) {
        if (f->wpos == sizeof(f->wbuf)) {
            if (file_flush(f) != 0)
                return done / size;
        }
        f->wbuf[f->wpos++] = p[done++];
    }
    return done / size;
}

static __attribute__((unused)) int fgetc(FILE *f)
{
    if (f == NULL)
        return -1;
    if (f->wpos != 0 && file_flush(f) != 0)
        return -1;
    if (f->rpos >= f->rlen && file_fill(f) == 0)
        return -1;
    return (int)f->rbuf[f->rpos++];
}

static __attribute__((unused)) int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;

    if (f == NULL)
        return -1;
    if (f->unbuffered) {
        if (sys_write(f->fd, &ch, 1) != 1) {
            f->err = 1;
            return -1;
        }
        return (int)ch;
    }
    if (f->wpos == sizeof(f->wbuf) && file_flush(f) != 0)
        return -1;
    f->wbuf[f->wpos++] = ch;
    return (int)ch;
}

static __attribute__((unused)) char *fgets(char *buf, int size, FILE *f)
{
    int i = 0;

    if (buf == NULL || size <= 0 || f == NULL)
        return NULL;

    while (i + 1 < size) {
        int c = fgetc(f);

        if (c < 0)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buf[i] = '\0';
    return buf;
}

static __attribute__((unused)) int fputs(const char *s, FILE *f)
{
    if (s == NULL || f == NULL)
        return -1;
    while (*s != '\0') {
        if (fputc(*s++, f) < 0)
            return -1;
    }
    return 0;
}

static __attribute__((unused)) i64 fseek(FILE *f, i64 offset, int whence)
{
    i64 pos;

    if (f == NULL)
        return -1;
    if (file_flush(f) != 0)
        return -1;

    pos = sys_lseek(f->fd, offset, whence);
    if (pos < 0)
        return -1;
    f->rpos = 0;
    f->rlen = 0;
    f->eof = 0;
    return 0;
}

static __attribute__((unused)) i64 ftell(FILE *f)
{
    i64 pos;

    if (f == NULL)
        return -1;
    if (file_flush(f) != 0)
        return -1;

    pos = sys_lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    return pos - (i64)(f->rlen - f->rpos);
}

static __attribute__((unused)) void rewind(FILE *f)
{
    (void)fseek(f, 0, SEEK_SET);
}

static __attribute__((unused)) int fprintf(FILE *f, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    if (f == NULL)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        u64 len = (u64)n;
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        (void)fwrite(buf, 1, len, f);
    }
    return n;
}

static __attribute__((unused)) int fflush(FILE *f)
{
    if (f != NULL)
        return file_flush(f);

    {
        FILE *p;
        int rc = 0;

        for (p = libc_file_list; p != NULL; p = p->next) {
            if (file_flush(p) != 0)
                rc = -1;
        }
        return rc;
    }
}

static __attribute__((unused)) int feof(FILE *f)
{
    return f != NULL && f->eof;
}

static __attribute__((unused)) int ferror(FILE *f)
{
    return f != NULL && f->err;
}

static __attribute__((unused)) int fileno(FILE *f)
{
    return f != NULL ? f->fd : -1;
}