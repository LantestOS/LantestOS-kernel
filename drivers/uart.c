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
#include <uart.h>

#define COM1          0x3F8
#define UART_LSR      5
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20
#define UART_SCR      7

static int uart_present;

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
    if (!uart_present)
        return;

    /* Poll THR-empty, but never spin forever on broken hardware. */
    for (u32 spin = 0; spin < 1000000u; ++spin) {
        if (inb(COM1 + UART_LSR) & UART_LSR_THRE) {
            outb(COM1, (u8)c);
            return;
        }
    }
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
