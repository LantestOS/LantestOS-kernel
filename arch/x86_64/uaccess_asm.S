/*
 * Lantest OS - assembly side of copy_from_user/copy_to_user (M3c-3)
 *
 * Each copy is a single `rep movsb`. The faulting instruction range is
 * recorded in .ex_table together with a fixup address. The #PF handler walks
 * the table; on a match it redirects RIP to the fixup, which returns -EFAULT
 * to the caller instead of panicking the kernel.
 *
 * `cld` is required: a malicious user could enter the kernel with DF=1, which
 * would make rep movsb walk backwards and corrupt memory.
 */

.section .text
.code64

.global __copy_from_user
.type __copy_from_user, @function
__copy_from_user:              /* rdi = dst, rsi = user src, rdx = len */
    cld
    movq %rdx, %rcx            /* rep movsb uses RCX as the counter */
.Lcfu_start:
    rep movsb
.Lcfu_end:
    xorl %eax, %eax            /* 0 = success */
    ret

.global __copy_to_user
.type __copy_to_user, @function
__copy_to_user:                /* rdi = user dst, rsi = kernel src, rdx = len */
    cld
    movq %rdx, %rcx            /* rep movsb uses RCX as the counter */
.Lctu_start:
    rep movsb
.Lctu_end:
    xorl %eax, %eax
    ret

/*
 * Probe helper for the SMAP self-test: read a user pointer WITHOUT stac.
 * With SMAP enabled this must fault; the fixup returns -1.
 */
.global smap_probe_user_read
.type smap_probe_user_read, @function
smap_probe_user_read:          /* rdi = user pointer */
.Lprobe_start:
    movq (%rdi), %rax
.Lprobe_end:
    ret

/* ---- fixup code (linked into .text, only reached by the #PF handler) ---- */
.section .fixup, "ax"
.code64

.Lcfu_fixup:
    movl $-14, %eax            /* -EFAULT */
    ret

.Lctu_fixup:
    movl $-14, %eax
    ret

.Lprobe_fixup:
    movq $-1, %rax
    ret

/* ---- exception table: [insn_start, insn_end), fixup ---- */
.section .ex_table, "a"
.align 8
    .quad .Lcfu_start, .Lcfu_end, .Lcfu_fixup
    .quad .Lctu_start, .Lctu_end, .Lctu_fixup
    .quad .Lprobe_start, .Lprobe_end, .Lprobe_fixup
