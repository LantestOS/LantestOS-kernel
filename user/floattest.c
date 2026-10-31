/*
 * Lantest OS - SSE / floating-point smoke test
 */
#include "userlib.h"
#include "libc.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    volatile double a = 1.5;
    volatile double b = 2.25;
    double c = a * b;
    int ok = (c == 3.375);

    (void)argc;
    (void)argv;
    (void)envp;

    printf("floattest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
