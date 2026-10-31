/*
 * Lantest OS - PS/2 keyboard driver (IRQ1, scancode set 1)
 *
 * The IRQ handler reads port 0x60, translates make codes to ASCII with a
 * shift table, and pushes bytes into a 256-byte ring buffer. console_read
 * drains this buffer; UART polling remains as a fallback.
 */
#include <types.h>
#include <io.h>
#include <kprintf.h>
#include <keyboard.h>
#include <console.h>
#include <x86_64/irq.h>

#define KBD_DATA      0x60
#define KBD_STATUS    0x64
#define KBD_BUF_SIZE  256u

static volatile u8 kbd_head;
static volatile u8 kbd_tail;
static char kbd_buf[KBD_BUF_SIZE];
static int kbd_shift;
static int kbd_ctrl;
static int kbd_extended;
static int kbd_ready;

static const char map_normal[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' ',
};

static const char map_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' ',
};

static void kbd_push(char c)
{
    u8 next = (u8)((kbd_head + 1) % KBD_BUF_SIZE);

    if (next == kbd_tail)
        return;   /* full: drop */
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

int keyboard_getc(char *out)
{
    if (out == NULL || kbd_tail == kbd_head)
        return 0;
    *out = kbd_buf[kbd_tail];
    kbd_tail = (u8)((kbd_tail + 1) % KBD_BUF_SIZE);
    return 1;
}

void keyboard_irq(void)
{
    u8 scancode;

    if (!kbd_ready)
        return;

    /* Only read when the output buffer has data. */
    if ((inb(KBD_STATUS) & 1u) == 0)
        return;

    scancode = inb(KBD_DATA);

    if (scancode == 0xE0) {
        kbd_extended = 1;
        return;
    }
    if (kbd_extended) {
        kbd_extended = 0;
        if (scancode & 0x80)
            return;   /* key release */
        if (scancode == 0x48) {        /* up */
            kbd_push('\x1b');
            kbd_push('[');
            kbd_push('A');
        } else if (scancode == 0x50) { /* down */
            kbd_push('\x1b');
            kbd_push('[');
            kbd_push('B');
        } else if (scancode == 0x4B) { /* left */
            kbd_push('\x1b');
            kbd_push('[');
            kbd_push('D');
        } else if (scancode == 0x4D) { /* right */
            kbd_push('\x1b');
            kbd_push('[');
            kbd_push('C');
        }
        return;
    }

    if (scancode & 0x80) {
        u8 make = (u8)(scancode & 0x7F);
        if (make == 0x2A || make == 0x36)
            kbd_shift = 0;
        if (make == 0x1D)
            kbd_ctrl = 0;
        return;
    }

    if (scancode == 0x1D) {
        kbd_ctrl = 1;
        return;
    }
    if (scancode == 0x2A || scancode == 0x36) {
        kbd_shift = 1;
        return;
    }
    if (scancode == 0x2E && kbd_ctrl) {
        if (!tty_send_sigint())
            kbd_push(0x03);
        return;
    }
    if (scancode == 0x2C && kbd_ctrl) {
        (void)tty_send_sigtstp();
        return;
    }
    if (scancode == 0x3A)
        return;   /* caps lock ignored for now */
    if (scancode >= 128)
        return;

    {
        char c = kbd_shift ? map_shift[scancode] : map_normal[scancode];
        if (c != '\0')
            kbd_push(c);
    }
}

void keyboard_init(void)
{
    /* Drain any stale bytes. */
    while ((inb(KBD_STATUS) & 1u) != 0)
        (void)inb(KBD_DATA);

    kbd_head = 0;
    kbd_tail = 0;
    kbd_shift = 0;
    kbd_ctrl = 0;
    kbd_extended = 0;
    kbd_ready = 1;

    pic_unmask(1);   /* IRQ1 */
    kprintf("[kbd] PS/2 keyboard IRQ1 enabled, scancode set 1\n");
}
