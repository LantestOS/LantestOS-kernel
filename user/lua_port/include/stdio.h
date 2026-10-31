#pragma once
#include <stddef.h>
#include <stdarg.h>
typedef struct _FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 512
#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
#define getc(f) fgetc(f)
int ungetc(int c, FILE *f);
FILE *freopen(const char *path, const char *mode, FILE *f);
int fputc(int c, FILE *f);
#define putc(c,f) fputc((c),(f))
char *fgets(char *buf, int size, FILE *f);
int fputs(const char *s, FILE *f);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
#define L_tmpnam 32
char *tmpnam(char *s);
FILE *tmpfile(void);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
int fileno(FILE *f);
