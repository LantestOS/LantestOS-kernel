/*
 * Lantest OS - user libc self-test
 */
#include "userlib.h"
#include "libc.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    char buf[128];
    char copied[16];
    const char *hello = "lantest";

    (void)argc;
    (void)argv;
    (void)envp;

    if (strlen(hello) != 7 || strcmp(hello, "lantest") != 0) {
        printf("libctest: strlen/strcmp FAIL\n");
        return 1;
    }
    if (atoi("-123") != -123 || atoi("456") != 456) {
        printf("libctest: atoi FAIL\n");
        return 1;
    }

    memset(copied, 0, sizeof(copied));
    memcpy(copied, "abc", 4);
    if (strcmp(copied, "abc") != 0 || memcmp(copied, "abc", 3) != 0) {
        printf("libctest: mem/str FAIL\n");
        return 1;
    }

    snprintf(buf, sizeof(buf), "%s %d %08x %c", "ok", -42, 0x1a2b, 'Z');
    if (strcmp(buf, "ok -42 00001a2b Z") != 0) {
        printf("libctest: snprintf FAIL [%s]\n", buf);
        return 1;
    }

    printf("libctest: %s %d %08x %c\n", "format", 7, 0xbeef, '!');
    printf("libctest: PASS\n");
    return 0;
}
