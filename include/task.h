/*
 * Lantest OS - kernel tasks and round-robin scheduler
 */
#pragma once

#include <types.h>
#include <fs.h>
#include <x86_64/isr.h>

struct vm_area;

enum task_state {
    TASK_READY = 1,
    TASK_RUNNING,
    TASK_WAITING,     /* blocked in wait() */
    TASK_ZOMBIE,      /* exited, waiting for parent/reaper */
    TASK_DEAD,
    TASK_STOPPED,     /* stopped by SIGTSTP/SIGSTOP */
};

#define TASK_NAME_MAX 16

struct task {
    u64 pid;
    u64 rsp;                 /* saved kernel stack pointer; MUST stay at offset 8 */
    u8 fpu_state[512] __attribute__((aligned(16))); /* FXSAVE area at offset 16 */
    u64 cr3;                 /* per-task PML4 physical address */
    u8 *kernel_stack;        /* base of the task kernel stack */
    u64 stack_size;
    void (*entry)(void);     /* kernel-thread entry (NULL for user tasks) */
    char name[TASK_NAME_MAX];
    u32 state;
    u32 ticks;
    u32 user;                /* 1 = Ring 3 user task */
    u32 reserved;
    u32 signal_pending;  /* bitmask: 1 << (sig-1) */
    u64 pgid;            /* process group id */
    u32 stop_signal;     /* signal that stopped the task */
    struct vm_area *vmas;/* virtual memory areas */
    u64 mmap_next;       /* next anonymous mmap address */
    u64 user_entry;          /* user RIP (for user tasks) */
    u64 user_stack;          /* user RSP (for user tasks) */
    void *fork_regs;         /* saved user registers for a fork() child */
    struct task *wait_next;  /* link for pipe wait queues */
    struct task *parent;     /* parent process (NULL = orphan / boot) */
    u64 exit_code;           /* valid once state == TASK_ZOMBIE */
    volatile u32 active_cpus;/* APs currently running this task */
    struct file *fd_table[MAX_FDS];   /* per-task file descriptors */
    struct task *rq_next;    /* per-CPU ready queue link */
    struct task *defer_next; /* deferred reap list link (never rq_next!) */
    u32 cpu;                 /* target CPU */
    u32 on_rq;
    struct task *next;
};

void scheduler_init(void);
int  scheduler_ready(void);

/* Create a kernel thread (Ring 0) that starts at entry(). */
struct task *task_create_kernel(const char *name, void (*entry)(void));

/*
 * Create a Ring 3 task from an embedded ELF64 image: allocates a PML4, loads
 * PT_LOAD segments page-wise via elf_load_user(), maps one user stack page,
 * sets up the iretq entry (e_entry), and installs fd 0/1/2 on the console.
 */
struct task *task_create_user_elf(const char *name, const u8 *elf, u64 size);

/*
 * fork(): deep-copy the parent's user address space, clone the fd table
 * (refcount++), allocate a new kernel stack, and record a copy of the
 * parent's registers with rax=0 for the child.
 */
struct task *task_fork(struct task *parent, registers_t *regs);

void schedule(void);
void task_yield(void);
void task_wakeup(struct task *t);
u64 task_wait_enter(void);
void task_wait_leave(u64 flags);

struct task *task_current(void);
u64 task_count(void);

/* ---- process lifecycle (M3c-1) ---- */
struct task *task_find_zombie_child(struct task *parent);
int  task_has_children(struct task *parent);

/* Reap one zombie: close fds, free user space/PML4/kernel stack/task struct. */
void task_reap(struct task *t, const char *reason);

/* Reap zombies that have no parent (orphans); called from the idle task. */
void task_reap_orphans(void);
void task_reap_deferred(void);

/* C side of the first-entry trampoline used by arch/x86_64/switch.S. */
void task_bootstrap(void);

/* Exit the current task (used by exit() and signal default action). */
void task_exit_current(int status) __attribute__((noreturn));

int task_setpgid(u64 pid, u64 pgid);
int task_detach(struct task *parent, u64 pid);
void task_signal_pgrp(u64 pgid, int sig);
struct task *task_find_child_pid(struct task *parent, u64 pid);
void task_stop_current(int sig);
