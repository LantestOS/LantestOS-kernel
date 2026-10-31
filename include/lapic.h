#pragma once

#include <types.h>

int lapic_init(void);
u32 lapic_id(void);
void lapic_send_ipi(u32 dest_apic_id, u32 icr_low);
void lapic_eoi(void);
void lapic_timer_start(u32 vector, u32 count);
void lapic_timer_stop(void);
int lapic_x2apic(void);
