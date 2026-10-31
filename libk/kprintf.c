/*
 * Lantest OS - kprintf implementation
 *
 * Writes to the UART (when present) and the VGA text console. Supports
 * %c %s %d %i %u %x %X %p %%, the '0' flag, decimal field width and the 'l'
 * length modifier.
 *
 * No libc, no libgcc: 64-bit decimal formatting is done with a software
 * divide-by-10, so the kernel image has no runtime-library dependency.
 *
 * Each kprintf() runs with interrupts disabled, which makes console output
 * atomic with respect to the timer IRQ and to preemptive task switches.
 */
#include <types.h>
#include <kprintf.h>
#include <uart.h>
#include <vga.h>
#include <spinlock.h>

typedef __builtin_va_list va_list;
static spinlock_t kprintf_lock = SPINLOCK_INIT;

void kputc(char c)
{
    if (c == '\n')
        uart_putc('\r');    /* serial terminals expect CRLF */
    uart_putc(c);
    vga_putc(c);
}

void kputs(const char *s)
{
    while (*s != '\0')
        kputc(*s++);
}

static void pad_field(int width, int len, int zero)
{
    while (width > len) {
        kputc(zero ? '0' : ' ');
        --width;
    }
}

/* Decimal digits of value, least significant first. Returns digit count. */
static int dec_digits(u64 value, char *buf)
{
    int n = 0;

    if (value == 0) {
        buf[n++] = '0';
        return n;
    }

    while (value != 0) {
        u64 q = 0;
        u32 r = 0;

        for (int i = 63; i >= 0; --i) {
            r = (r << 1) | (u32)((value >> (u32)i) & 1u);
            q <<= 1;
            if (r >= 10u) {
                r -= 10u;
                q |= 1u;
            }
        }

        buf[n++] = (char)('0' + r);
        value = q;
    }

    return n;
}

static void put_udec(u64 value, int zero, int width)
{
    char buf[20];
    int n = dec_digits(value, buf);

    pad_field(width, n, zero);
    while (n > 0)
        kputc(buf[--n]);
}

static void put_idec(i64 value, int zero, int width)
{
    u64 magnitude;

    if (value < 0) {
        kputc('-');
        if (width > 0)
            --width;
        magnitude = 0 - (u64)value;
    } else {
        magnitude = (u64)value;
    }

    put_udec(magnitude, zero, width);
}

static void put_hex(u64 value, int zero, int width, int upper)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char buf[16];
    int n = 0;

    do {
        buf[n++] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0);

    pad_field(width, n, zero);
    while (n > 0)
        kputc(buf[--n]);
}

static void vkprintf(const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p != '\0') {
        if (*p != '%') {
            kputc(*p++);
            continue;
        }

        ++p;

        int zero = 0;
        int width = 0;
        int is_long = 0;

        if (*p == '0') {
            zero = 1;
            ++p;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            ++p;
        }
        if (*p == 'l') {
            is_long = 1;
            ++p;
        }

        char conv = *p;
        if (conv == '\0')
            break;
        ++p;

        switch (conv) {
        case '%':
            kputc('%');
            break;

        case 'c':
            kputc((char)__builtin_va_arg(ap, int));
            break;

        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            int len = 0;

            if (s == NULL)
                s = "(null)";
            while (s[len] != '\0')
                ++len;
            pad_field(width, len, 0);
            kputs(s);
            break;
        }

        case 'd':
        case 'i': {
            i64 value = is_long
                ? __builtin_va_arg(ap, i64)
                : (i64)__builtin_va_arg(ap, int);
            put_idec(value, zero, width);
            break;
        }

        case 'u': {
            u64 value = is_long
                ? __builtin_va_arg(ap, u64)
                : (u64)__builtin_va_arg(ap, unsigned int);
            put_udec(value, zero, width);
            break;
        }

        case 'x':
        case 'X': {
            u64 value = is_long
                ? __builtin_va_arg(ap, u64)
                : (u64)__builtin_va_arg(ap, unsigned int);
            put_hex(value, zero, width, conv == 'X');
            break;
        }

        case 'p': {
            u64 value = (u64)__builtin_va_arg(ap, void *);
            kputs("0x");
            put_hex(value, 1, 16, 0);
            break;
        }

        default:
            kputc('%');
            kputc(conv);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    u64 flags;

    /* Make each line atomic with respect to IRQs, APs and preemption. */
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    spin_lock(&kprintf_lock);

    __builtin_va_start(ap, fmt);
    vkprintf(fmt, ap);
    __builtin_va_end(ap);

    spin_unlock(&kprintf_lock);
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory");
}
