/*
 * Lantest OS - minimal user-space syscall wrappers (no libc)
 */
#pragma once

typedef unsigned long u64;
typedef long i64;

#define USER_BASE 0x0000008000000000UL

#define STDIN_FD  0
#define STDOUT_FD 1
#define STDERR_FD 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define VFS_NAME_MAX 64u

#define DT_UNKNOWN 0u
#define DT_REG     1u
#define DT_DIR     2u

struct dirent {
    u64 d_ino;
    unsigned int d_type;
    unsigned int d_pad;
    char d_name[VFS_NAME_MAX];
};

static inline i64 sys_write(int fd, const void *buf, u64 len)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(1), "D"(fd), "S"(buf), "d"(len)
                      : "memory");
    return ret;
}

static inline void sys_exit(i64 code)
{
    __asm__ volatile ("int $0x80" : : "a"(2), "D"(code) : "memory");
    for (;;) {
    }
}

static inline i64 sys_wait(i64 *status)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(4), "D"(status)
                      : "memory");
    return ret;
}

static inline i64 sys_open(const char *path, i64 flags)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(5), "D"(path), "S"(flags)
                      : "memory");
    return ret;
}

static inline i64 sys_read(int fd, void *buf, u64 len)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(6), "D"(fd), "S"(buf), "d"(len)
                      : "memory");
    return ret;
}

static inline i64 sys_close(int fd)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(7), "D"(fd)
                      : "memory");
    return ret;
}

static inline i64 sys_mkdir(const char *path, u64 mode)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(8), "D"(path), "S"(mode)
                      : "memory");
    return ret;
}

static inline i64 sys_fork(void)
{
    i64 ret;

    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(9) : "memory");
    return ret;
}

static inline i64 sys_execve(const char *path, char *const argv[],
                             char *const envp[])
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(10), "D"(path), "S"(argv), "d"(envp)
                      : "memory");
    return ret;
}

static inline i64 sys_pipe(int fds[2])
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(12), "D"(fds)
                      : "memory");
    return ret;
}

static inline i64 sys_dup2(int oldfd, int newfd)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(13), "D"(oldfd), "S"(newfd)
                      : "memory");
    return ret;
}

static inline i64 sys_setpgid(int pid, int pgid)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(14), "D"(pid), "S"(pgid)
                      : "memory");
    return ret;
}

static inline i64 sys_set_fg(int pgid)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(15), "D"(pgid)
                      : "memory");
    return ret;
}

static inline i64 sys_detach(int pid)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(16), "D"(pid)
                      : "memory");
    return ret;
}

static inline i64 sys_kill_pgrp(int pgid, int sig)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(17), "D"(pgid), "S"(sig)
                      : "memory");
    return ret;
}

static inline i64 sys_waitpid(int pid, i64 *status)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(18), "D"(pid), "S"(status)
                      : "memory");
    return ret;
}

static inline i64 sys_mmap(u64 size)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(19), "D"(size)
                      : "memory");
    return ret;
}

static inline i64 sys_mmap_file(int fd, u64 offset, u64 length)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(20), "D"(fd), "S"(offset), "d"(length)
                      : "memory");
    return ret;
}

static inline i64 sys_munmap(u64 addr, u64 len)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(21), "D"(addr), "S"(len)
                      : "memory");
    return ret;
}

static inline i64 sys_mprotect(u64 addr, u64 len, u64 prot)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(22), "D"(addr), "S"(len), "d"(prot)
                      : "memory");
    return ret;
}

static inline i64 sys_mmap_shared(int fd, u64 offset, u64 length)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(23), "D"(fd), "S"(offset), "d"(length)
                      : "memory");
    return ret;
}

static inline i64 sys_msync(u64 addr, u64 len)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(24), "D"(addr), "S"(len)
                      : "memory");
    return ret;
}

static inline i64 sys_lseek(int fd, i64 offset, int whence)
{
    i64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(25), "D"(fd), "S"(offset), "d"(whence)
                      : "memory");
    return ret;
}

static inline u64 sys_time_ms(void)
{
    u64 ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(26)
                      : "memory");
    return ret;
}

static inline i64 sys_getpid(void)
{
    i64 ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(3) : "memory");
    return ret;
}

static inline i64 sys_unlink(const char *path)
{
    i64 ret;

    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(11), "D"(path)
                      : "memory");
    return ret;
}

static inline void sys_write_str(const char *s)
{
    u64 len = 0;

    while (s[len] != '\0')
        ++len;
    sys_write(STDOUT_FD, s, len);
}
