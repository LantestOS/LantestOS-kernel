/*
 * Lantest OS - per-CPU state (M4-34)
 *
 * The kernel GS base points at a per-CPU block. task_current() reads
 * current_task from gs:0, so BSP and APs cannot clobber each other.
 */
#include <types.h>
#include <percpu.h>
#include <task.h>
#include <spinlock.h>
#include <lapic.h>

#define MSR_GS_BASE 0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

static struct per_cpu cpus[PERCPU_MAX];

static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 value)
{
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)));
}

void percpu_init_cpu(u32 apic_id, struct task *current)
{
    struct per_cpu *pc;
    u32 idx = apic_id;

    if (idx >= PERCPU_MAX)
        idx = 0;
    pc = &cpus[idx];
    pc->current_task = (u64)(usize)current;
    pc->apic_id = apic_id;
    pc->online = 1;
    pc->ready_head = NULL;
    pc->idle_task = NULL;
    wrmsr(MSR_GS_BASE, (u64)(usize)pc);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

struct task *percpu_current(void)
{
    struct task *t;

    if (rdmsr(MSR_GS_BASE) == 0)
        return NULL;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(t));
    return t;
}

void percpu_set_current(struct task *t)
{
    struct per_cpu *pc = (struct per_cpu *)(usize)rdmsr(MSR_GS_BASE);

    if (pc != NULL)
        pc->current_task = (u64)(usize)t;
    else
        __asm__ volatile ("movq %0, %%gs:0" : : "r"((u64)(usize)t));
}

struct per_cpu *percpu_self(void)
{
    return (struct per_cpu *)(usize)rdmsr(MSR_GS_BASE);
}

void percpu_set_idle(u32 cpu, struct task *t)
{
    if (cpu < PERCPU_MAX)
        cpus[cpu].idle_task = t;
}

void percpu_enqueue_task(u32 cpu, struct task *t)
{
    struct per_cpu *pc;
    u64 flags;

    if (cpu >= PERCPU_MAX || t == NULL)
        return;
    pc = &cpus[cpu];
    spin_lock_irqsave(&pc->rq_lock, &flags);
    t->rq_next = NULL;
    if (pc->ready_head == NULL) {
        pc->ready_head = t;
    } else {
        struct task *p = pc->ready_head;
        while (p->rq_next != NULL)
            p = p->rq_next;
        p->rq_next = t;
    }
    t->on_rq = 1;
    spin_unlock_irqrestore(&pc->rq_lock, flags);

    if (cpu != 0 && cpus[cpu].apic_id != 0)
        lapic_send_ipi((u32)cpus[cpu].apic_id, 0xFEu);
}

struct task *percpu_dequeue_task(u32 cpu)
{
    struct per_cpu *pc;
    struct task *t;
    u64 flags;

    if (cpu >= PERCPU_MAX)
        return NULL;
    pc = &cpus[cpu];
    spin_lock_irqsave(&pc->rq_lock, &flags);
    t = pc->ready_head;
    if (t != NULL) {
        pc->ready_head = t->rq_next;
        t->rq_next = NULL;
        t->on_rq = 0;
    }
    spin_unlock_irqrestore(&pc->rq_lock, flags);
    return t;
}

void percpu_remove_task(u32 cpu, struct task *t)
{
    struct per_cpu *pc;
    u64 flags;

    if (cpu >= PERCPU_MAX || t == NULL || !t->on_rq)
        return;
    pc = &cpus[cpu];
    spin_lock_irqsave(&pc->rq_lock, &flags);
    if (pc->ready_head == t) {
        pc->ready_head = t->rq_next;
        t->rq_next = NULL;
        t->on_rq = 0;
    } else {
        struct task *p = pc->ready_head;

        while (p != NULL && p->rq_next != t)
            p = p->rq_next;
        if (p != NULL) {
            p->rq_next = t->rq_next;
            t->rq_next = NULL;
            t->on_rq = 0;
        }
    }
    spin_unlock_irqrestore(&pc->rq_lock, flags);
}

int percpu_cpu_online(u32 cpu)
{
    if (cpu >= PERCPU_MAX)
        return 0;
    return cpus[cpu].online != 0;
}

u32 percpu_apic_id_of(u32 cpu)
{
    if (cpu >= PERCPU_MAX)
        return 0;
    return (u32)cpus[cpu].apic_id;
}

u32 percpu_apic_id(void)
{
    struct per_cpu *pc = (struct per_cpu *)(usize)rdmsr(MSR_GS_BASE);
    return pc != NULL ? (u32)pc->apic_id : 0;
}
