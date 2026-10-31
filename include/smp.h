#pragma once

#include <types.h>

void smp_init(u64 mb2_info);
u32 smp_cpu_count(void);
u32 smp_ap_count(void);
