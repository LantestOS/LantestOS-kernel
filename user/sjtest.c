/*
 * Lantest OS - setjmp/longjmp and multi-page stack test
 */
#include "userlib.h"
#include "libc.h"
#include "crt.h"

static jmp_buf jump_env;
static volatile int jumped;

int main(int argc, char **argv, char **envp)
{
    volatile unsigned char big[8192];
    int v;

    (void)argc;
    (void)argv;
    (void)envp;

    big[0] = 1;
    big[sizeof(big) - 1] = 2;
    if (big[0] != 1 || big[sizeof(big) - 1] != 2) {
        printf("sjtest: stack FAIL\n");
        return 1;
    }

    v = setjmp(jump_env);
    if (v == 0) {
        jumped = 1;
        longjmp(jump_env, 42);
    }
    if (v != 42 || jumped != 1) {
        printf("sjtest: setjmp/longjmp FAIL (%d)\n", v);
        return 1;
    }

    printf("sjtest: PASS\n");
    return 0;
}
