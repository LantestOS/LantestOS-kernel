/*
 * Lantest OS - interrupt/exception register frame
 *
 * Field order is defined by the low-to-high stack layout produced by
 * arch/x86_64/isr_asm.S. Do not reorder without updating the assembly.
 */
#pragma once

#include <types.h>

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err_code;
    /* Pushed by the CPU on interrupt entry: */
    u64 rip, cs, rflags, rsp, ss;
} __attribute__((packed)) registers_t;

/* Common C entry called by the assembly stubs for every vector. */
void isr_handler(registers_t *regs);
