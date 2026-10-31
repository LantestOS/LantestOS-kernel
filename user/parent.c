/*
 * Lantest OS - Ring 3 parent process (C, loaded from ELF)
 */
#include "userlib.h"

/* Force a .data section so the ELF has an RW segment in the text page. */
volatile unsigned long g_parent_data = 0x12345678UL;

void _start(void)
{
    i64 status = 0;
    i64 pid;

    sys_write_str("parent: C ELF from Ring 3, waiting for child\n");
    if (g_parent_data != 0x12345678UL)
        sys_write_str("parent: .data check FAILED\n");

    pid = sys_wait(&status);
    if (pid > 0 && status == 42)
        sys_write_str("parent: child reaped, status=42\n");
    else
        sys_write_str("parent: wait failed\n");

    /* M3c-3: malicious pointer tests through fd 1. */
    if (sys_write(STDOUT_FD, (const void *)0x100000UL, 4) < 0)
        sys_write_str("parent: kernel pointer rejected\n");
    else
        sys_write_str("parent: kernel pointer NOT rejected\n");

    if (sys_write(STDOUT_FD, (const void *)(USER_BASE + 0x2000UL), 4) < 0)
        sys_write_str("parent: unmapped user pointer rejected\n");
    else
        sys_write_str("parent: unmapped user pointer NOT rejected\n");

    /* M4: fd table rejects an unopened descriptor. */
    if (sys_write(99, "x", 1) < 0)
        sys_write_str("parent: bad fd rejected\n");
    else
        sys_write_str("parent: bad fd NOT rejected\n");

    sys_exit((pid > 0 && status == 42) ? 0 : 1);
}
