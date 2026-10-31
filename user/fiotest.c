/*
 * Lantest OS - FILE stream self-test
 */
#include "userlib.h"
#include "stdio.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    FILE *f;
    char buf[64];
    char c;

    (void)argc;
    (void)argv;
    (void)envp;

    f = fopen("/fio.txt", "w");
    if (f == NULL) {
        printf("fiotest: fopen w FAIL\n");
        return 1;
    }
    if (fprintf(f, "hello %d %s\n", 42, "file") < 0) {
        printf("fiotest: fprintf FAIL\n");
        return 1;
    }
    if (fwrite("abc", 1, 3, f) != 3) {
        printf("fiotest: fwrite FAIL\n");
        return 1;
    }
    if (fflush(NULL) != 0 || ftell(f) != 17) {
        printf("fiotest: write buffer flush FAIL\n");
        return 1;
    }
    fclose(f);

    f = fopen("/fio.txt", "r");
    if (f == NULL) {
        printf("fiotest: fopen r FAIL\n");
        return 1;
    }
    if (fread(buf, 1, 12, f) != 12) {
        printf("fiotest: fread FAIL\n");
        return 1;
    }
    buf[12] = '\0';
    if (strcmp(buf, "hello 42 fil") != 0) {
        printf("fiotest: content FAIL [%s]\n", buf);
        return 1;
    }
    c = (char)fgetc(f);
    if (c != 'e') {
        printf("fiotest: fgetc FAIL [%c]\n", c);
        return 1;
    }
    fclose(f);

    f = fopen("/fio.txt", "r");
    if (f == NULL || fgets(buf, sizeof(buf), f) == NULL ||
        strcmp(buf, "hello 42 file\n") != 0) {
        printf("fiotest: fgets FAIL\n");
        return 1;
    }
    fclose(f);

    f = fopen("/fio.txt", "a");
    if (f == NULL) {
        printf("fiotest: fopen a FAIL\n");
        return 1;
    }
    fputs("XYZ", f);
    fclose(f);

    f = fopen("/fio.txt", "r");
    if (f == NULL) {
        printf("fiotest: reopen append FAIL\n");
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != 20) {
        printf("fiotest: ftell FAIL\n");
        return 1;
    }
    if (fseek(f, -3, SEEK_END) != 0 || fgetc(f) != 'X') {
        printf("fiotest: fseek end FAIL\n");
        return 1;
    }
    if (fseek(f, 6, SEEK_SET) != 0 || fgetc(f) != '4') {
        printf("fiotest: fseek set FAIL\n");
        return 1;
    }
    fclose(f);

    printf("fiotest: PASS\n");
    return 0;
}
