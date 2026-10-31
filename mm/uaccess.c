/*
 * Lantest OS - safe user memory access (M3c-3)
 *
 * copy_from_user/copy_to_user use small assembly routines with exception-table
 * fixups. If the user pointer is not mapped, the #PF handler redirects RIP to
 * the fixup code, which returns -EFAULT instead of panicking the kernel.
 *
 * When the CPU supports SMAP, CR4.SMAP is enabled: Ring 0 cannot touch user
 * pages unless AC is set. stac/clac bracket only the copy routines, so any
 * accidental direct dereference of a user pointer is caught by #PF.
 */
#include <types.h>
#include <kprintf.h>
#include <pmm.h>
#include <uaccess.h>
#include <x86_64/paging.h>
#include <x86_64/usermode.h>

struct ex_entry {
    u64 insn;
    u64 end;
    u64 fixup;
} __attribute__((packed));

/* Defined by the linker script around the .ex_table section. */
extern const struct ex_entry __ex_table_start[];
extern const struct ex_entry __ex_table_end[];

extern i64 __copy_from_user(void *dst, const void *src, u64 len);
extern i64 __copy_to_user(void *dst, const void *src, u64 len);
extern i64 smap_probe_user_read(const void *ptr);

static int smap_on;

/* ------------------------------- CPUID / CR4 ------------------------------ */

static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    u32 eax = leaf;
    u32 ecx = sub;
    u32 ebx;
    u32 edx;

    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    *a = eax;
    *b = ebx;
    *c = ecx;
    *d = edx;
}

static int cpu_has_smap(void)
{
    u32 a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7)
        return 0;

    cpuid(7, 0, &a, &b, &c, &d);
    return (b & (1u << 20)) != 0;   /* CPUID.(EAX=7):EBX[20] = SMAP */
}

static u64 read_cr4(void)
{
    u64 value;

    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr4(u64 value)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(value) : "memory");
}

static void enable_smap(void)
{
    write_cr4(read_cr4() | (1ULL << 21));   /* CR4.SMAP */
    smap_on = 1;
}

static inline void stac(void)
{
    if (smap_on)
        __asm__ volatile ("stac" : : : "memory");
}

static inline void clac(void)
{
    if (smap_on)
        __asm__ volatile ("clac" : : : "memory");
}

int smap_is_enabled(void)
{
    return smap_on;
}

void uaccess_clear_ac(void)
{
    clac();
}

/* --------------------------- user range checks ---------------------------- */

int user_range_ok(u64 ptr, u64 len)
{
    if (ptr < USER_BASE)
        return 0;
    if (ptr >= USER_STACK_TOP)
        return 0;
    if (ptr + len < ptr)
        return 0;
    if (len > USER_STACK_TOP - ptr)
        return 0;
    return 1;
}

int copy_from_user(void *dst, const void *src, u64 len)
{
    i64 ret;

    if (len == 0)
        return 0;
    if (!user_range_ok((u64)(usize)src, len))
        return -EFAULT;

    stac();
    ret = __copy_from_user(dst, src, len);
    clac();

    return (int)ret;
}

int copy_to_user(void *dst, const void *src, u64 len)
{
    i64 ret;

    if (len == 0)
        return 0;
    if (!user_range_ok((u64)(usize)dst, len))
        return -EFAULT;

    stac();
    ret = __copy_to_user(dst, src, len);
    clac();

    return (int)ret;
}

int fixup_exception(registers_t *regs)
{
    const struct ex_entry *e = __ex_table_start;

    while (e < __ex_table_end) {
        if (regs->rip >= e->insn && regs->rip < e->end) {
            regs->rip = e->fixup;
            return 1;
        }
        ++e;
    }

    return 0;
}

/* ------------------------------ boot self-test ---------------------------- */

int uaccess_init(void)
{
    int supported = cpu_has_smap();
    int ok = 1;
    u64 pml4 = paging_read_cr3();
    u64 page_phys;
    volatile u64 *page;
    u8 kbuf[16];
    u64 pattern = 0xDEADBEEFCAFEBABEULL;

    if (supported) {
        enable_smap();
        kprintf("[uaccess] SMAP supported, enabled (CR4.SMAP=1)\n");
    } else {
        kprintf("[uaccess] SMAP not supported by CPU; using fixup only\n");
    }

    page_phys = pmm_alloc_page();
    if (page_phys == 0)
        return 0;

    page = (volatile u64 *)(usize)page_phys;
    for (u32 i = 0; i < PAGE_SIZE; ++i)
        ((u8 *)page)[i] = (u8)(0xA0 + i);

    if (paging_map_user(pml4, USER_BASE, page_phys, USER_PAGE_FLAGS) != 0) {
        pmm_free_page(page_phys);
        return 0;
    }

    /* 1. With SMAP on, a supervisor read of a user page must fault and be
     *    recovered by the exception table (returns -1, no panic). */
    if (smap_on) {
        if (smap_probe_user_read((const void *)USER_BASE) != (i64)-1) {
            kprintf("[uaccess] SMAP probe did NOT fault - enforcement broken\n");
            ok = 0;
        }
    }

    /* 2. copy_from_user from a mapped user page. */
    for (u32 i = 0; i < sizeof(kbuf); ++i)
        kbuf[i] = 0;
    if (copy_from_user(kbuf, (const void *)USER_BASE, sizeof(kbuf)) != 0) {
        ok = 0;
    } else {
        for (u32 i = 0; i < sizeof(kbuf); ++i) {
            if (kbuf[i] != (u8)(0xA0 + i))
                ok = 0;
        }
    }

    /* 3. copy_to_user to the same page, verified through the identity map. */
    if (copy_to_user((void *)USER_BASE, &pattern, sizeof(pattern)) != 0)
        ok = 0;
    else if (page[0] != pattern)
        ok = 0;

    /* 4. Unmapped page inside the user range: fixup must return -EFAULT. */
    if (copy_from_user(kbuf, (const void *)(USER_BASE + 0x2000),
                       sizeof(kbuf)) != -EFAULT)
        ok = 0;

    /* 5. Kernel pointer: rejected by the range check without touching memory. */
    if (copy_from_user(kbuf, (const void *)0x100000, sizeof(kbuf)) != -EFAULT)
        ok = 0;

    /* The test mapping lives in PML4[1]; drop the subtree but keep the kernel
     * PML4 itself (we are still running on it). */
    paging_destroy_user_mappings(pml4);

    kprintf("[uaccess] self-test: %s (fixup, copy in/out%s)\n",
            ok ? "PASS" : "FAIL",
            smap_on ? ", SMAP probe blocked" : ", SMAP off");
    return ok;
}
