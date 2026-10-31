#pragma once

#include <types.h>

#define GDT_KERNEL_CS 0x08u
#define GDT_KERNEL_DS 0x10u
#define GDT_USER_CS   0x18u
#define GDT_USER_DS   0x20u
#define GDT_TSS       0x28u
#define GDT_MAX_CPUS  8u

void gdt_init(void);
void gdt_init_cpu(u32 cpu);

u64 gdt_tss_rsp0(void);
void gdt_set_tss_rsp0(u64 rsp0);
u64 gdt_tss_rsp0_cpu(u32 cpu);
void gdt_set_tss_rsp0_cpu(u32 cpu, u64 rsp0);
