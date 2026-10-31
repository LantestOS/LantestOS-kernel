/*
 * Lantest OS - enter Ring 3 via iretq (x86_64)
 */
.section .text
.code64

/*
 * void user_enter(u64 entry, u64 user_stack_top);
 *   rdi = user RIP, rsi = user RSP
 */
.global user_enter
.type user_enter, @function
user_enter:
    cli

    pushq $0x23            /* SS  = user data selector (0x20 | RPL 3) */
    pushq %rsi             /* RSP = user stack top */
    pushq $0x202           /* RFLAGS: IF = 1 */
    pushq $0x1B            /* CS  = user code selector (0x18 | RPL 3) */
    pushq %rdi             /* RIP = user entry */

    swapgs                 /* kernel per-CPU GS -> user GS=0 */
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    iretq

/*
 * void user_enter_regs(registers_t *regs);
 *
 * The frame layout is defined in include/x86_64/isr.h:
 *   r15..r8 (0..56), rbp(64), rdi(72), rsi(80), rdx(88), rcx(96),
 *   rbx(104), rax(112), int_no(120), err_code(128), rip(136), cs(144),
 *   rflags(152), rsp(160), ss(168).
 *
 * Used by fork(): the child restores the parent's user registers but with
 * rax = 0, so the fork() call appears to return 0 in the child.
 */
.global user_enter_regs
.type user_enter_regs, @function
user_enter_regs:
    cli

    pushq 168(%rdi)        /* SS */
    pushq 160(%rdi)        /* RSP */
    pushq 152(%rdi)        /* RFLAGS */
    pushq 144(%rdi)        /* CS */
    pushq 136(%rdi)        /* RIP */

    swapgs                 /* kernel per-CPU GS -> user GS=0 */
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    movq 0(%rdi), %r15
    movq 8(%rdi), %r14
    movq 16(%rdi), %r13
    movq 24(%rdi), %r12
    movq 32(%rdi), %r11
    movq 40(%rdi), %r10
    movq 48(%rdi), %r9
    movq 56(%rdi), %r8
    movq 64(%rdi), %rbp
    movq 80(%rdi), %rsi
    movq 88(%rdi), %rdx
    movq 96(%rdi), %rcx
    movq 104(%rdi), %rbx
    movq 112(%rdi), %rax
    movq 72(%rdi), %rdi   /* restore rdi last (it is the frame pointer) */

    iretq
