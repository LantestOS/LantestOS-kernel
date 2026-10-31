/*
 * Lantest OS - common naked startup for external programs
 *
 * Kernel initial stack: [argc][argv...][NULL][envp...][NULL][strings...]
 * A C _start(void) cannot see those arguments, so this header provides a
 * naked entry that extracts them and calls main(argc, argv, envp).
 */
#pragma once

#include "userlib.h"

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
