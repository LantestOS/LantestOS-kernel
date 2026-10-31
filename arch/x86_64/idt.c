/*
 * Lantest OS - interrupt descriptor table and exception dispatch
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>
#include <syscall.h>
#include <signal.h>
#include <task.h>
#include <uaccess.h>
#include <x86_64/idt.h>
#include <x86_64/isr.h>
#include <x86_64/irq.h>
#include <x86_64/paging.h>
#include <x86_64/usermode.h>
#include <tlb.h>
#include <percpu.h>
#include <lapic.h>

#define IDT_ENTRIES 256u
#define KERNEL_CS   0x08u
#define SYSCALL_VEC 0x80u
#define SMP_TEST_VEC 0x30u
#define SMP_TIMER_VEC 0x31u
#define SMP_WAKE_VEC 0xFEu

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  flags;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

extern const u64 isr_stub_table[50];
extern void isr_default(void);
extern void isr128(void);
extern void isr254(void);
extern void isr253(void);

volatile u32 smp_test_irq_count;
volatile u32 smp_timer_ticks;
static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));
static struct idt_ptr idtr;

static const char *const exception_names[32] = {
    "divide error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point error",
    "alignment check",
    "machine check",
    "SIMD floating-point error",
    "virtualization exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection exception",
    "VMM communication exception",
    "security exception",
    "reserved",
};

static void idt_set_gate_flags(u32 vec, u64 handler, u8 flags)
{
    idt[vec].offset_low = (u16)(handler & 0xFFFF);
    idt[vec].selector = KERNEL_CS;
    idt[vec].ist = 0;
    idt[vec].flags = flags;
    idt[vec].offset_mid = (u16)((handler >> 16) & 0xFFFF);
    idt[vec].offset_high = (u32)((handler >> 32) & 0xFFFFFFFF);
    idt[vec].zero = 0;
}

void idt_init(void)
{
    u64 default_handler = (u64)(usize)isr_default;

    for (u32 i = 0; i < IDT_ENTRIES; ++i)
        idt_set_gate_flags(i, default_handler, 0x8E);

    for (u32 i = 0; i < 50; ++i)
        idt_set_gate_flags(i, isr_stub_table[i], 0x8E);

    /* int 0x80 is the custom syscall gate, callable from Ring 3 (DPL 3). */
    idt_set_gate_flags(SYSCALL_VEC, (u64)(usize)isr128, 0xEE);
    idt_set_gate_flags(SMP_WAKE_VEC, (u64)(usize)isr254, 0x8E);

    idtr.limit = (u16)(sizeof(idt) - 1);
    idtr.base = (u64)idt;
    __asm__ volatile ("lidt (%0)" : : "r"(&idtr));
}

void idt_reload(void)
{
    __asm__ volatile ("lidt (%0)" : : "r"(&idtr));
}

void isr_handler(registers_t *regs)
{
    /*
     * A user task could set AC before int 0x80 to try to bypass SMAP.
     * Clear it on every kernel entry; copy_*_user sets it only around the
     * actual user access.
     */
    uaccess_clear_ac();

    if (regs->int_no == SMP_WAKE_VEC) {
        /* Never schedule() from an interrupt context: with real IPI
         * delivery this handler now actually fires, and a nested
         * schedule() on top of the interrupted context corrupts the
         * AP's switch state. The AP idle loop polls, so an EOI is
         * all the wakeup needs. */
        lapic_eoi();
        return;
    }

    if (regs->int_no == SMP_TIMER_VEC) {
        __atomic_add_fetch(&smp_timer_ticks, 1, __ATOMIC_SEQ_CST);
        lapic_eoi();
        schedule();
        return;
    }

    if (regs->int_no == SMP_TEST_VEC) {
        __atomic_add_fetch(&smp_test_irq_count, 1, __ATOMIC_SEQ_CST);
        kprintf("[smp] AP vector 0x30 handled, apic id=%u\n",
                percpu_apic_id());
        if (paging_read_cr3() == tlb_shootdown_target()) {
            paging_write_cr3(paging_read_cr3());
        }
        __atomic_add_fetch(&tlb_ack_count, 1, __ATOMIC_SEQ_CST);
        lapic_eoi();
        return;
    }

    if (regs->int_no == SYSCALL_VEC) {
        syscall_handler(regs);
        return;
    }

    if (regs->int_no >= 32) {
        irq_handler(regs);
        if ((regs->cs & 3u) == 3u)
            signal_deliver_pending(regs);
        return;
    }

    /*
     * Copy-on-write: a write to a COW page can fault from user mode or from
     * copy_to_user in kernel mode. Resolve it before the uaccess fixup path,
     * so copy_to_user retries instead of returning -EFAULT.
     */
    if (regs->int_no == 14) {
        u64 cr2;

        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 >= USER_BASE && cr2 < USER_STACK_TOP &&
            (paging_handle_cow(cr2) || paging_handle_demand(cr2)))
            return;
    }

    /*
     * A user fault that is neither COW nor demand is a segfault. Deliver
     * SIGSEGV to this task only; the kernel keeps running.
     */
    if (regs->int_no == 14 && (regs->cs & 3u) == 3u) {
        u64 cr2;

        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("[segv] pid=%lu name=%s cr2=%p err=%lx\n",
                task_current()->pid, task_current()->name,
                (void *)cr2, regs->err_code);
        signal_send(task_current(), SIGSEGV);
        signal_deliver_pending(regs);
        return;
    }

    /*
     * copy_from_user/copy_to_user fault recovery: if the faulting RIP is in
     * the exception table, redirect to the fixup and return -EFAULT instead
     * of panicking (kernel-mode faults only; user faults are never covered).
     */
    if (regs->int_no == 14 && (regs->cs & 3u) == 0u &&
        fixup_exception(regs))
        return;

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  CPU EXCEPTION %lu: %s\n",
            regs->int_no, exception_names[regs->int_no]);
    kprintf("==================================================\n");
    kprintf("[exc ] err_code=%lx\n", regs->err_code);
    kprintf("[exc ] rip=%p cs=%lx rflags=%lx\n",
            (void *)regs->rip, regs->cs, regs->rflags);
    kprintf("[exc ] rsp=%p ss=%lx\n", (void *)regs->rsp, regs->ss);

    if (regs->int_no == 14) {
        u64 cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("[exc ] cr2=%p (faulting address)\n", (void *)cr2);
    }

    panic("unhandled CPU exception");
}
