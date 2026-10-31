/*
 * Lantest OS - PS/2 keyboard IRQ1 + ring buffer
 */
#pragma once

#include <types.h>

void keyboard_init(void);
void keyboard_irq(void);
int keyboard_getc(char *out);
