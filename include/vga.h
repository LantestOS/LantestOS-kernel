/*
 * Lantest OS - VGA text console (80x25 at 0xB8000)
 */
#pragma once

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
