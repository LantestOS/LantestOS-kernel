/*
 * Lantest OS - independent error codes for the syscall/file ABI
 */
#pragma once

#define EINTR    4   /* interrupted by signal */
#define ENOENT   2   /* no such file or directory */
#define E2BIG    7   /* argument list too long */
#define EIO      5   /* I/O error */
#define EBADF    9   /* bad file descriptor */
#define ENOMEM  12   /* out of memory */
#define EFAULT  14   /* bad user pointer */
#define EEXIST  17   /* already exists */
#define ENOTDIR 20   /* not a directory */
#define EISDIR  21   /* is a directory */
#define EINVAL  22   /* invalid argument */
#define EMFILE  24   /* fd table full */
#define ENOSPC  28   /* no space left */
#define EPIPE   32   /* broken pipe */
#define ENOSYS  38   /* not implemented */
