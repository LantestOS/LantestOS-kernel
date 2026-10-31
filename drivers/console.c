/*
 * Lantest OS - console character device
 *
 * M4-1: fd 0/1/2 backend, write -> kputc.
 * M4-3: line-oriented read from COM1 (UART polling), with echo and backspace.
 * UART interrupts are disabled, so the read loop does `sti; hlt` between
 * polls; the 100 Hz PIT wakes it and the scheduler can run other tasks.
 */
#include <types.h>
#include <kprintf.h>
#include <errno.h>
#include <uart.h>
#include <keyboard.h>
#include <fs.h>
#include <console.h>
#include <task.h>
#include <signal.h>

static u64 tty_fg_pgid;

void tty_set_foreground_pgid(u64 pgid)
{
    tty_fg_pgid = pgid;
}

u64 tty_foreground_pgid(void)
{
    return tty_fg_pgid;
}

int tty_send_sigtstp(void)
{
    if (tty_fg_pgid == 0)
        return 0;
    task_signal_pgrp(tty_fg_pgid, SIGTSTP);
    return 1;
}

int tty_send_sigint(void)
{
    if (tty_fg_pgid == 0)
        return 0;
    task_signal_pgrp(tty_fg_pgid, SIGINT);
    return 1;
}

static i64 console_write(struct file *file, const void *buf, u64 count)
{
    const char *p = (const char *)buf;
    (void)file;

    /* Whole buffer under one uart lock hold: no cross-CPU mid-line
     * interleaving on the console, and the kernel log keeps its own
     * kprintf_lock line atomicity. */
    uart_puts(p, count);

    return (i64)count;
}

static i64 console_read(struct file *file, void *buf, u64 count)
{
    char *out = (char *)buf;
    (void)file;

    if (count == 0)
        return 0;
    if (!uart_available())
        return -EINVAL;

    for (;;) {
        char c;

        if (task_current()->signal_pending & (1u << (SIGTSTP - 1))) {
            task_current()->signal_pending &= ~(1u << (SIGTSTP - 1));
            task_stop_current(SIGTSTP);
            continue;
        }
        if (task_current()->signal_pending != 0)
            return -EINTR;

        if (keyboard_getc(&c) || uart_poll_getc(&c)) {
            if (c == '\r')
                c = '\n';
            out[0] = c;
            return 1;
        }

        /* Sleep until the next interrupt (PIT 100 Hz or IRQ1). */
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}

const struct file_operations console_fops = {
    .read = console_read,
    .write = console_write,
    .ioctl = NULL,
};
