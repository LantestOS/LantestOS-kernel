/*
 * Lantest OS - intentional SIGSEGV test
 *
 * A wild user pointer must terminate only this process (status 139), not
 * panic the kernel.
 */
#include "userlib.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    sys_write_str("segvtest: about to fault\n");
    *(volatile u64 *)0x200000000ULL = 0xDEADBEEFULL;
    sys_write_str("segvtest: FAIL survived\n");
    return 1;
}
