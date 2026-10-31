/*
 * Lantest OS - VGA text console
 *
 * 80x25 characters, light grey on black, buffer at physical 0xB8000
 * (identity mapped, see boot.S).
 */
#include <types.h>
#include <io.h>
#include <vga.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x07

static volatile u16 *const vga_buf = (volatile u16 *)0xB8000;
static u32 vga_row;
static u32 vga_col;

static u16 vga_entry(char c, u8 attr)
{
    return (u16)((u8)c | ((u16)attr << 8));
}

static void vga_scroll(void)
{
    for (u32 i = 0; i < (VGA_ROWS - 1) * VGA_COLS; ++i)
        vga_buf[i] = vga_buf[i + VGA_COLS];

    for (u32 i = 0; i < VGA_COLS; ++i)
        vga_buf[(VGA_ROWS - 1) * VGA_COLS + i] = vga_entry(' ', VGA_ATTR);

    vga_row = VGA_ROWS - 1;
}

static void vga_update_cursor(void)
{
    u16 pos = (u16)(vga_row * VGA_COLS + vga_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)(pos >> 8));
}

void vga_clear(void)
{
    for (u32 i = 0; i < VGA_ROWS * VGA_COLS; ++i)
        vga_buf[i] = vga_entry(' ', VGA_ATTR);

    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_init(void)
{
    vga_clear();
}

void vga_putc(char c)
{
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS)
            vga_scroll();
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        vga_buf[vga_row * VGA_COLS + vga_col] = vga_entry(c, VGA_ATTR);
        if (++vga_col >= VGA_COLS) {
            vga_col = 0;
            if (++vga_row >= VGA_ROWS)
                vga_scroll();
        }
    }
    vga_update_cursor();
}
