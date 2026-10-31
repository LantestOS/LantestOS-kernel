/*
 * Lantest OS - user program linker script
 *
 * PHDRS keeps two PT_LOAD segments:
 *   - text segment (RX) at USER_BASE
 *   - data segment (RW) just after the RX data, kept non-page-aligned and,
 *     when possible, inside the same 4 KiB page as the tail of RX data.
 * This still exercises the page-sharing ELF loader path, while allowing
 * programs (like the shell) whose text grows beyond one page.
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x0000008000000000;

    .text : ALIGN(16)
    {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } :text

    /* Page-align the RW segment so PT_LOADs never share a page. */
    . = ALIGN(4096);

    .data :
    {
        *(.data .data.*)
    } :data

    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
        *(.debug*)
        *(.line)
    }
}
