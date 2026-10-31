/*
 * Lantest OS - kernel panic
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>

void panic(const char *msg)
{
    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  KERNEL PANIC: %s\n", msg);
    kprintf("==================================================\n");
    kprintf("[panic] system halted (cli; hlt)\n");

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}
