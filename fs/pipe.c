/*
 * Lantest OS - pipe IPC (M4-8)
 *
 * A pipe is a 4 KiB ring buffer plus reader/writer wait queues. Reads block
 * when the buffer is empty and writers exist; writes block when it is full.
 * Closing the last writer wakes readers (EOF); closing the last reader wakes
 * writers (-EPIPE).
 *
 * M4-45: pipe->lock protects rpos/wpos/used/readers/writers and wait queues.
 */
#include <types.h>
#include <kprintf.h>
#include <kmalloc.h>
#include <errno.h>
#include <fs.h>
#include <task.h>
#include <pipe.h>
#include <signal.h>
#include <spinlock.h>

#define PIPE_BUF_SIZE 4096u

struct pipe {
    u8 buf[PIPE_BUF_SIZE];
    u32 rpos;
    u32 wpos;
    u32 used;
    u32 readers;
    u32 writers;
    struct task *read_wait;
    struct task *write_wait;
    spinlock_t lock;
};

static void pipe_wake_all(struct task **queue)
{
    while (*queue != NULL) {
        struct task *t = *queue;

        *queue = t->wait_next;
        t->wait_next = NULL;
        task_wakeup(t);
    }
}

static void pipe_wait(struct pipe *p, struct task **queue)
{
    struct task *t = task_current();
    u64 flags;

    spin_lock_irqsave(&p->lock, &flags);
    t->wait_next = *queue;
    *queue = t;
    t->state = TASK_WAITING;
    spin_unlock_irqrestore(&p->lock, flags);
    schedule();
}

static void pipe_release(struct file *file)
{
    struct pipe *p = (struct pipe *)file->private_data;
    int write = (file->f_flags & O_WRONLY) != 0;
    u64 flags;

    if (p == NULL)
        return;

    spin_lock_irqsave(&p->lock, &flags);
    if (write) {
        if (p->writers > 0)
            p->writers--;
        pipe_wake_all(&p->read_wait);
    } else {
        if (p->readers > 0)
            p->readers--;
        pipe_wake_all(&p->write_wait);
    }

    if (p->readers == 0 && p->writers == 0) {
        spin_unlock_irqrestore(&p->lock, flags);
        kfree(p);
        return;
    }
    spin_unlock_irqrestore(&p->lock, flags);
}

static i64 pipe_read(struct file *file, void *buf, u64 count)
{
    struct pipe *p = (struct pipe *)file->private_data;
    u8 *dst = (u8 *)buf;
    u64 flags;

    if (p == NULL)
        return -EBADF;
    if (count == 0)
        return 0;

    for (;;) {
        if (task_current()->signal_pending & (1u << (SIGTSTP - 1))) {
            task_current()->signal_pending &= ~(1u << (SIGTSTP - 1));
            task_stop_current(SIGTSTP);
            continue;
        }
        if (task_current()->signal_pending != 0)
            return -EINTR;

        spin_lock_irqsave(&p->lock, &flags);
        if (p->used > 0) {
            u32 n = (u32)count;
            if (n > p->used)
                n = p->used;
            for (u32 i = 0; i < n; ++i) {
                dst[i] = p->buf[p->rpos];
                p->rpos = (p->rpos + 1) % PIPE_BUF_SIZE;
            }
            p->used -= n;
            pipe_wake_all(&p->write_wait);
            spin_unlock_irqrestore(&p->lock, flags);
            return (i64)n;
        }

        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }

        spin_unlock_irqrestore(&p->lock, flags);
        pipe_wait(p, &p->read_wait);
    }
}

static i64 pipe_write(struct file *file, const void *buf, u64 count)
{
    struct pipe *p = (struct pipe *)file->private_data;
    const u8 *src = (const u8 *)buf;
    u64 done = 0;
    u64 flags;

    if (p == NULL)
        return -EBADF;

    while (done < count) {
        if (task_current()->signal_pending & (1u << (SIGTSTP - 1))) {
            task_current()->signal_pending &= ~(1u << (SIGTSTP - 1));
            task_stop_current(SIGTSTP);
            continue;
        }
        if (task_current()->signal_pending != 0)
            return -EINTR;

        spin_lock_irqsave(&p->lock, &flags);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            signal_send(task_current(), SIGPIPE);
            return -EPIPE;
        }

        if (p->used == PIPE_BUF_SIZE) {
            spin_unlock_irqrestore(&p->lock, flags);
            pipe_wait(p, &p->write_wait);
            continue;
        }

        {
            u32 space = PIPE_BUF_SIZE - p->used;
            u32 n = (u32)(count - done);
            if (n > space)
                n = space;
            for (u32 i = 0; i < n; ++i) {
                p->buf[p->wpos] = src[done + i];
                p->wpos = (p->wpos + 1) % PIPE_BUF_SIZE;
            }
            p->used += n;
            done += n;
            pipe_wake_all(&p->read_wait);
        }
        spin_unlock_irqrestore(&p->lock, flags);
    }

    return (i64)count;
}

static const struct file_operations pipe_fops = {
    .read = pipe_read,
    .write = pipe_write,
    .ioctl = NULL,
    .release = pipe_release,
};

struct pipe *pipe_alloc(void)
{
    struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));

    if (p == NULL)
        return NULL;

    for (u32 i = 0; i < sizeof(struct pipe); ++i)
        ((u8 *)p)[i] = 0;
    return p;
}

struct file *pipe_make_file(struct pipe *p, int write)
{
    struct file *f;
    u64 flags;

    if (p == NULL)
        return NULL;

    f = file_alloc(&pipe_fops, write ? O_WRONLY : O_RDONLY, p);
    if (f == NULL)
        return NULL;

    spin_lock_irqsave(&p->lock, &flags);
    if (write)
        p->writers++;
    else
        p->readers++;
    spin_unlock_irqrestore(&p->lock, flags);

    return f;
}
