/*
 * Lantest OS - 8259A PIC and 8254 PIT
 */
#pragma once

#include <types.h>
#include <x86_64/isr.h>

#define IRQ_TIMER 0u

/* Remap IRQs to 0x20-0x2F and mask everything. */
void pic_remap(void);

void pic_mask_all(void);
void pic_unmask(u8 irq);

/* Program PIT channel 0 to fire IRQ0 at frequency_hz and unmask IRQ0. */
void pit_init(u32 frequency_hz);

/* Called from isr_handler() for vectors >= 32. Sends EOI. */
void irq_handler(registers_t *regs);

/* Number of timer ticks since boot. */
u64 timer_ticks(void);
