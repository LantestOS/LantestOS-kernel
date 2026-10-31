/*
 * Lantest OS - interrupt/exception entry stubs (x86_64, GNU as)
 *
 * Layout produced on the kernel stack (low -> high addresses):
 *
 *   [r15][r14][r13][r12][r11][r10][r9][r8]
 *   [rbp][rdi][rsi][rdx][rcx][rbx][rax]
 *   [int_no][err_code]
 *   [rip][cs][rflags][rsp][ss]      <- pushed by the CPU
 *
 * This matches the field order of registers_t in include/x86_64/isr.h.
 *
 * Vectors 8, 10, 11, 12, 13, 14, 17, 21, 29, 30 push an error code themselves,
 * so their stubs must not push a dummy one. Every other stub pushes 0.
 */

.section .text
.code64

.macro ISR_NOERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushq $\num
    jmp isr_common
.endm

/* 0-7: no error code */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

/* 8-17 */
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17

/* 18-31 */
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* 32-47: remapped PIC IRQs (no error code) */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* M4-37: AP self-test vector */
ISR_NOERR 48
ISR_NOERR 49
ISR_NOERR 254
ISR_NOERR 253

/* int 0x80: custom syscall gate (DPL 3 in the IDT) */
ISR_NOERR 128

/* Vectors 48-255 all point here (spurious/unexpected). */
.global isr_default
.type isr_default, @function
isr_default:
    pushq $0
    pushq $255
    jmp isr_common

isr_common:
    testb $3, 24(%rsp)      /* saved CS at int_no+24 */
    jz 1f
    swapgs
1:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* registers_t *regs = rsp; keep the untampered rsp in callee-saved rbp */
    movq %rsp, %rdi
    movq %rsp, %rbp
    andq $-16, %rsp        /* SysV ABI: rsp 16-byte aligned before call */
    call isr_handler
    movq %rbp, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    testb $3, 24(%rsp)      /* CS still at int_no+24 */
    jz 2f
    swapgs
2:
    addq $16, %rsp         /* drop int_no and err_code */
    iretq

/* Jump table with the 48 implemented entry points, used by idt_init(). */
.section .rodata
.align 8
.global isr_stub_table
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .quad isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    .quad isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
    .quad isr48, isr49
