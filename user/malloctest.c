/*
 * Lantest OS - malloc/free/calloc/realloc self-test
 */
#include "userlib.h"
#include "malloc.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    unsigned char *a;
    unsigned char *b;
    unsigned char *big;
    unsigned char *z;
    unsigned char *r;

    (void)argc;
    (void)argv;
    (void)envp;

    a = (unsigned char *)malloc(100);
    b = (unsigned char *)malloc(200);
    if (a == NULL || b == NULL) {
        sys_write_str("malloctest: small alloc FAIL\n");
        return 1;
    }
    for (int i = 0; i < 100; ++i)
        a[i] = (unsigned char)(i + 1);
    for (int i = 0; i < 200; ++i)
        b[i] = (unsigned char)(i + 2);
    if (a[0] != 1 || a[99] != 100 || b[0] != 2 || b[199] != 201) {
        sys_write_str("malloctest: write FAIL\n");
        return 1;
    }

    z = (unsigned char *)calloc(64, 8);
    if (z == NULL) {
        sys_write_str("malloctest: calloc FAIL\n");
        return 1;
    }
    for (int i = 0; i < 512; ++i) {
        if (z[i] != 0) {
            sys_write_str("malloctest: calloc not zero FAIL\n");
            return 1;
        }
    }

    r = (unsigned char *)realloc(a, 256);
    if (r == NULL || r[0] != 1 || r[99] != 100) {
        sys_write_str("malloctest: realloc FAIL\n");
        return 1;
    }
    a = r;

    big = (unsigned char *)malloc(64 * 1024);
    if (big == NULL) {
        sys_write_str("malloctest: large alloc FAIL\n");
        return 1;
    }
    for (int i = 0; i < 64 * 1024; ++i)
        big[i] = (unsigned char)(i & 0xff);
    for (int i = 0; i < 64 * 1024; ++i) {
        if (big[i] != (unsigned char)(i & 0xff)) {
            sys_write_str("malloctest: large write FAIL\n");
            return 1;
        }
    }

    free(big);   /* unmaps its 64 KiB VMA */
    free(a);
    free(b);
    free(z);

    a = (unsigned char *)malloc(64);
    if (a == NULL) {
        sys_write_str("malloctest: reuse FAIL\n");
        return 1;
    }
    free(a);

    sys_write_str("malloctest: PASS\n");
    return 0;
}
