/*
 * Lantest OS - isolated AP user sys_exit test
 */
#include "userlib.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    __asm__ volatile ("movq $2, %rax; movq $42, %rdi; int $0x80");
    for (;;)
        ;
}
