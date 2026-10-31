/*
 * Lantest OS - linker script (x86_64, Multiboot2 / GRUB)
 *
 * The kernel is loaded by GRUB at the physical address 1 MiB, which is the
 * conventional load address for Multiboot2 kernels on x86.
 *
 * boot.S builds an identity mapping (virtual address == physical address) for
 * the first 1 GiB, so the addresses produced by this script are both the
 * physical load addresses and the virtual addresses used by the kernel.
 *
 * _kernel_start/_kernel_end delimit the image for the physical page allocator;
 * __ex_table_start/__ex_table_end delimit the user-copy exception table.
 */

ENTRY(_start)

SECTIONS
{
    /* AP SIPI trampoline must be reachable at physical 0x6000. */
    . = 0x6000;
    .ap_trampoline : ALIGN(16)
    {
        KEEP(*(.ap_trampoline))
    }

    . = 1M;
    _kernel_start = .;

    /*
     * Multiboot2 header must be inside the first 32 KiB of the file and
     * 8-byte aligned. Keeping it as the very first section satisfies both.
     */
    .multiboot2 : ALIGN(8)
    {
        KEEP(*(.multiboot2))
    }

    .text : ALIGN(4K)
    {
        *(.text .text.*)
        *(.fixup)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    /*
     * Exception table for copy_from_user/copy_to_user fixups.
     * Entry format: { insn_start, insn_end, fixup } (see include/uaccess.h).
     */
    .ex_table : ALIGN(8)
    {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    }

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
    }
}
