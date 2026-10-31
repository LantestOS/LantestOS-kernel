/*
 * Lantest OS - SMP discovery and AP startup (M4-33)
 */
#include <types.h>
#include <acpi.h>
#include <lapic.h>
#include <smp.h>
#include <kprintf.h>
#include <x86_64/paging.h>
#include <x86_64/gdt.h>
#include <x86_64/idt.h>
#include <spinlock.h>
extern volatile u32 smp_test_irq_count;
extern volatile u32 smp_timer_ticks;
#include <percpu.h>
#include <task.h>

struct ap_boot_params {
    u64 cr3;
    u64 stack_top;
    u64 entry;
};

#define AP_PARAMS ((volatile struct ap_boot_params *)0x7000)
#define AP_MAX 8

static u8 ap_stacks[AP_MAX][8192] __attribute__((aligned(16)));
static volatile u32 ap_online;
static u32 discovered_cpus = 1;
static spinlock_t ap_test_lock = SPINLOCK_INIT;
static struct task ap_idle_tasks[AP_MAX];
static volatile u32 ap_test_counter;
static volatile u32 ap_percpu_ok;

static u32 cpuid_apic_id(void)
{
    u32 a, b, c, d;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1));
    (void)a; (void)c; (void)d;
    return (b >> 24) & 0xffu;
}

static void ap_delay(u32 loops)
{
    while (loops-- > 0)
        __asm__ volatile ("pause");
}

void ap_main(void)
{
    for (u32 i = 0; i < 100000u; ++i) {
        spin_lock(&ap_test_lock);
        ap_test_counter++;
        spin_unlock(&ap_test_lock);
    }
    u32 cpu = cpuid_apic_id();
    struct task *idle = &ap_idle_tasks[cpu % AP_MAX];
    idle->pid = 0;
    idle->state = TASK_RUNNING;
    idle->cr3 = paging_read_cr3();
    idle->user = 0;
    percpu_init_cpu(cpu, idle);
    percpu_set_idle(cpu, idle);
    if (task_current() == idle)
        __atomic_add_fetch(&ap_percpu_ok, 1, __ATOMIC_SEQ_CST);

    gdt_init_cpu(cpu);
    idt_reload();
    __asm__ volatile ("sti");
    __atomic_add_fetch(&ap_online, 1, __ATOMIC_SEQ_CST);
    lapic_init();
    lapic_timer_start(0x31, 1000000u);
    __asm__ volatile ("int $0x30");

    /* AP idle: poll the local run queue. This avoids relying on IPI/timer
     * delivery while the SMP scheduler is still maturing. */
    for (;;) {
        schedule();
        __asm__ volatile ("pause");
    }
}

static int start_one_ap(u32 apic_id, u32 slot)
{
    u64 cr3 = paging_read_cr3();
    u32 target = ap_online + 1;
    u32 spins;

    AP_PARAMS->cr3 = cr3;
    AP_PARAMS->stack_top = (u64)(ap_stacks[slot] + sizeof(ap_stacks[slot]));
    AP_PARAMS->entry = (u64)(usize)ap_main;
    __sync_synchronize();

    /* INIT assert, 10 ms-ish; INIT de-assert; two SIPIs. */
    lapic_send_ipi(apic_id, 0x00004500u);
    ap_delay(8000000u);
    lapic_send_ipi(apic_id, 0x00008500u);
    ap_delay(200000u);

    lapic_send_ipi(apic_id, 0x00000606u);
    ap_delay(200000u);
    lapic_send_ipi(apic_id, 0x00000606u);

    for (spins = 0; spins < 20000000u; ++spins) {
        if (__atomic_load_n(&ap_online, __ATOMIC_SEQ_CST) >= target)
            return 1;
        ap_delay(4);
    }
    return 0;
}

static void start_aps(void)
{
    const struct acpi_info *info = acpi_get();
    u32 bsp = lapic_id();
    u32 slot = 0;

    ap_online = 0;
    ap_test_counter = 0;
    ap_percpu_ok = 0;
    if (!lapic_x2apic())
        return;

    for (u32 i = 0; i < info->cpu_count && slot < AP_MAX; ++i) {
        u32 id = info->apic_ids[i];

        if (id == bsp)
            continue;
        if (start_one_ap(id, slot))
            kprintf("[smp] AP APIC ID %u online\n", id);
        else
            kprintf("[smp] AP APIC ID %u did not start\n", id);
        ++slot;
    }
}

void smp_init(u64 mb2_info)
{
    const struct acpi_info *info;
    int x2;

    acpi_init(mb2_info);
    info = acpi_get();

    x2 = lapic_init();
    discovered_cpus = info->cpu_count ? info->cpu_count : 1;

    kprintf("[smp] MADT: %u CPU(s), BSP APIC ID %u, x2APIC=%s\n",
            info->cpu_count, lapic_id(), x2 ? "yes" : "no");
    for (u32 i = 0; i < info->cpu_count; ++i) {
        kprintf("[smp]   CPU%u APIC ID %u%s\n",
                i, info->apic_ids[i],
                info->apic_ids[i] == lapic_id() ? " (BSP)" : "");
    }

    start_aps();
    ap_delay(100000u);

    /* IPI probe: each AP already did one software `int $0x30` during its
     * own bring-up (counter = 1 each). Any extra count after this hardware
     * IPI proves the x2APIC ICR path really delivers. */
    if (ap_online > 0) {
        u32 before = smp_test_irq_count;
        u32 self = lapic_id();

        for (u32 i = 0; i < info->cpu_count; ++i) {
            if (info->apic_ids[i] != self) {
                lapic_send_ipi(info->apic_ids[i], 0x30u);
                break;
            }
        }
        ap_delay(2000000u);
        kprintf("[smp] IPI probe: irq %u -> %u (expect +1)\n",
                (u32)before, (u32)smp_test_irq_count);
    }

    kprintf("[smp] APs online=%u, total CPUs=%u, spin counter=%u, percpu ok=%u, irq=%u, timer ticks=%u\n",
            (u32)ap_online, (u32)ap_online + 1u,
            (u32)ap_test_counter, (u32)ap_percpu_ok,
            (u32)smp_test_irq_count, (u32)smp_timer_ticks);
}

u32 smp_ap_count(void)
{
    return (u32)ap_online;
}

u32 smp_cpu_count(void)
{
    return (u32)ap_online + 1u;
}
