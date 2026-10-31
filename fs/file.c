/*
 * Lantest OS - file objects and per-task fd tables (M4 step 1/4)
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>
#include <kmalloc.h>
#include <errno.h>
#include <fs.h>
#include <vfs.h>
#include <console.h>
#include <task.h>

struct file *file_alloc(const struct file_operations *fops, u32 flags,
                        void *private_data)
{
    struct file *f = (struct file *)kmalloc(sizeof(struct file));

    if (f == NULL)
        return NULL;

    f->f_op = fops;
    f->private_data = private_data;
    f->f_inode = NULL;
    f->f_pos = 0;
    f->f_flags = flags;
    f->refcount = 1;
    return f;
}

void file_get(struct file *file)
{
    if (file != NULL && file->refcount > 0)
        file->refcount++;
}

void file_close(struct file *file)
{
    if (file == NULL)
        return;

    if (file->refcount > 0)
        file->refcount--;

    if (file->refcount > 0)
        return;

    if (file->f_op != NULL && file->f_op->release != NULL) {
        int inode_file = (file->f_inode != NULL);

        if (inode_file)
            vfs_io_lock();
        file->f_op->release(file);
        if (inode_file)
            vfs_io_unlock();
    }

    kfree(file);
}

int task_fd_install(struct task *t, struct file *file)
{
    if (t == NULL || file == NULL)
        return -EINVAL;

    for (u32 fd = 0; fd < MAX_FDS; ++fd) {
        if (t->fd_table[fd] == NULL) {
            t->fd_table[fd] = file;
            return (int)fd;
        }
    }

    return -EBADF;
}

struct file *task_fd_get(struct task *t, int fd)
{
    if (t == NULL || fd < 0 || fd >= (int)MAX_FDS)
        return NULL;
    return t->fd_table[fd];
}

int task_fd_dup2(struct task *t, int oldfd, int newfd)
{
    struct file *file;

    if (t == NULL || newfd < 0 || newfd >= (int)MAX_FDS)
        return -EBADF;

    file = task_fd_get(t, oldfd);
    if (file == NULL)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;

    file_get(file);
    if (t->fd_table[newfd] != NULL)
        file_close(t->fd_table[newfd]);
    t->fd_table[newfd] = file;
    return newfd;
}

int task_fd_close(struct task *t, int fd)
{
    struct file *file = task_fd_get(t, fd);

    if (file == NULL)
        return -EBADF;

    t->fd_table[fd] = NULL;
    file_close(file);
    return 0;
}

void task_fd_close_all(struct task *t)
{
    if (t == NULL)
        return;

    for (u32 fd = 0; fd < MAX_FDS; ++fd) {
        if (t->fd_table[fd] != NULL) {
            file_close(t->fd_table[fd]);
            t->fd_table[fd] = NULL;
        }
    }
}

void task_setup_std_fds(struct task *t)
{
    struct file *in;
    struct file *out;
    struct file *err;

    in = file_alloc(&console_fops, O_RDONLY, NULL);
    out = file_alloc(&console_fops, O_WRONLY, NULL);
    err = file_alloc(&console_fops, O_WRONLY, NULL);

    if (in == NULL || out == NULL || err == NULL) {
        file_close(in);
        file_close(out);
        file_close(err);
        panic("task_setup_std_fds: out of memory");
    }

    if (task_fd_install(t, in) != 0 ||
        task_fd_install(t, out) != 1 ||
        task_fd_install(t, err) != 2) {
        task_fd_close_all(t);
        panic("task_setup_std_fds: fd 0/1/2 unavailable");
    }
}
