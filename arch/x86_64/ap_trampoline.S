/*
 * Lantest OS - AP startup trampoline (M4-33)
 *
 * Linked at physical/virtual 0x6000. SIPI vector 0x06 enters here in 16-bit
 * real mode. The trampoline switches to protected mode, then long mode, using
 * the BSP page tables and boot parameters written at 0x7000.
 */
.section .ap_trampoline, "ax"
.code16
.global ap_trampoline_start
ap_trampoline_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    lgdt ap_gdtr

    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $ap_pm

.code32
ap_pm:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl 0x7000, %eax          /* ap_boot_params.cr3 */
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0
    ljmpl $0x18, $ap_lm

.code64
ap_lm:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq 0x7008, %rsp          /* ap_boot_params.stack_top */
    movq 0x7010, %rax          /* ap_boot_params.entry */
    call *%rax
1:
    hlt
    jmp 1b

.align 8
ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF   /* 32-bit code, base 0, limit 4G */
    .quad 0x00CF92000000FFFF   /* data, base 0, limit 4G */
    .quad 0x00AF9A000000FFFF   /* 64-bit code */
ap_gdtr:
    .word ap_gdtr - ap_gdt - 1
    .long ap_gdt

.global ap_trampoline_end
ap_trampoline_end:
