/*
 * Lantest OS - kernel formatted output (serial + VGA)
 */
#pragma once

#include <types.h>

/* Write one character to every console that is currently available. */
void kputc(char c);

/* Write a NUL-terminated string. */
void kputs(const char *s);

/*
 * Minimal printf: %c %s %d %i %u %x %X %p %%
 * Optional '0' flag, decimal field width, and 'l' length modifier.
 */
void kprintf(const char *fmt, ...);
