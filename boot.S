/*
 * Lantest OS - M1 boot entry (x86_64)
 *
 * GRUB / Multiboot2 enters this file in 32-bit protected mode. The bootstrap
 * below:
 *   1. saves the Multiboot2 arguments,
 *   2. verifies that the CPU supports long mode (x86_64),
 *   3. builds a minimal identity mapping for the first 1 GiB (2 MiB pages),
 *   4. enables PAE, long mode (EFER.LME) and paging,
 *   5. loads a flat 64-bit GDT and far-jumps into 64-bit code,
 *   6. sets up a 16 KiB stack and calls kernel_main().
 *
 * This is an independent implementation written against the public x86_64
 * and Multiboot2 specifications. No Linux source code, headers or ABI are
 * used.
 */

/*
 * Author's note (deliberately kept in the lowest-level boot code):
 *   1. I live you LantestOS.
 *   2. Chinese81192.
 */

/* ------------------------------------------------------------------ */
/* Multiboot2 header                                                   */
/* Must be 8-byte aligned and located in the first 32 KiB of the file. */
/* ------------------------------------------------------------------ */
.section .multiboot2, "a"
.align 8
mb2_header:
    .long 0xE85250D6                                  /* magic */
    .long 0                                           /* architecture: i386 */
    .long mb2_header_end - mb2_header                 /* header length */
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))  /* checksum */

    .align 8
    .short 0                                          /* end tag */
    .short 0
    .long 8
mb2_header_end:

/* ------------------------------------------------------------------ */
/* 32-bit entry point                                                  */
/* ------------------------------------------------------------------ */
.section .text
.code32
.global _start
.extern kernel_main

_start:
    cli

    /* GRUB: EAX = 0x36D76289, EBX = Multiboot2 info pointer */
    mov %eax, %ebp
    mov %ebx, %esi

    /*
     * Zero the page tables ourselves. GRUB normally zeroes .bss, but the
     * bootstrap must not rely on the bootloader for this.
     */
    mov $page_tables_start, %edi
    mov $(page_tables_end - page_tables_start), %ecx
    shr $2, %ecx
    xor %eax, %eax
    cld
    rep stosl

    /* Is long mode available? CPUID.80000001H:EDX[29] = LM */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode

    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz no_long_mode

    /* PML4[0] -> PDPT (present, writable) */
    mov $pml4, %eax
    mov $pdpt, %edx
    or $0x3, %edx
    mov %edx, (%eax)

    /* PDPT[0] -> PD (present, writable) */
    mov $pdpt, %eax
    mov $pd, %edx
    or $0x3, %edx
    mov %edx, (%eax)

    /* PD[i] = i * 2 MiB, flags: present | writable | 2 MiB page */
    mov $pd, %edi
    xor %ecx, %ecx
1:
    mov %ecx, %edx
    shl $21, %edx
    or $0x83, %edx
    mov %edx, (%edi, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jb 1b

    /* CR4.PAE + OSFXSR + OSXMMEXCPT */
    mov %cr4, %eax
    or $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    mov %eax, %cr4

    /* IA32_EFER.LME = 1 */
    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr

    /* CR3 = PML4 base */
    mov $pml4, %eax
    mov %eax, %cr3

    /* CR0: MP|NE, clear EM/TS, PG=1, PE=1; then init x87/SSE */
    mov %cr0, %eax
    and $0xFFFFFFF3, %eax
    or $0x80000023, %eax
    mov %eax, %cr0
    fninit

    /* Load the 64-bit GDT and far-jump into long mode */
    lgdt gdt64_descriptor
    ljmp $0x08, $long_mode_entry

no_long_mode:
    /* CPU or environment without long mode: there is nothing to boot. */
    cli
    hlt
    jmp no_long_mode

/* ------------------------------------------------------------------ */
/* 64-bit entry point                                                  */
/* ------------------------------------------------------------------ */
.code64
long_mode_entry:
    /* Flat data segments (selector 0x10) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* Boot stack: 16 KiB in .bss, 16-byte aligned */
    mov $stack_top, %rsp

    /* kernel_main(mb2_info, mb2_magic); SysV AMD64 calling convention */
    mov %esi, %edi
    mov %ebp, %esi
    xor %rbp, %rbp
    call kernel_main

halt:
    cli
    hlt
    jmp halt

/* ------------------------------------------------------------------ */
/* Global descriptor table (flat 64-bit code and data segments)        */
/* ------------------------------------------------------------------ */
.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000        /* 0x00: null descriptor */
    .quad 0x00AF9A000000FFFF        /* 0x08: kernel code, DPL 0, 64-bit */
    .quad 0x00AF92000000FFFF        /* 0x10: kernel data, DPL 0 */
gdt64_end:

/*
 * Loaded while still in 32-bit mode, so the base address is stored as a
 * 32-bit value (the kernel lives below 4 GiB).
 */
gdt64_descriptor:
    .word gdt64_end - gdt64 - 1
    .long gdt64

/* ------------------------------------------------------------------ */
/* Boot stack and page tables (.bss)                                   */
/* ------------------------------------------------------------------ */
.section .bss
.align 4096
page_tables_start:
pml4:
    .skip 4096
.align 4096
pdpt:
    .skip 4096
.align 4096
pd:
    .skip 4096
page_tables_end:

.align 16
stack_bottom:
    .skip 16384
stack_top:
