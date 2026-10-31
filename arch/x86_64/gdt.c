/*
 * Lantest OS - GDT and per-CPU TSS (M4-37)
 *
 * Layout:
 *   0x00 null
 *   0x08 kernel code
 *   0x10 kernel data
 *   0x18 user code (DPL 3)
 *   0x20 user data (DPL 3)
 *   0x28 + cpu*0x10: 64-bit TSS for CPU cpu (two slots)
 */
#include <types.h>
#include <x86_64/gdt.h>

#define GDT_MAX_CPUS 8u
#define GDT_ENTRIES (5u + 2u * GDT_MAX_CPUS)

struct tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static u64 gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static struct tss64 tss[GDT_MAX_CPUS] __attribute__((aligned(16)));
static u8 tss_stacks[GDT_MAX_CPUS][8192] __attribute__((aligned(16)));
static struct gdt_ptr gdtr;

static void gdt_zero(u8 *p, usize n)
{
    for (usize i = 0; i < n; ++i)
        p[i] = 0;
}

/* 64-bit TSS descriptor occupies 16 bytes (two GDT slots). */
static void gdt_set_tss(u32 idx, u64 base, u32 limit)
{
    gdt[idx] = (u64)(limit & 0xFFFF)
             | ((u64)(base & 0xFFFFFF) << 16)
             | ((u64)0x89 << 40)
             | ((u64)((limit >> 16) & 0x0F) << 48)
             | ((u64)((base >> 32) & 0xFF) << 56);
    gdt[idx + 1] = (u64)((base >> 32) & 0xFFFFFFFFu);
}

static u16 tss_selector(u32 cpu)
{
    return (u16)(GDT_TSS + cpu * 0x10u);
}

static void gdt_load_cpu(u32 cpu)
{
    u16 sel = tss_selector(cpu);

    __asm__ volatile ("lgdt (%0)" : : "r"(&gdtr));
    __asm__ volatile (
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        :
        :
        : "ax");
    __asm__ volatile (
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : : "rax", "memory");
    __asm__ volatile ("ltr %0" : : "rm"(sel));
}

void gdt_init(void)
{
    u32 i;

    gdt_zero((u8 *)gdt, sizeof(gdt));
    gdt_zero((u8 *)tss, sizeof(tss));

    gdt[0] = 0x0000000000000000ULL;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00AF92000000FFFFULL;
    gdt[3] = 0x00AFFA000000FFFFULL;
    gdt[4] = 0x00CFF2000000FFFFULL;

    for (i = 0; i < GDT_MAX_CPUS; ++i) {
        tss[i].rsp0 = (u64)(tss_stacks[i] + sizeof(tss_stacks[i]));
        tss[i].iomap_base = (u16)sizeof(struct tss64);
        gdt_set_tss(5u + i * 2u, (u64)&tss[i], sizeof(struct tss64) - 1);
    }

    gdtr.limit = (u16)(sizeof(gdt) - 1);
    gdtr.base = (u64)gdt;

    gdt_load_cpu(0);
}

void gdt_init_cpu(u32 cpu)
{
    if (cpu >= GDT_MAX_CPUS)
        return;
    gdt_load_cpu(cpu);
}

u64 gdt_tss_rsp0_cpu(u32 cpu)
{
    if (cpu >= GDT_MAX_CPUS)
        return 0;
    return tss[cpu].rsp0;
}

void gdt_set_tss_rsp0_cpu(u32 cpu, u64 rsp0)
{
    if (cpu < GDT_MAX_CPUS)
        tss[cpu].rsp0 = rsp0;
}

u64 gdt_tss_rsp0(void)
{
    return gdt_tss_rsp0_cpu(0);
}

void gdt_set_tss_rsp0(u64 rsp0)
{
    gdt_set_tss_rsp0_cpu(0, rsp0);
}
