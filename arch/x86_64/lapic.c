/*
 * Lantest OS - x2APIC support (M4-32 SMP foundation)
 */
#include <types.h>
#include <kprintf.h>
#include <lapic.h>

#define MSR_APIC_BASE 0x1Bu
#define MSR_X2APIC_ID 0x802u
#define MSR_X2APIC_TPR 0x808u
#define MSR_X2APIC_EOI 0x80Bu
#define MSR_X2APIC_SVR 0x80Fu
#define MSR_X2APIC_ICR 0x830u
#define MSR_X2APIC_ESR 0x828u

static int x2apic_on;

static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 value)
{
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)));
}

static int cpuid_x2apic(void)
{
    u32 a, b, c, d;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1));
    (void)a; (void)b; (void)d;
    return (c & (1u << 21)) != 0;
}

int lapic_init(void)
{
    u64 base;

    if (!cpuid_x2apic())
        return 0;

    base = rdmsr(MSR_APIC_BASE);
    base |= (1u << 10);          /* x2APIC enable */
    wrmsr(MSR_APIC_BASE, base);

    wrmsr(MSR_X2APIC_TPR, 0);
    wrmsr(MSR_X2APIC_SVR, 0x1FF); /* APIC enabled, spurious vector 0xFF */
    wrmsr(MSR_X2APIC_EOI, 0);
    x2apic_on = 1;
    return 1;
}

int lapic_x2apic(void)
{
    return x2apic_on;
}

u32 lapic_id(void)
{
    if (x2apic_on)
        return (u32)(rdmsr(MSR_X2APIC_ID) & 0xFFFFFFFFu);
    {
        u32 a, b, c, d;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                          : "a"(1));
        (void)a; (void)c; (void)d;
        return (b >> 24) & 0xffu;
    }
}

void lapic_eoi(void)
{
    if (x2apic_on)
        wrmsr(MSR_X2APIC_EOI, 0);
}

void lapic_timer_start(u32 vector, u32 count)
{
    if (!x2apic_on)
        return;
    /* periodic mode: LVT bit 17 = 1; vector in low byte */
    wrmsr(0x832u, 0x00020000u | (vector & 0xFFu));
    wrmsr(0x83Eu, 0x0Bu);        /* divide by 1 */
    wrmsr(0x838u, count);        /* initial count */
}

void lapic_timer_stop(void)
{
    if (x2apic_on)
        wrmsr(0x832u, 0x00010000u);  /* masked */
}

static int icr_delivery_idle(void)
{
    return (rdmsr(MSR_X2APIC_ICR) & (1ULL << 12)) == 0;
}

void lapic_send_ipi(u32 dest_apic_id, u32 icr_low)
{
    u64 icr;
    u64 esr;

    if (!x2apic_on)
        return;

    /* A still-pending previous send (status bit 12 set) makes the
     * hardware silently DROP this write; drain it first. */
    for (u32 i = 0; i < 100000u; ++i) {
        if (icr_delivery_idle())
            break;
        __asm__ volatile ("pause");
    }

    icr = ((u64)dest_apic_id << 32) | (u64)icr_low;
    wrmsr(MSR_X2APIC_ICR, icr);

    /* Wait for delivery status (bit 12) to clear. */
    for (u32 i = 0; i < 100000u; ++i) {
        if (icr_delivery_idle())
            break;
        __asm__ volatile ("pause");
    }

    /* Report (and clear) any send error: illegal vector, accept error. */
    esr = rdmsr(MSR_X2APIC_ESR);
    if (esr != 0) {
        wrmsr(MSR_X2APIC_ESR, esr);     /* write-1-to-clear */
        kprintf("[lapic] IPI error ESR=%p dest=%u low=%p\n",
                (void *)esr, dest_apic_id, (void *)(u64)icr_low);
    }
}
