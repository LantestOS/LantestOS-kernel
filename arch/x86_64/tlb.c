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
    u32 i;

    if (smp_ap_count() == 0)
        return;

    __atomic_store_n(&tlb_target_cr3, cr3, __ATOMIC_SEQ_CST);
    __sync_synchronize();

    for (i = 1; i < PERCPU_MAX; ++i) {
        u32 apicid = percpu_apic_id_of(i);

        if (percpu_cpu_online(i) && apicid != 0)
            lapic_send_ipi(apicid, 0x30u | 0x4000u);
    }

    /* Fire-and-forget. We deliberately do NOT spin for acknowledgements:
     * task_finish_reap only runs when active_cpus == 0, so no CPU is
     * executing this address space any more; a later CR3 write on any
     * core (task switch into a space reusing this value) flushes stale
     * entries in hardware. The old ACK wait livelocked every CPU that
     * entered it concurrently: syscalls run with IF=0 behind the
     * interrupt gate, so a spinning sender can never service the very
     * shootdown IPI its peers are waiting for, and the single global
     * tlb_ack_count got zeroed by every re-entering reaper (GDB: 4/4
     * CPUs parked in the ack loop, ack=0, all locks free). The IPI is
     * still posted so a peer that has interrupts enabled reloads early;
     * idle APs consume it when they next enable interrupts. */
}
