/*
 * Lantest OS - 16550 UART driver (COM1, polling)
 */
#pragma once

/*
 * Probe and initialize COM1 at 115200 8N1.
 * Returns 1 if a UART was detected, 0 otherwise (e.g. VirtualBox without a
 * serial port configured). In both cases the kernel keeps running; the VGA
 * console remains available.
 */
int uart_init(void);

void uart_putc(char c);

/* Polling input: returns 1 and stores a byte if one is available. */
int uart_poll_getc(char *out);

int uart_available(void);
