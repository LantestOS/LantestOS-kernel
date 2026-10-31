/*
 * Lantest OS - TLB shootdown via x2APIC IPI 0xFD (M4-45)
 */
#include <types.h>
#include <tlb.h>
#include <spinlock.h>
#include <lapic.h>
#include <smp.h>
#include <percpu.h>
#include <x86_64/paging.h>

static volatile u64 tlb_target_cr3;
volatile u32 tlb_ack_count;

u64 tlb_shootdown_target(void)
{
    return tlb_target_cr3;
}

void tlb_shootdown_all(u64 cr3)
{
    u32 ap_count = smp_ap_count();
    u32 i;

    if (ap_count == 0)
        return;

    __atomic_store_n(&tlb_target_cr3, cr3, __ATOMIC_SEQ_CST);
    __atomic_store_n(&tlb_ack_count, 0, __ATOMIC_SEQ_CST);
    __sync_synchronize();

    for (i = 1; i < PERCPU_MAX; ++i) {
        u32 apicid = percpu_apic_id_of(i);

        if (percpu_cpu_online(i) && apicid != 0) {
            lapic_send_ipi(apicid, 0x30u);
        }
    }

    for (u32 spins = 0; spins < 1000000u; ++spins) {
        if (__atomic_load_n(&tlb_ack_count, __ATOMIC_SEQ_CST) >= ap_count)
            break;
        __asm__ volatile ("pause");
    }
}
