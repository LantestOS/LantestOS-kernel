#pragma once

#include <types.h>

#define ACPI_MAX_CPUS 32

struct acpi_info {
    u32 cpu_count;
    u32 bsp_apic_id;
    u32 apic_ids[ACPI_MAX_CPUS];
    u64 lapic_base;
};

int acpi_init(u64 mb2_info);
const struct acpi_info *acpi_get(void);
