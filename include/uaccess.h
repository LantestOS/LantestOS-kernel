/*
 * Lantest OS - safe user memory access (copyin/copyout + SMAP)
 */
#pragma once

#include <types.h>
#include <errno.h>
#include <x86_64/isr.h>

/*
 * Initialize user-access hardening:
 *   - detect SMAP via CPUID and enable CR4.SMAP when available
 *   - map a temporary user page, verify a supervisor read is blocked
 *   - verify copy_from_user/copy_to_user and the page-fault fixup path
 * Returns 1 on success (kernel boots), 0 on failure.
 */
int uaccess_init(void);

/* Range check: pointer + length must be inside [USER_BASE, USER_STACK_TOP). */
int user_range_ok(u64 ptr, u64 len);

/* Copy to/from user memory. Returns 0 or -EFAULT. */
int copy_from_user(void *dst, const void *src, u64 len);
int copy_to_user(void *dst, const void *src, u64 len);

/*
 * Called from the #PF handler: if RIP is covered by an exception-table entry,
 * redirect execution to the fixup code and return 1 (no panic).
 */
int fixup_exception(registers_t *regs);

/* Clear AC on kernel entry (prevents user from setting AC to bypass SMAP). */
void uaccess_clear_ac(void);

int smap_is_enabled(void);
