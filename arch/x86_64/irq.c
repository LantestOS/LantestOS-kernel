/*
 * Lantest OS - 8259A PIC remapping and 8254 PIT timer
 *
 * On each timer tick the preemption counter advances; every SCHED_SLICE
 * ticks the scheduler is invoked (after EOI) to switch tasks.
 */
#include <types.h>
#include <io.h>
#include <kprintf.h>
#include <task.h>
#include <x86_64/isr.h>
#include <x86_64/irq.h>
#include <keyboard.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_FREQ   1193182u

#define SCHED_SLICE 10u   /* 100 Hz / 10 -> 100 ms time slice */

static volatile u64 ticks;
static int user_irq_reported;

void pic_remap(void)
{
    /* ICW1: begin initialization, expect ICW4 */
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20);   /* master IRQ0-7  -> 0x20-0x27 */
    io_wait();
    outb(PIC2_DATA, 0x28);   /* slave  IRQ8-15 -> 0x28-0x2F */
    io_wait();

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);   /* slave is attached to IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);   /* slave identity */
    io_wait();

    /* ICW4: 8086/88 mode */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    pic_mask_all();
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(u8 irq)
{
    u16 port;
    u8 bit;

    if (irq < 8) {
        port = PIC1_DATA;
        bit = irq;
    } else {
        port = PIC2_DATA;
        bit = (u8)(irq - 8);
        /* IRQ2 on the master cascades to the slave PIC. */
        u8 master = inb(PIC1_DATA);
        outb(PIC1_DATA, (u8)(master & ~(1u << 2)));
    }

    u8 mask = inb(port);
    outb(port, (u8)(mask & ~(1u << bit)));
}

void pit_init(u32 frequency_hz)
{
    u32 divisor = PIT_FREQ / frequency_hz;

    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    /* Channel 0, access lobyte/hibyte, mode 3 (square wave), binary. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));

    pic_unmask(IRQ_TIMER);
}

void irq_handler(registers_t *regs)
{
    u32 irq = (u32)(regs->int_no - 32);

    if (irq == IRQ_TIMER) {
        ticks++;

        if (!user_irq_reported && ((regs->cs & 3u) == 3u)) {
            user_irq_reported = 1;
            kprintf("[M3b] timer IRQ from Ring 3 (CS=%lx): TSS.RSP0 stack switch OK\n", regs->cs);
        }

        if ((ticks % 100) == 0)
            kprintf("[tick] uptime=%lu s ticks=%lu\n", ticks / 100, ticks);
    } else if (irq == 1) {
        keyboard_irq();
    } else {
        kprintf("[irq ] unexpected IRQ %u (vector %lu)\n", irq, regs->int_no);
    }

    /* Acknowledge before a possible context switch, so the next tick is not
     * lost while another task's suspended handler is waiting to resume. */
    if (regs->int_no >= 40)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    if (irq == IRQ_TIMER && scheduler_ready() && (ticks % SCHED_SLICE) == 0)
        schedule();
}

u64 timer_ticks(void)
{
    return ticks;
}
