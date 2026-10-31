/*
 * Lantest OS - user mode constants and iretq entry
 */
#pragma once

#include <types.h>
#include <x86_64/isr.h>

/*
 * Ring 3 mappings live in PML4[1] (512 GiB) because the kernel identity map
 * occupies PML4[0] (low 1 GiB) with U/S = 0. The kernel's own mappings stay
 * untouched in every task PML4.
 */
#define USER_BASE       0x0000008000000000ULL        /* 512 GiB */
#define USER_STACK_TOP  (USER_BASE + 0x00100000ULL)  /* 1 MiB above base */
#define USER_STACK_PAGES 16u                      /* 64 KiB user stack */
#define USER_PAGE_FLAGS 0x7ULL                       /* P | W | U */

/*
 * Build an iretq frame (SS, RSP, RFLAGS, CS, RIP) and drop to Ring 3.
 * Never returns.
 */
void user_enter(u64 entry, u64 user_stack_top) __attribute__((noreturn));

/*
 * Return to Ring 3 using a saved registers_t frame (used by fork: the child
 * resumes at the parent's post-int 0x80 RIP with rax = 0).
 * Never returns.
 */
void user_enter_regs(registers_t *regs) __attribute__((noreturn));
