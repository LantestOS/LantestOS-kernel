#pragma once

#include <types.h>

extern volatile u32 tlb_ack_count;
void tlb_shootdown_all(u64 cr3);
u64 tlb_shootdown_target(void);
