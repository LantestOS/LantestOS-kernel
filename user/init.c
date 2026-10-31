/*
 * Lantest OS - init process
 *
 * The kernel only loads this tiny ELF. init forks and execve()s /bin/sh from
 * LantestFS; if the shell exits, init waits and restarts it.
 */
#include "userlib.h"

void _start(void)
{
    char *argv[] = { "/bin/sh", 0 };

    for (;;) {
        i64 pid = sys_fork();

        if (pid == 0) {
            sys_execve(argv[0], argv, 0);
            sys_write_str("init: exec /bin/sh failed\n");
            sys_exit(127);
        } else if (pid > 0) {
            i64 status = 0;
            sys_wait(&status);
            sys_write_str("init: shell exited, restarting\n");
        } else {
            sys_write_str("init: fork failed\n");
            sys_exit(1);
        }
    }
}
