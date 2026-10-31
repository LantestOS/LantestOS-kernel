/*
 * Lantest OS - Ring 3 child process (C, loaded from ELF)
 */
#include "userlib.h"

/* .data and .bss sections, both placed in the same 4 KiB page as .text by
 * user/user.ld to exercise the loader's page-sharing logic. */
volatile unsigned long g_child_data = 0x0BADF00DUL;
volatile unsigned long g_child_bss;

void _start(void)
{
    sys_write_str("child: C ELF from Ring 3, exiting 42\n");
    if (g_child_data != 0x0BADF00DUL || g_child_bss != 0)
        sys_write_str("child: data/bss check FAILED\n");

    /* Stay in Ring 3 across several PIT ticks (also exercises user-mode IRQs). */
    for (g_child_bss = 0; g_child_bss < 3000000UL; ++g_child_bss) {
    }

    sys_exit(42);
}
