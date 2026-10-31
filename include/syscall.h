/*
 * Lantest OS - custom syscall ABI (independent from Linux)
 */
#pragma once

#include <x86_64/isr.h>

#define SYS_WRITE   1u   /* write(fd=rdi, buf=rsi, len=rdx) -> bytes or -errno */
#define SYS_EXIT    2u   /* exit(rdi = status code)            -> zombie + schedule */
#define SYS_GETPID  3u   /* getpid()                           -> pid */
#define SYS_WAIT    4u   /* wait(rdi = &status)                -> child pid (reaps it) */
#define SYS_OPEN    5u   /* open(path=rdi, flags=rsi)          -> fd or -errno */
#define SYS_READ    6u   /* read(fd=rdi, buf=rsi, len=rdx)     -> bytes or -errno */
#define SYS_CLOSE   7u   /* close(fd=rdi)                      -> 0 or -errno */
#define SYS_MKDIR   8u   /* mkdir(path=rdi, mode=rsi)          -> 0 or -errno */
#define SYS_FORK    9u   /* fork()                             -> child pid / 0 */
#define SYS_EXECVE 10u   /* execve(path=rdi, argv=rsi, envp=rdx) -> only returns on error */
#define SYS_UNLINK 11u
#define SYS_PIPE   12u   /* pipe(fds=rdi)                      -> 0 */
#define SYS_DUP2   13u
#define SYS_SETPGID 14u  /* setpgid(pid=rdi, pgid=rsi)           -> 0 */
#define SYS_SET_FG  15u  /* set foreground pgrp (pgid=rdi)       -> 0 */
#define SYS_DETACH  16u
#define SYS_KILL    17u  /* signal pgrp (pgid=rdi, sig=rsi)      -> 0 */
#define SYS_WAITPID 18u
#define SYS_MMAP    19u  /* mmap(size=rdi) -> user address */
#define SYS_MMAP_FILE 20u
#define SYS_MMAP_SHARED 23u /* mmap_shared(fd, off, len) */
#define SYS_MSYNC   24u
#define SYS_LSEEK   25u
#define SYS_TIME    26u  /* milliseconds since boot */  /* lseek(fd=rdi, off=rsi, whence=rdx) */  /* msync(addr=rdi, len=rsi) */
#define SYS_MUNMAP  21u  /* munmap(addr=rdi, len=rsi)             -> 0 */
#define SYS_MPROTECT 22u /* mprotect(addr=rdi, len=rsi, prot=rdx) -> 0 */ /* mmap_file(fd=rdi, off=rsi, len=rdx) */  /* waitpid(pid=rdi, status=rsi)          -> pid/0 */  /* detach child (pid=rdi)                -> 0 */   /* dup2(oldfd=rdi, newfd=rsi)          -> newfd */   /* unlink(path=rdi)                   -> 0 or -errno */

void syscall_handler(registers_t *regs);
