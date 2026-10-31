/*
 * Lantest OS - 16550 UART driver (COM1, polling)
 *
 * A UART is not guaranteed to exist (e.g. VirtualBox without a serial port).
 * uart_init() probes the 16550 scratch register and disables output cleanly
 * when no device answers, so the kernel never hangs on a missing serial port.
 *
 * Input is polled (UART interrupts stay disabled); console_read() sleeps in
 * hlt between polls, so the PIT wakes it every 10 ms without busy-spinning.
 */
#include <types.h>
#include <io.h>
#include <spinlock.h>
#include <uart.h>

#define COM1          0x3F8
#define UART_LSR      5
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20
#define UART_SCR      7

static int uart_present;
static spinlock_t uart_lock = SPINLOCK_INIT;

int uart_init(void)
{
    /* Probe: the scratch register must read back what was written. */
    outb(COM1 + UART_SCR, 0xAE);
    if (inb(COM1 + UART_SCR) != 0xAE) {
        uart_present = 0;
        return 0;
    }

    outb(COM1 + 1, 0x00);   /* disable UART interrupts */
    outb(COM1 + 3, 0x80);   /* enable DLAB */
    outb(COM1 + 0, 0x01);   /* divisor low byte: 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);   /* divisor high byte */
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* enable + clear FIFOs, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* IRQs off, RTS/DSR asserted */

    uart_present = 1;
    return 1;
}

void uart_putc(char c)
{
    u64 flags;

    if (!uart_present)
        return;

    spin_lock_irqsave(&uart_lock, &flags);
    /* Wait unboundedly for THR empty, cross-CPU serialized. The old
     * bounded "best effort" wait silently DROPPED characters while the
     * UART was busy and raced other CPUs into the THR; both corrupted
     * serial-log lines during stress markers (lost "stress: worker PASS"
     * made passing runs look failed). QEMU clears THRE within
     * microseconds; a dead UART now hangs loudly instead of eating
     * output. */
    while ((inb(COM1 + UART_LSR) & UART_LSR_THRE) == 0)
        cpu_relax();
    outb(COM1, (u8)c);
    spin_unlock_irqrestore(&uart_lock, flags);
}

void uart_puts(const char *s, u64 len)
{
    u64 flags;
    u64 i;

    if (!uart_present)
        return;

    /* One lock hold per buffer: a console write (usually one printf
     * line) is atomic on the wire, so two CPUs can never interleave a
     * line mid-way. The spinlock is not recursive: emit inline instead
     * of calling uart_putc() here. */
    spin_lock_irqsave(&uart_lock, &flags);
    for (i = 0; i < len; ++i) {
        while ((inb(COM1 + UART_LSR) & UART_LSR_THRE) == 0)
            cpu_relax();
        outb(COM1, (u8)s[i]);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
}

int uart_poll_getc(char *out)
{
    if (!uart_present || out == NULL)
        return 0;

    if ((inb(COM1 + UART_LSR) & UART_LSR_DR) == 0)
        return 0;

    *out = (char)inb(COM1);
    return 1;
}

int uart_available(void)
{
    return uart_present;
}
