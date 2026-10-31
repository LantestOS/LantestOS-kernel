/*
 * Lantest OS - VFS file objects and per-task file descriptor tables
 *
 * These structures are original to Lantest OS: they follow the public idea
 * of "file operations + fd table" but do not copy Linux layouts or ABI.
 */
#pragma once

#include <types.h>

#define MAX_FDS 32u

#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_RDWR   2u
#define O_CREAT  0x40u
#define O_TRUNC  0x200u

struct task;   /* forward declaration, see include/task.h */
struct file;
struct inode;

struct file_operations {
    i64 (*read)(struct file *file, void *buf, u64 count);
    i64 (*write)(struct file *file, const void *buf, u64 count);
    i64 (*ioctl)(struct file *file, u32 cmd, u64 arg);
    void (*release)(struct file *file);   /* called when refcount hits 0 */
};

struct file {
    const struct file_operations *f_op;
    void *private_data;
    struct inode *f_inode;  /* VFS inode for page cache */
    u64 f_pos;
    u32 f_flags;
    u32 refcount;
};

/* Allocate/free a file object (refcount starts at 1). */
struct file *file_alloc(const struct file_operations *fops, u32 flags,
                        void *private_data);
void file_get(struct file *file);
void file_close(struct file *file);

/* Per-task fd table operations. */
int task_fd_install(struct task *t, struct file *file);
struct file *task_fd_get(struct task *t, int fd);
int task_fd_close(struct task *t, int fd);
int task_fd_dup2(struct task *t, int oldfd, int newfd);
void task_fd_close_all(struct task *t);

/* Install fd 0/1/2 (stdin/stdout/stderr) on a new user task. */
void task_setup_std_fds(struct task *t);
