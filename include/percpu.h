#pragma once

#define PERCPU_MAX 16u

#include <types.h>
#include <spinlock.h>

struct task;

struct per_cpu {
    u64 current_task;   /* offset 0: mov %gs:0 */
    u64 apic_id;
    u64 online;
    struct task *ready_head;
    struct task *idle_task;
    spinlock_t rq_lock;
};

void percpu_init_cpu(u32 apic_id, struct task *current);
struct task *percpu_current(void);
void percpu_set_current(struct task *t);
u32 percpu_apic_id(void);
u32 percpu_apic_id_of(u32 cpu);
int percpu_cpu_online(u32 cpu);
struct per_cpu *percpu_self(void);
void percpu_set_idle(u32 cpu, struct task *t);
void percpu_enqueue_task(u32 cpu, struct task *t);
struct task *percpu_dequeue_task(u32 cpu);
void percpu_remove_task(u32 cpu, struct task *t);
