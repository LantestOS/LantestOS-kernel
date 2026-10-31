/*
 * Lantest OS - console character device (stdin/stdout/stderr backend)
 */
#pragma once

#include <fs.h>

extern const struct file_operations console_fops;

void tty_set_foreground_pgid(u64 pgid);
u64 tty_foreground_pgid(void);
int tty_send_sigint(void);
int tty_send_sigtstp(void);
