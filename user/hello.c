/*
 * Lantest OS - external program loaded by execve()
 *
 * The kernel lays out the SysV-style initial stack:
 *   [argc][argv[0]..argv[argc-1]][NULL][envp...][NULL][strings...]
 * The naked _start reads argc/argv/envp, calls main(), then sys_exit().
 */
#include "userlib.h"

static void print_num(long v)
{
    char b[24];
    int i = 0;
    unsigned long u;

    if (v == 0) {
        sys_write(STDOUT_FD, "0", 1);
        return;
    }
    if (v < 0) {
        sys_write(STDOUT_FD, "-", 1);
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    while (u > 0) {
        b[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    while (i > 0)
        sys_write(STDOUT_FD, &b[--i], 1);
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    sys_write_str("hello from an exec'd ELF!\n");
    sys_write_str("argc=");
    print_num(argc);
    sys_write_str("\n");
    for (int i = 0; i < argc; ++i) {
        sys_write_str("argv[");
        print_num(i);
        sys_write_str("]=");
        sys_write_str(argv[i]);
        sys_write_str("\n");
    }
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile (
        "movq (%rsp), %rdi\n"
        "leaq 8(%rsp), %rsi\n"
        "leaq 8(%rsi,%rdi,8), %rdx\n"
        "andq $-16, %rsp\n"
        "call main\n"
        "movl %eax, %edi\n"
        "movl $2, %eax\n"
        "int $0x80\n"
        "1: jmp 1b\n"
    );
}
