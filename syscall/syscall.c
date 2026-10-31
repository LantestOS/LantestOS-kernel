/*
 * Lantest OS - custom syscall handler (int 0x80)
 *
 * Independent ABI: rax = syscall number, rdi/rsi/rdx = arguments, return
 * value goes back into the interrupt frame's rax. No Linux syscall numbers
 * or structures are used.
 *
 * M4-1: write(fd, buf, count) via the per-task fd table.
 * M4-2: open/read/close against the ramfs VFS.
 * M4-3: mkdir, directory read, console input.
 * M4-4: fork (deep copy), execve (replace user space from ramfs ELF),
 *       unlink (delete-on-close).
 */
#include <types.h>
#include <kprintf.h>
#include <errno.h>
#include <fs.h>
#include <vfs.h>
#include <pipe.h>
#include <signal.h>
#include <console.h>
#include <vma.h>
#include <page_cache.h>
#include <elf.h>
#include <kmalloc.h>
#include <pmm.h>
#include <task.h>
#include <percpu.h>
#include <uaccess.h>
#include <x86_64/isr.h>
#include <x86_64/paging.h>
#include <x86_64/irq.h>
#include <x86_64/usermode.h>
#include <syscall.h>

#define EXEC_MAX_IMAGE (256u * 1024u)

/*
 * Copy a NUL-terminated path from user space, one byte at a time, so an
 * unmapped user page is recovered by the copy fixup instead of panicking.
 */
static int copy_user_path(char *dst, u64 user_ptr, u32 max)
{
    for (u32 i = 0; i < max; ++i) {
        char c;

        if (copy_from_user(&c, (const void *)(usize)(user_ptr + i), 1) != 0)
            return -EFAULT;
        dst[i] = c;
        if (c == '\0')
            return 0;
    }

    dst[max - 1] = '\0';
    return -EINVAL;
}

static void zero_page_phys(u64 phys)
{
    u8 *p = (u8 *)(usize)phys;

    for (u32 i = 0; i < 4096; ++i)
        p[i] = 0;
}

#define EXEC_MAX_ARGS 16
#define EXEC_MAX_ENVS 8
#define EXEC_ARG_MAX  128

static char exec_argv[EXEC_MAX_ARGS][EXEC_ARG_MAX];
static char exec_envp[EXEC_MAX_ENVS][EXEC_ARG_MAX];

static int str_len_small(const char *s)
{
    int n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

static int copy_user_cstr(char *dst, u64 user_ptr, u32 max)
{
    for (u32 i = 0; i < max; ++i) {
        char c;

        if (copy_from_user(&c, (const void *)(usize)(user_ptr + i), 1) != 0)
            return -EFAULT;
        dst[i] = c;
        if (c == '\0')
            return 0;
    }
    return -EINVAL;
}

static int collect_user_vec(u64 user_vec, char out[][EXEC_ARG_MAX],
                            int max, int *count)
{
    *count = 0;
    if (user_vec == 0)
        return 0;

    for (int i = 0; i < max; ++i) {
        u64 ptr;

        if (copy_from_user(&ptr, (const void *)(usize)(user_vec + i * 8),
                           8) != 0)
            return -EFAULT;
        if (ptr == 0)
            return 0;
        if (copy_user_cstr(out[i], ptr, EXEC_ARG_MAX) != 0)
            return -EFAULT;
        *count = i + 1;
    }

    return -E2BIG;
}

static int build_user_stack(u64 stack_phys, int argc, int envc, u64 *out_rsp)
{
    u8 *page = (u8 *)(usize)stack_phys;
    const u64 base = USER_STACK_TOP - 4096;
    u64 argv_addr[EXEC_MAX_ARGS];
    u64 env_addr[EXEC_MAX_ENVS];
    u64 off = 4096;
    u64 strings_floor;
    u64 words;
    u64 bytes;
    u64 *sp;

    for (int i = envc - 1; i >= 0; --i) {
        int len = str_len_small(exec_envp[i]) + 1;

        if (off < (u64)len)
            return -1;
        off -= (u64)len;
        for (int j = 0; j < len; ++j)
            page[off + (u64)j] = (u8)exec_envp[i][j];
        env_addr[i] = base + off;
    }

    for (int i = argc - 1; i >= 0; --i) {
        int len = str_len_small(exec_argv[i]) + 1;

        if (off < (u64)len)
            return -1;
        off -= (u64)len;
        for (int j = 0; j < len; ++j)
            page[off + (u64)j] = (u8)exec_argv[i][j];
        argv_addr[i] = base + off;
    }

    off &= ~7ULL;
    strings_floor = off;

    words = 1 + (u64)argc + 1 + (u64)envc + 1;
    bytes = words * 8;
    if (off < bytes)
        return -1;
    off -= bytes;
    if ((off & 15ULL) != 8ULL)
        off -= 8;
    if ((off & 15ULL) != 8ULL || off + bytes > strings_floor)
        return -1;

    sp = (u64 *)(page + off);
    sp[0] = (u64)argc;
    for (int i = 0; i < argc; ++i)
        sp[1 + i] = argv_addr[i];
    sp[1 + argc] = 0;
    for (int i = 0; i < envc; ++i)
        sp[2 + argc + i] = env_addr[i];
    sp[2 + argc + envc] = 0;

    *out_rsp = base + off;
    return 0;
}

static void do_execve(registers_t *regs)
{
    char path[VFS_PATH_MAX];
    u8 *image;
    u64 size = 0;
    u64 new_pml4;
    u64 entry = 0;
    u64 stack_phys;
    u64 user_rsp = 0;
    struct task *t = task_current();
    u64 old_cr3;
    struct file *exec_file;
    int argc = 0;
    int envc = 0;
    int rc;

    rc = copy_user_path(path, regs->rdi, sizeof(path));
    if (rc != 0) {
        regs->rax = (u64)(i64)rc;
        return;
    }

    rc = collect_user_vec(regs->rsi, exec_argv, EXEC_MAX_ARGS, &argc);
    if (rc != 0) {
        regs->rax = (u64)(i64)rc;
        return;
    }
    rc = collect_user_vec(regs->rdx, exec_envp, EXEC_MAX_ENVS, &envc);
    if (rc != 0) {
        regs->rax = (u64)(i64)rc;
        return;
    }

    if (argc == 0) {
        int n = str_len_small(path);
        if (n + 1 > EXEC_ARG_MAX) {
            regs->rax = (u64)(i64)-E2BIG;
            return;
        }
        for (int i = 0; i <= n; ++i)
            exec_argv[0][i] = path[i];
        argc = 1;
    }

    image = (u8 *)kmalloc(EXEC_MAX_IMAGE);
    if (image == NULL) {
        regs->rax = (u64)(i64)-ENOMEM;
        return;
    }

    rc = vfs_read_file(path, image, EXEC_MAX_IMAGE, &size);
    if (rc != 0) {
        kfree(image);
        regs->rax = (u64)(i64)rc;
        return;
    }

    new_pml4 = paging_create_kernel_pml4();
    if (new_pml4 == 0) {
        kfree(image);
        regs->rax = (u64)(i64)-ENOMEM;
        return;
    }

    paging_clear_user_root(new_pml4);

    exec_file = vfs_open(path, O_RDONLY);
    if (exec_file == NULL) {
        paging_destroy_user_space(new_pml4);
        kfree(image);
        regs->rax = (u64)(i64)-ENOENT;
        return;
    }

    vma_free_all(t);
    t->mmap_next = USER_BASE + 0x80000ULL;

    if (elf_load_vmas(t, exec_file, image, size, &entry) != 0) {
        file_close(exec_file);
        paging_destroy_user_space(new_pml4);
        kfree(image);
        regs->rax = (u64)(i64)-ENOENT;
        return;
    }
    file_close(exec_file);

    stack_phys = 0;
    for (u32 i = 0; i < USER_STACK_PAGES; ++i) {
        u64 sp = pmm_alloc_page();

        if (sp == 0) {
            paging_destroy_user_space(new_pml4);
            kfree(image);
            regs->rax = (u64)(i64)-ENOMEM;
            return;
        }
        zero_page_phys(sp);
        if (paging_map_user(new_pml4, USER_STACK_TOP - (i + 1) * PAGE_SIZE,
                            sp, USER_PAGE_FLAGS) != 0) {
            pmm_free_page(sp);
            paging_destroy_user_space(new_pml4);
            kfree(image);
            regs->rax = (u64)(i64)-ENOMEM;
            return;
        }
        if (i == 0)
            stack_phys = sp;
    }
    (void)vma_add(t, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                  USER_STACK_TOP, VM_READ | VM_WRITE | VM_ANON);

    if (build_user_stack(stack_phys, argc, envc, &user_rsp) != 0) {
        paging_destroy_user_space(new_pml4);
        kfree(image);
        regs->rax = (u64)(i64)-E2BIG;
        return;
    }

    old_cr3 = t->cr3;
    t->cr3 = new_pml4;
    t->user = 1;
    t->user_entry = entry;
    t->user_stack = user_rsp;

    if (t->fork_regs != NULL) {
        kfree(t->fork_regs);
        t->fork_regs = NULL;
    }

    paging_write_cr3(new_pml4);
    paging_destroy_user_space(old_cr3);
    kfree(image);

    kprintf("[exec] pid=%lu -> %s argc=%d env=%d entry=%p\n",
            t->pid, path, argc, envc, (void *)entry);

    regs->rip = entry;
    regs->rsp = user_rsp;
    regs->rax = 0;
}

void syscall_handler(registers_t *regs)
{
    u64 nr = regs->rax;

    switch (nr) {
    case SYS_WRITE: {
        int fd = (int)regs->rdi;
        u64 ptr = regs->rsi;
        u64 len = regs->rdx;
        struct file *file = task_fd_get(task_current(), fd);
        u8 kbuf[128];
        u64 done = 0;
        int failed = 0;

        if (file == NULL || file->f_op == NULL ||
            file->f_op->write == NULL) {
            regs->rax = (u64)(i64)-EBADF;
            break;
        }

        while (done < len) {
            u64 chunk = len - done;

            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);

            if (copy_from_user(kbuf, (const void *)(usize)(ptr + done),
                               chunk) != 0) {
                failed = -EFAULT;
                break;
            }

            if (file->f_inode != NULL)
                vfs_io_lock();
            i64 written = file->f_op->write(file, kbuf, chunk);
            if (file->f_inode != NULL)
                vfs_io_unlock();
            if (written < 0) {
                failed = (int)written;
                break;
            }
            if ((u64)written != chunk) {
                failed = -EINVAL;
                break;
            }

            done += chunk;
        }

        regs->rax = failed ? (u64)(i64)failed : len;
        break;
    }

    case SYS_OPEN: {
        char path[VFS_PATH_MAX];
        int rc = copy_user_path(path, regs->rdi, sizeof(path));
        struct file *file;
        int fd;

        if (rc != 0) {
            regs->rax = (u64)(i64)rc;
            break;
        }

        file = vfs_open(path, (u32)regs->rsi);
        if (file == NULL) {
            regs->rax = (u64)(i64)-ENOENT;
            break;
        }

        fd = task_fd_install(task_current(), file);
        if (fd < 0) {
            file_close(file);
            regs->rax = (u64)(i64)-EMFILE;
            break;
        }

        regs->rax = (u64)fd;
        break;
    }

    case SYS_READ: {
        int fd = (int)regs->rdi;
        u64 user_buf = regs->rsi;
        u64 len = regs->rdx;
        struct file *file = task_fd_get(task_current(), fd);
        u8 kbuf[256];
        u64 total = 0;
        int failed = 0;

        if (file == NULL || file->f_op == NULL ||
            file->f_op->read == NULL) {
            regs->rax = (u64)(i64)-EBADF;
            break;
        }

        while (total < len) {
            u64 chunk = len - total;
            i64 n;

            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);

            if (file->f_inode != NULL)
                vfs_io_lock();
            n = file->f_op->read(file, kbuf, chunk);
            if (file->f_inode != NULL)
                vfs_io_unlock();
            if (n < 0) {
                failed = (int)n;
                break;
            }
            if (n == 0)
                break;   /* EOF */

            if (copy_to_user((void *)(usize)(user_buf + total), kbuf,
                             (u64)n) != 0) {
                failed = -EFAULT;
                break;
            }

            total += (u64)n;
            if ((u64)n < chunk)
                break;
        }

        regs->rax = failed ? (u64)(i64)failed : total;
        break;
    }

    case SYS_PIPE: {
        struct pipe *p = pipe_alloc();
        struct file *rf;
        struct file *wf;
        int rfd = -1;
        int wfd = -1;
        int fds[2];

        if (p == NULL) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        rf = pipe_make_file(p, 0);
        wf = pipe_make_file(p, 1);
        if (rf == NULL || wf == NULL) {
            file_close(rf);
            file_close(wf);
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }

        rfd = task_fd_install(task_current(), rf);
        if (rfd >= 0)
            wfd = task_fd_install(task_current(), wf);
        if (rfd < 0 || wfd < 0) {
            if (rfd >= 0) task_fd_close(task_current(), rfd);
            else file_close(rf);
            if (wfd >= 0) task_fd_close(task_current(), wfd);
            else file_close(wf);
            regs->rax = (u64)(i64)-EMFILE;
            break;
        }

        fds[0] = rfd;
        fds[1] = wfd;
        if (copy_to_user((void *)(usize)regs->rdi, fds, sizeof(fds)) != 0) {
            task_fd_close(task_current(), rfd);
            task_fd_close(task_current(), wfd);
            regs->rax = (u64)(i64)-EFAULT;
            break;
        }
        regs->rax = 0;
        break;
    }

    case SYS_DUP2: {
        int rc = task_fd_dup2(task_current(), (int)regs->rdi,
                              (int)regs->rsi);
        regs->rax = rc < 0 ? (u64)(i64)rc : (u64)rc;
        break;
    }

    case SYS_SETPGID: {
        int rc = task_setpgid((u64)regs->rdi, (u64)regs->rsi);
        regs->rax = rc < 0 ? (u64)(i64)rc : 0;
        break;
    }

    case SYS_SET_FG:
        tty_set_foreground_pgid((u64)regs->rdi);
        regs->rax = 0;
        break;

    case SYS_DETACH: {
        int rc = task_detach(task_current(), (u64)regs->rdi);
        regs->rax = rc < 0 ? (u64)(i64)rc : 0;
        break;
    }

    case SYS_KILL:
        task_signal_pgrp((u64)regs->rdi, (int)regs->rsi);
        regs->rax = 0;
        break;

    case SYS_WAITPID: {
        struct task *self = task_current();
        u64 want = regs->rdi;
        u64 status_ptr = regs->rsi;
        u64 flags = task_wait_enter();

        for (;;) {
            struct task *child = task_find_child_pid(self, want);
            u64 code;
            u64 child_pid;

            if (child == NULL) {
                task_wait_leave(flags);
                regs->rax = (u64)-1;
                break;
            }
            if (child->state == TASK_ZOMBIE ||
                child->state == TASK_STOPPED) {
                int zombie = (child->state == TASK_ZOMBIE);

                child_pid = child->pid;
                code = zombie ? child->exit_code
                              : (0x7f | ((u64)child->stop_signal << 8));
                /* Drop the scheduler lock before copy_to_user/task_reap:
                 * both are slow and task_reap takes this same lock. */
                task_wait_leave(flags);
                if (status_ptr != 0 &&
                    copy_to_user((void *)(usize)status_ptr, &code,
                                 sizeof(code)) != 0) {
                    regs->rax = (u64)(i64)-EFAULT;
                    break;
                }
                if (zombie)
                    task_reap(child, "waitpid");
                regs->rax = child_pid;
                break;
            }
            self->state = TASK_WAITING;
            task_wait_leave(flags);
            schedule();
            flags = task_wait_enter();
        }
        break;
    }

    case SYS_MMAP: {
        struct task *self = task_current();
        u64 size = regs->rdi;
        u64 pages;
        u64 total;
        u64 start;
        u64 end;

        if (size == 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        total = pages * PAGE_SIZE;
        start = (self->mmap_next + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        end = start + total;
        if (end < start || end > USER_STACK_TOP - 0x10000ULL) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }

        if (vma_add(self, start, end, VM_READ | VM_WRITE | VM_ANON) != 0) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        self->mmap_next = end;
        regs->rax = start;
        break;
    }

    case SYS_MMAP_FILE: {
        struct task *self = task_current();
        int fd = (int)regs->rdi;
        u64 off = regs->rsi;
        u64 len = regs->rdx;
        struct file *file = task_fd_get(self, fd);
        u64 start;
        u64 end;

        if (file == NULL) {
            regs->rax = (u64)(i64)-EBADF;
            break;
        }
        if (len == 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        start = (self->mmap_next + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        end = start + ((len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1));
        if (end < start || end > USER_STACK_TOP - 0x10000ULL) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        if (vma_add_file(self, start, end,
                         VM_READ | VM_WRITE | VM_FILE, file,
                         off, off + len) != 0) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        self->mmap_next = end;
        regs->rax = start;
        break;
    }

    case SYS_MUNMAP: {
        struct task *self = task_current();
        u64 start = regs->rdi;
        u64 len = regs->rsi;
        u64 end;

        if ((start & (PAGE_SIZE - 1)) != 0 || len == 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        end = start + ((len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1));
        if (end < start) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        paging_unmap_range(self->cr3, start, end);
        (void)vma_unmap_range(self, start, end);
        regs->rax = 0;
        break;
    }

    case SYS_MPROTECT: {
        struct task *self = task_current();
        u64 start = regs->rdi;
        u64 len = regs->rsi;
        u64 prot = regs->rdx;
        u64 end;
        u32 flags = 0;

        if ((start & (PAGE_SIZE - 1)) != 0 || len == 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        end = start + ((len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1));
        if (end < start) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        if ((prot & 1u) != 0)
            flags |= VM_READ;
        if ((prot & 2u) != 0)
            flags |= VM_WRITE;
        if ((prot & 4u) != 0)
            flags |= VM_EXEC;
        flags |= VM_READ;
        paging_protect_range(self->cr3, start, end, (prot & 2u) != 0);
        (void)vma_protect_range(self, start, end, flags);
        regs->rax = 0;
        break;
    }

    case SYS_MMAP_SHARED: {
        struct task *self = task_current();
        int fd = (int)regs->rdi;
        u64 off = regs->rsi;
        u64 len = regs->rdx;
        struct file *file = task_fd_get(self, fd);
        u64 start;
        u64 end;

        if (file == NULL) {
            regs->rax = (u64)(i64)-EBADF;
            break;
        }
        if (len == 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        start = (self->mmap_next + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        end = start + ((len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1));
        if (end < start || end > USER_STACK_TOP - 0x10000ULL) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        if (vma_add_file(self, start, end,
                         VM_READ | VM_WRITE | VM_FILE | VM_SHARED, file,
                         off, off + len) != 0) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        self->mmap_next = end;
        regs->rax = start;
        break;
    }

    case SYS_MSYNC: {
        struct task *self = task_current();
        u64 start = regs->rdi;
        u64 len = regs->rsi;
        u64 end;
        struct vm_area *v;

        if (len == 0) {
            regs->rax = 0;
            break;
        }
        end = start + len;
        if (end < start) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        for (v = self->vmas; v != NULL; v = v->next) {
            u64 vs;
            u64 ve;
            u64 foff;
            u64 fend;

            if ((v->flags & VM_SHARED) == 0 || v->file == NULL ||
                v->file->f_inode == NULL)
                continue;
            if (end <= v->start || start >= v->end)
                continue;
            vs = start > v->start ? start : v->start;
            ve = end < v->end ? end : v->end;
            foff = v->file_offset + (vs - v->start);
            fend = v->file_offset + (ve - v->start);
            page_cache_flush(v->file->f_inode,
                             foff / PAGE_SIZE,
                             (fend + PAGE_SIZE - 1) / PAGE_SIZE);
        }
        regs->rax = 0;
        break;
    }

    case SYS_LSEEK: {
        int fd = (int)regs->rdi;
        i64 off = (i64)regs->rsi;
        int whence = (int)regs->rdx;
        struct file *file = task_fd_get(task_current(), fd);
        i64 pos;

        if (file == NULL) {
            regs->rax = (u64)(i64)-EBADF;
            break;
        }
        if (whence == 0) {
            pos = off;
        } else if (whence == 1) {
            pos = (i64)file->f_pos + off;
        } else if (whence == 2) {
            if (file->f_inode == NULL) {
                regs->rax = (u64)(i64)-EINVAL;
                break;
            }
            pos = (i64)file->f_inode->i_size + off;
        } else {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        if (pos < 0) {
            regs->rax = (u64)(i64)-EINVAL;
            break;
        }
        file->f_pos = (u64)pos;
        regs->rax = (u64)pos;
        break;
    }

    case SYS_TIME:
        regs->rax = timer_ticks() * 10u;
        break;

    case SYS_CLOSE: {
        int rc = task_fd_close(task_current(), (int)regs->rdi);

        regs->rax = rc < 0 ? (u64)(i64)rc : 0;
        break;
    }

    case SYS_MKDIR: {
        char path[VFS_PATH_MAX];
        int rc = copy_user_path(path, regs->rdi, sizeof(path));

        if (rc != 0) {
            regs->rax = (u64)(i64)rc;
            break;
        }
        rc = vfs_mkdir(path, (u32)regs->rsi);
        regs->rax = rc < 0 ? (u64)(i64)rc : 0;
        break;
    }

    case SYS_UNLINK: {
        char path[VFS_PATH_MAX];
        int rc = copy_user_path(path, regs->rdi, sizeof(path));

        if (rc != 0) {
            regs->rax = (u64)(i64)rc;
            break;
        }
        rc = vfs_unlink(path);
        regs->rax = rc < 0 ? (u64)(i64)rc : 0;
        break;
    }

    case SYS_FORK: {
        struct task *child = task_fork(task_current(), regs);

        if (child == NULL) {
            regs->rax = (u64)(i64)-ENOMEM;
            break;
        }
        regs->rax = child->pid;
        break;
    }

    case SYS_EXECVE:
        do_execve(regs);
        break;

    case SYS_EXIT:
        task_exit_current((int)regs->rdi);

    case SYS_GETPID:
        regs->rax = task_current()->pid;
        break;

    case SYS_WAIT: {
        struct task *self = task_current();
        u64 status_ptr = regs->rdi;
        u64 flags = task_wait_enter();

        for (;;) {
            struct task *child = task_find_zombie_child(self);

            if (child != NULL) {
                u64 code = child->exit_code;
                u64 child_pid = child->pid;

                task_wait_leave(flags);
                if (status_ptr != 0 &&
                    copy_to_user((void *)(usize)status_ptr, &code,
                                 sizeof(code)) != 0) {
                    regs->rax = (u64)(i64)-EFAULT;
                    break;
                }

                kprintf("[proc] wait: parent pid=%lu reaped child pid=%lu "
                        "status=%lu\n", self->pid, child_pid, code);
                task_reap(child, "wait");
                regs->rax = child_pid;
                break;
            }

            if (!task_has_children(self)) {
                task_wait_leave(flags);
                regs->rax = (u64)-1;
                break;
            }

            self->state = TASK_WAITING;
            task_wait_leave(flags);
            schedule();
            flags = task_wait_enter();
        }
        break;
    }

    default:
        regs->rax = (u64)(i64)-ENOSYS;
        break;
    }

    signal_deliver_pending(regs);
}
