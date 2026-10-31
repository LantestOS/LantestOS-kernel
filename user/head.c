/*
 * Lantest OS - /bin/head : first line of stdin
 */
#include "userlib.h"
#include "stdio.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    int c;

    (void)argc;
    (void)argv;
    (void)envp;

    while ((c = fgetc(stdin)) >= 0) {
        fputc(c, stdout);
        if (c == '\n')
            break;
    }
    return 0;
}
