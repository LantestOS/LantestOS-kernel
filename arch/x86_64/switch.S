/*
 * Lantest OS - context switch and first-entry trampoline (x86_64)
 *
 * void switch_to(struct task *prev, struct task *next);
 *   rdi = prev, rsi = next; task->rsp is at offset 8 (see include/task.h).
 *
 * Only callee-saved registers are preserved here, as required by the SysV
 * AMD64 ABI for an ordinary function call. When a task is preempted from the
 * timer IRQ, the interrupt stub has already saved every other register on the
 * task's kernel stack.
 */

.section .text
.code64

.extern task_bootstrap

.global switch_to
.type switch_to, @function
switch_to:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    fxsave 16(%rdi)         /* prev->fpu_state (offset 16) */
    fxrstor 16(%rsi)        /* next->fpu_state */
    movq %rsp, 8(%rdi)      /* prev->rsp = current stack pointer */
    movq 8(%rsi), %rsp      /* switch to next->rsp */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

/*
 * First-entry trampoline for a brand-new kernel thread.
 *
 * switch_to() "returns" here with RSP at the top of the thread's kernel
 * stack (16-byte aligned). Enable interrupts, then call the C bootstrap with
 * a properly aligned stack for the SysV ABI.
 */
.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    sti
    call task_bootstrap
    cli
    hlt
    jmp task_trampoline
