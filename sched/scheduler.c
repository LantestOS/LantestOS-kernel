/*
 * Lantest OS - round-robin scheduler and process lifecycle
 *
 * M3b: per-task PML4/CR3, Ring 3, int 0x80 syscalls.
 * M3c-1: exit -> ZOMBIE, wait() reaps children, idle reaper for orphans.
 * M3c-2: user tasks are loaded from real ELF64 images (page-wise).
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>
#include <kmalloc.h>
#include <pmm.h>
#include <elf.h>
#include <task.h>
#include <x86_64/gdt.h>
#include <x86_64/irq.h>
#include <x86_64/paging.h>
#include <x86_64/usermode.h>
#include <signal.h>
#include <vma.h>
#include <errno.h>
#include <percpu.h>
#include <tlb.h>
#include <lapic.h>
#include <spinlock.h>
#include <percpu.h>
#include <tlb.h>

#define TASK_STACK_PAGES 4u     /* 16 KiB per kernel stack */
#define SCHED_SLICE      10u    /* 100 Hz / 10 = 100 ms time slice */

extern void switch_to(struct task *prev, struct task *next);
extern void task_trampoline(void);

static struct task boot_task;
static struct task *current_task;
static struct task *task_head;
static u64 next_pid;
static u64 total_tasks;
static u64 boot_rsp0;
static int ready;
static spinlock_t run_queue_lock = SPINLOCK_INIT;
static spinlock_t deferred_lock = SPINLOCK_INIT;
static struct task *deferred_head;

/* switch.S relies on this offset; guard it at compile time. */
static_assert(__builtin_offsetof(struct task, rsp) == 8,
              "switch.S stores prev->rsp at offset 8");

static void name_copy(char *dst, const char *src)
{
    usize i = 0;

    while (src[i] != '\0' && i + 1 < TASK_NAME_MAX) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

void scheduler_init(void)
{
    boot_task.pid = 0;
    boot_task.rsp = 0;
    boot_task.cr3 = paging_read_cr3();      /* boot address space */
    boot_task.kernel_stack = NULL;
    boot_task.stack_size = 0;
    boot_task.entry = NULL;
    name_copy(boot_task.name, "boot");
    boot_task.state = TASK_RUNNING;
    boot_task.ticks = 0;
    boot_task.user = 0;
    boot_task.reserved = 0;
    boot_task.signal_pending = 0;
    boot_task.pgid = 0;
    boot_task.stop_signal = 0;
    boot_task.vmas = NULL;
    boot_task.mmap_next = 0;
    boot_task.user_entry = 0;
    boot_task.user_stack = 0;
    boot_task.fork_regs = NULL;
    boot_task.wait_next = NULL;
    boot_task.parent = NULL;
    boot_task.exit_code = 0;
    boot_task.next = &boot_task;

    current_task = &boot_task;
    percpu_init_cpu(lapic_id(), &boot_task);
    task_head = &boot_task;
    next_pid = 1;
    total_tasks = 1;
    boot_rsp0 = gdt_tss_rsp0();
    ready = 1;
}

int scheduler_ready(void)
{
    return ready;
}

static struct task *task_alloc(const char *name, u64 cr3)
{
    struct task *t;
    struct task *tail;
    u8 *stack;
    u64 stack_size;
    u64 *sp;

    t = (struct task *)kmalloc(sizeof(struct task));
    if (t == NULL)
        panic("task_alloc: out of heap memory");

    stack = (u8 *)pmm_alloc_pages(TASK_STACK_PAGES);
    if (stack == NULL)
        panic("task_alloc: out of physical memory for kernel stack");

    stack_size = (u64)TASK_STACK_PAGES * PAGE_SIZE;
    sp = (u64 *)(stack + stack_size);

    /* [r15][r14][r13][r12][rbp][rbx][ret = task_trampoline] */
    *--sp = (u64)(usize)task_trampoline;
    *--sp = 0;   /* rbx */
    *--sp = 0;   /* rbp */
    *--sp = 0;   /* r12 */
    *--sp = 0;   /* r13 */
    *--sp = 0;   /* r14 */
    *--sp = 0;   /* r15 */

    t->pid = next_pid++;
    t->pgid = t->pid;
    t->rsp = (u64)sp;
    for (u32 i = 0; i < sizeof(t->fpu_state); ++i)
        t->fpu_state[i] = 0;
    t->cr3 = cr3;
    t->kernel_stack = stack;
    t->stack_size = stack_size;
    t->entry = NULL;
    name_copy(t->name, name);
    t->state = TASK_READY;
    t->ticks = 0;
    t->user = 0;
    t->reserved = 0;
    t->signal_pending = 0;
    t->stop_signal = 0;
    t->vmas = NULL;
    t->mmap_next = USER_BASE + 0x80000ULL;
    t->user_entry = 0;
    t->user_stack = 0;
    t->fork_regs = NULL;
    t->wait_next = NULL;
    t->parent = NULL;
    t->exit_code = 0;
    t->active_cpus = 0;
    t->cpu = 0;
    t->on_rq = 0;
    t->rq_next = NULL;
    for (u32 i = 0; i < MAX_FDS; ++i)
        t->fd_table[i] = NULL;

    {
        u64 flags;

        spin_lock_irqsave(&run_queue_lock, &flags);
        tail = task_head;
        while (tail->next != task_head)
            tail = tail->next;
        tail->next = t;
        t->next = task_head;
        total_tasks++;
        spin_unlock_irqrestore(&run_queue_lock, flags);
    }

    total_tasks++;
    return t;
}

struct task *task_create_kernel(const char *name, void (*entry)(void))
{
    u64 pml4 = paging_create_kernel_pml4();
    struct task *t;

    if (pml4 == 0)
        panic("task_create_kernel: out of physical memory for PML4");

    t = task_alloc(name, pml4);
    t->entry = entry;
    return t;
}

struct task *task_create_user_elf(const char *name, const u8 *elf, u64 size)
{
    u64 pml4 = paging_create_kernel_pml4();
    u64 entry = 0;
    struct task *t;
    u64 stack_phys;

    if (pml4 == 0)
        panic("task_create_user_elf: out of physical memory for PML4");

    if (elf_load_user(pml4, elf, size, &entry) != 0) {
        paging_destroy_user_space(pml4);
        panic("task_create_user_elf: invalid ELF image");
    }

    for (u32 i = 0; i < USER_STACK_PAGES; ++i) {
        stack_phys = pmm_alloc_page();
        if (stack_phys == 0) {
            paging_destroy_user_space(pml4);
            panic("task_create_user_elf: out of physical memory for user stack");
        }
        for (u32 j = 0; j < PAGE_SIZE; ++j)
            ((u8 *)(usize)stack_phys)[j] = 0;
        if (paging_map_user(pml4, USER_STACK_TOP - (i + 1) * PAGE_SIZE,
                            stack_phys, USER_PAGE_FLAGS) != 0) {
            paging_destroy_user_space(pml4);
            panic("task_create_user_elf: failed to map user stack");
        }
    }

    t = task_alloc(name, pml4);
    t->user = 1;
    t->user_entry = entry;
    t->user_stack = USER_STACK_TOP - 8;   /* ABI: rsp % 16 == 8 at entry */
    (void)vma_add(t, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                  USER_STACK_TOP, VM_READ | VM_WRITE | VM_ANON);
    task_setup_std_fds(t);
    return t;
}

struct task *task_fork(struct task *parent, registers_t *regs)
{
    u64 pml4;
    struct task *t;
    registers_t *frame;

    if (parent == NULL || regs == NULL || !parent->user)
        return NULL;

    pml4 = paging_create_kernel_pml4();
    if (pml4 == 0)
        return NULL;

    /* Detach the parent user root copied by paging_create_kernel_pml4,
     * then deep-copy the parent user pages into the new PML4. */
    paging_clear_user_root(pml4);
    if (paging_copy_user_space(pml4, parent->cr3) != 0) {
        paging_destroy_user_space(pml4);
        return NULL;
    }

    t = task_alloc("fork-child", pml4);
    t->user = 1;
    t->user_entry = parent->user_entry;
    t->user_stack = parent->user_stack;
    t->parent = parent;
    t->mmap_next = parent->mmap_next;
    if (vma_copy(t, parent) != 0)
        kprintf("[vma] WARNING: fork VMA copy failed\n");

    for (u32 fd = 0; fd < MAX_FDS; ++fd) {
        if (parent->fd_table[fd] != NULL) {
            t->fd_table[fd] = parent->fd_table[fd];
            file_get(t->fd_table[fd]);
        }
    }

    frame = (registers_t *)kmalloc(sizeof(registers_t));
    if (frame == NULL)
        panic("task_fork: out of memory for register frame");
    *frame = *regs;
    frame->rax = 0;   /* fork() returns 0 in the child */
    t->fork_regs = frame;

    t->cpu = parent->cpu;
    if (t->cpu != 0)
        percpu_enqueue_task(t->cpu, t);

    kprintf("[fork] pid=%lu -> child pid=%lu cpu=%u\n",
            parent->pid, t->pid, t->cpu);
    return t;
}

void task_bootstrap(void)
{
    struct task *t = task_current();

    if (t->fork_regs != NULL) {
        registers_t *fr = (registers_t *)t->fork_regs;
        registers_t local = *fr;

        t->fork_regs = NULL;
        kfree(fr);
        kprintf("[fork] child pid=%lu resumes in Ring 3 (rax=0)\n",
                t->pid);
        user_enter_regs(&local);
    }

    if (t->user) {
        kprintf("[M3b] entering Ring 3: %s pid=%lu entry=%p stack=%p\n",
                t->name, t->pid, (void *)t->user_entry,
                (void *)t->user_stack);
        user_enter(t->user_entry, t->user_stack);
    }

    kprintf("[M3] scheduler running: kernel thread %s (pid=%lu) started\n",
            t->name, t->pid);

    t->entry();

    t->state = TASK_DEAD;
    for (;;)
        schedule();
}

/* ---------------- process lifecycle (M3c-1) ---------------- */

struct task *task_find_child_pid(struct task *parent, u64 pid)
{
    struct task *t = task_head->next;

    while (t != task_head) {
        if (t->parent == parent && t->pid == pid)
            return t;
        t = t->next;
    }
    return NULL;
}

struct task *task_find_zombie_child(struct task *parent)
{
    struct task *t = task_head->next;

    while (t != task_head) {
        if (t->parent == parent && t->state == TASK_ZOMBIE)
            return t;
        t = t->next;
    }

    return NULL;
}

int task_has_children(struct task *parent)
{
    struct task *t = task_head->next;

    while (t != task_head) {
        if (t->parent == parent && t->state != TASK_ZOMBIE &&
            t->state != TASK_DEAD)
            return 1;
        t = t->next;
    }

    return 0;
}

static void task_finish_reap(struct task *t)
{
    task_fd_close_all(t);
    vma_free_all(t);

    tlb_shootdown_all(t->cr3);
    if (t->cr3 != 0) {
        if (t->user)
            paging_destroy_user_space(t->cr3);
        else
            pmm_free_page(t->cr3);
    }

    if (t->kernel_stack != NULL)
        pmm_free_pages((u64)(usize)t->kernel_stack, TASK_STACK_PAGES);
    if (t->fork_regs != NULL)
        kfree(t->fork_regs);

    kfree(t);
}

void task_reap(struct task *t, const char *reason)
{
    struct task *prev;

    if (t == NULL || t == task_head || t == task_current())
        return;

    kprintf("[reap] %s: pid=%lu name=%s exit=%lu\n",
            reason, t->pid, t->name, t->exit_code);

    /* Any children become orphans; the idle reaper will take care of them. */
    struct task *c = task_head;
    do {
        if (c->parent == t)
            c->parent = NULL;
        c = c->next;
    } while (c != task_head);

    prev = task_head;
    while (prev->next != t && prev->next != task_head)
        prev = prev->next;
    if (prev->next != t)
        return;

    prev->next = t->next;
    {
        u64 flags;

        spin_lock_irqsave(&run_queue_lock, &flags);
        if (total_tasks > 0)
            total_tasks--;
        spin_unlock_irqrestore(&run_queue_lock, flags);
    }

    /* If an AP still owns the address space, defer the destructive part. */
    if (__atomic_load_n(&t->active_cpus, __ATOMIC_SEQ_CST) != 0) {
        u64 flags;

        t->rq_next = NULL;
        spin_lock_irqsave(&deferred_lock, &flags);
        t->rq_next = deferred_head;
        deferred_head = t;
        spin_unlock_irqrestore(&deferred_lock, flags);
        kprintf("[reap] pid=%lu deferred (active_cpus=%u)\n",
                t->pid, (u32)__atomic_load_n(&t->active_cpus, __ATOMIC_SEQ_CST));
        return;
    }

    task_finish_reap(t);
}

void task_reap_deferred(void)
{
    struct task *done = NULL;
    u64 flags;

    spin_lock_irqsave(&deferred_lock, &flags);
    struct task *t = deferred_head;
    deferred_head = NULL;
    while (t != NULL) {
        struct task *next = t->rq_next;

        t->rq_next = NULL;
        if (__atomic_load_n(&t->active_cpus, __ATOMIC_SEQ_CST) == 0) {
            t->rq_next = done;
            done = t;
        } else {
            /* push back */
            t->rq_next = deferred_head;
            deferred_head = t;
        }
        t = next;
    }
    spin_unlock_irqrestore(&deferred_lock, flags);

    while (done != NULL) {
        struct task *next = done->rq_next;
        task_finish_reap(done);
        done = next;
    }
}

void task_reap_orphans(void)
{
    u64 flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");

    for (;;) {
        struct task *t = task_head->next;
        struct task *found = NULL;

        while (t != task_head) {
            if (t->state == TASK_ZOMBIE && t->parent == NULL) {
                found = t;
                break;
            }
            t = t->next;
        }

        if (found == NULL)
            break;
        task_reap(found, "orphan");
    }

    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory");
}

/* ---------------- scheduling ---------------- */

static struct task *pick_next(struct task *prev)
{
    struct task *t = prev->next;

    for (u64 i = 0; i < total_tasks + 1; ++i) {
        if (t->cpu == 0 &&
            (t->state == TASK_READY || t->state == TASK_RUNNING))
            return t;
        t = t->next;
    }

    return prev;
}

static void ap_schedule(void)
{
    struct per_cpu *pc = percpu_self();
    struct task *prev = task_current();
    struct task *next;
    u32 cpu = percpu_apic_id();

    if (!ready || prev == NULL || pc == NULL)
        return;

    next = percpu_dequeue_task(cpu);
    if (next == NULL) {
        if (prev->state == TASK_RUNNING && prev != pc->idle_task)
            return;
        next = pc->idle_task;
    }
    if (next == NULL || next == prev)
        return;

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        if (prev->user)
            __atomic_sub_fetch(&prev->active_cpus, 1, __ATOMIC_SEQ_CST);
        if (prev != pc->idle_task)
            percpu_enqueue_task(cpu, prev);
    }
    if (next->user)
        __atomic_add_fetch(&next->active_cpus, 1, __ATOMIC_SEQ_CST);
    next->state = TASK_RUNNING;
    percpu_set_current(next);
    gdt_set_tss_rsp0_cpu(cpu,
        next->kernel_stack != NULL
            ? (u64)(next->kernel_stack + next->stack_size) : 0);
    if (next->cr3 != 0 && next->cr3 != paging_read_cr3())
        paging_write_cr3(next->cr3);
    switch_to(prev, next);
}

void schedule(void)
{
    struct task *prev;
    struct task *next;
    u64 flags;

    if (!ready)
        return;

    if (percpu_apic_id() != 0) {
        ap_schedule();
        return;
    }

    spin_lock_irqsave(&run_queue_lock, &flags);
    prev = current_task;
    next = pick_next(prev);

    if (next == prev) {
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_RUNNING;
        spin_unlock_irqrestore(&run_queue_lock, flags);
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;
    percpu_set_current(next);
    spin_unlock_irqrestore(&run_queue_lock, flags);

    if (next->cr3 != 0 && next->cr3 != paging_read_cr3())
        paging_write_cr3(next->cr3);

    gdt_set_tss_rsp0(next->kernel_stack != NULL
                         ? (u64)(next->kernel_stack + next->stack_size)
                         : boot_rsp0);

    switch_to(prev, next);
}

void task_yield(void)
{
    schedule();
}

struct task *task_current(void)
{
    struct task *t = percpu_current();
    return t != NULL ? t : current_task;
}

/* Critical section shared by "wait: scan children + set WAITING" and
 * "exit: set ZOMBIE + wake parent". Holding it in both places removes the
 * lost-wakeup window between the parent's scan and its sleep. Callers must
 * NOT do slow work (copy_to_user / task_reap) while holding it. */
u64 task_wait_enter(void)
{
    u64 flags;

    spin_lock_irqsave(&run_queue_lock, &flags);
    return flags;
}

void task_wait_leave(u64 flags)
{
    spin_unlock_irqrestore(&run_queue_lock, flags);
}

void task_wakeup(struct task *t)
{
    if (t == NULL)
        return;
    if (t->state != TASK_WAITING && t->state != TASK_STOPPED)
        return;
    t->state = TASK_READY;
    t->stop_signal = 0;
    if (t->cpu != 0 && !t->on_rq)
        percpu_enqueue_task(t->cpu, t);
}

u64 task_count(void)
{
    u64 flags;
    u64 n;

    spin_lock_irqsave(&run_queue_lock, &flags);
    n = total_tasks;
    spin_unlock_irqrestore(&run_queue_lock, flags);
    return n;
}


void task_exit_current(int status)
{
    struct task *t = task_current();

    if (t->user && percpu_apic_id() != 0)
        __atomic_sub_fetch(&t->active_cpus, 1, __ATOMIC_SEQ_CST);
    {
        u64 flags;

        spin_lock_irqsave(&run_queue_lock, &flags);
        t->exit_code = (u64)status;
        t->state = TASK_ZOMBIE;
        spin_unlock_irqrestore(&run_queue_lock, flags);
    }
    kprintf("[proc] exit: pid=%lu name=%s cpu=%u status=%lu -> ZOMBIE\n",
            t->pid, t->name, percpu_apic_id(), t->exit_code);

    task_wakeup(t->parent);

    for (;;)
        schedule();
}

static struct task *task_lookup_pid(u64 pid)
{
    struct task *t = task_head->next;

    while (t != task_head) {
        if (t->pid == pid)
            return t;
        t = t->next;
    }
    return NULL;
}

int task_setpgid(u64 pid, u64 pgid)
{
    struct task *t = (pid == 0) ? task_current() : task_lookup_pid(pid);

    if (t == NULL || !t->user)
        return -EINVAL;
    if (pgid == 0)
        pgid = t->pid;
    t->pgid = pgid;
    return 0;
}

int task_detach(struct task *parent, u64 pid)
{
    struct task *t = task_lookup_pid(pid);

    if (t == NULL || t->parent != parent)
        return -EINVAL;
    t->parent = NULL;
    return 0;
}

void task_stop_current(int sig)
{
    struct task *t = current_task;

    {
        u64 flags;

        spin_lock_irqsave(&run_queue_lock, &flags);
        t->stop_signal = (u32)sig;
        t->state = TASK_STOPPED;
        spin_unlock_irqrestore(&run_queue_lock, flags);
    }
    kprintf("[proc] stop: pid=%lu name=%s signal=%d -> STOPPED\n",
            t->pid, t->name, sig);

    task_wakeup(t->parent);

    while (t->state == TASK_STOPPED)
        schedule();
}

void task_signal_pgrp(u64 pgid, int sig)
{
    struct task *t;

    if (pgid == 0)
        return;

    t = task_head->next;
    while (t != task_head) {
        if (t->pgid == pgid && t->user &&
            t->state != TASK_ZOMBIE && t->state != TASK_DEAD) {
            if (sig == SIGCONT && t->state == TASK_STOPPED) {
                task_wakeup(t);
            } else if (sig != SIGCONT) {
                signal_send(t, sig);
                task_wakeup(t);
            }
        }
        t = t->next;
    }
}
