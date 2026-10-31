/*
 * Lantest OS - minimal signal support (M4-10)
 *
 * Only SIGPIPE exists for now, and its default action is to terminate the
 * process (exit status 128 + signal). Signal delivery happens at the end of
 * a syscall, after the kernel has set the return value but before returning
 * to Ring 3.
 */
#include <types.h>
#include <kprintf.h>
#include <signal.h>
#include <task.h>

void signal_send(struct task *t, int sig)
{
    if (t == NULL || sig <= 0 || sig > 31)
        return;
    t->signal_pending |= (1u << (sig - 1));
}

void signal_deliver_pending(registers_t *regs)
{
    struct task *t = task_current();
    (void)regs;

    if (t == NULL || t->signal_pending == 0)
        return;

    for (int sig = 1; sig <= 31; ++sig) {
        u32 mask = 1u << (sig - 1);

        if ((t->signal_pending & mask) == 0)
            continue;

        t->signal_pending &= ~mask;

        if (sig == SIGCONT) {
            if (t->state == TASK_STOPPED) {
                t->state = TASK_READY;
                t->stop_signal = 0;
            }
            continue;
        }
        if (sig == SIGTSTP) {
            task_stop_current(sig);
            continue;
        }

        kprintf("[signal] pid=%lu name=%s signal=%d -> terminate (status=%d)\n",
                t->pid, t->name, sig, 128 + sig);
        task_exit_current(128 + sig);
    }
}
