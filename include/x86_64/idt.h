/*
 * Lantest OS - interrupt descriptor table
 */
#pragma once

/* Fill all 256 gates (0-31 exceptions, 32-47 IRQs, rest default) and lidt. */
void idt_init(void);
void idt_reload(void);
