/*
 * Lantest OS - physical page frame allocator
 *
 * Bitmap allocator over 4 KiB pages, managing the first 1 GiB of physical
 * memory (the range covered by the identity map built in boot.S). Pages are
 * marked free from the Multiboot2 memory map, then the low 1 MiB, the kernel
 * image and the Multiboot2 information structure are reserved again.
 *
 * Single CPU, no locking (SMP comes much later).
 */
#include <types.h>
#include <kprintf.h>
#include <multiboot2.h>
#include <pmm.h>
#include <spinlock.h>

#define MAX_PHYS_ADDR  (1ULL << 30)                 /* identity-mapped range */
#define MAX_PAGES      (MAX_PHYS_ADDR / PAGE_SIZE)  /* 262144 frames */
#define BITMAP_BYTES   (MAX_PAGES / 8)              /* 32 KiB */
#define LOW_MEM_END    0x100000ULL
#define FIRST_FREE_PFN (LOW_MEM_END / PAGE_SIZE)

static u8  bitmap[BITMAP_BYTES];
static u16 refcount[MAX_PAGES];   /* per-frame reference count */
static u64 free_pages;
static u64 total_pages;
static u64 alloc_hint;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static int bit_test(u64 pfn)
{
    return (bitmap[pfn >> 3] >> (pfn & 7)) & 1;
}

static void bit_set(u64 pfn)
{
    bitmap[pfn >> 3] |= (u8)(1u << (pfn & 7));
}

static void bit_clear(u64 pfn)
{
    bitmap[pfn >> 3] &= (u8)~(1u << (pfn & 7));
}

static u64 align_up(u64 value)
{
    return (value + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
}

static u64 align_down(u64 value)
{
    return value & ~(u64)(PAGE_SIZE - 1);
}

static void mark_used(u64 start, u64 end)
{
    if (end > MAX_PHYS_ADDR)
        end = MAX_PHYS_ADDR;
    start = align_up(start);
    end = align_down(end);

    for (u64 pfn = start / PAGE_SIZE; pfn < end / PAGE_SIZE; ++pfn) {
        if (!bit_test(pfn)) {
            bit_set(pfn);
            free_pages--;
        }
    }
}

static void mark_free(u64 start, u64 end)
{
    if (end > MAX_PHYS_ADDR)
        end = MAX_PHYS_ADDR;
    start = align_up(start);
    end = align_down(end);
    if (start >= end)
        return;

    for (u64 pfn = start / PAGE_SIZE; pfn < end / PAGE_SIZE; ++pfn) {
        if (bit_test(pfn)) {
            if (refcount[pfn] > 1) {
                refcount[pfn]--;
            } else {
                refcount[pfn] = 0;
                bit_clear(pfn);
                free_pages++;
            }
        }
    }
}

void pmm_init(u64 mb2_info, u64 kernel_start, u64 kernel_end)
{
    const struct mb2_tag_mmap *mmap;
    u64 usable_bytes = 0;
    u32 usable_regions = 0;
    u32 shown = 0;

    /* Start with everything reserved; free only what the firmware reports. */
    for (usize i = 0; i < BITMAP_BYTES; ++i)
        bitmap[i] = 0xFF;

    for (u64 i = 0; i < MAX_PAGES; ++i)
        refcount[i] = 0;

    free_pages = 0;
    total_pages = MAX_PAGES;
    alloc_hint = FIRST_FREE_PFN;

    mmap = mb2_find_mmap(mb2_info);
    if (mmap == NULL) {
        kprintf("[pmm] ERROR: Multiboot2 memory map not found, allocator off\n");
        return;
    }

    u32 entries = (mmap->size - (u32)sizeof(struct mb2_tag_mmap)) / mmap->entry_size;
    const u8 *p = (const u8 *)mmap + sizeof(struct mb2_tag_mmap);

    for (u32 i = 0; i < entries; ++i, p += mmap->entry_size) {
        const struct mb2_mmap_entry *entry = (const struct mb2_mmap_entry *)p;
        u64 addr;
        u64 end;

        if (entry->type != MB2_MEMORY_AVAILABLE)
            continue;

        addr = entry->addr;
        end = entry->addr + entry->len;
        if (addr >= MAX_PHYS_ADDR)
            continue;
        if (end > MAX_PHYS_ADDR)
            end = MAX_PHYS_ADDR;

        usable_bytes += end - addr;
        usable_regions++;
        mark_free(addr, end);

        if (shown < 8) {
            kprintf("[mem ] usable %p-%p  %lu KiB\n",
                    (void *)addr, (void *)end, (end - addr) / 1024);
            shown++;
        }
    }

    /* Reserve low memory (BIOS/VGA), the kernel image and the MB2 info. */
    mark_used(0, LOW_MEM_END);
    mark_used(kernel_start, kernel_end);
    if (mb2_total_size(mb2_info) != 0)
        mark_used(mb2_info, mb2_info + mb2_total_size(mb2_info));

    kprintf("[pmm] usable RAM (<=1 GiB): %lu MiB in %u regions\n",
            usable_bytes / (1024 * 1024), usable_regions);
    kprintf("[pmm] page frames: total=%lu free=%lu\n", total_pages, free_pages);
}

u64 pmm_alloc_page(void)
{
    u64 flags;
    u64 result = 0;

    spin_lock_irqsave(&pmm_lock, &flags);
    /* First-fit scan from the allocation hint, wrapping to the start. */
    for (u64 pfn = alloc_hint; pfn < MAX_PAGES; ++pfn) {
        if (!bit_test(pfn)) {
            bit_set(pfn);
            refcount[pfn] = 1;
            free_pages--;
            alloc_hint = pfn + 1;
            result = pfn * PAGE_SIZE;
            goto out;
        }
    }
    for (u64 pfn = FIRST_FREE_PFN; pfn < alloc_hint; ++pfn) {
        if (!bit_test(pfn)) {
            bit_set(pfn);
            refcount[pfn] = 1;
            free_pages--;
            alloc_hint = pfn + 1;
            result = pfn * PAGE_SIZE;
            goto out;
        }
    }
out:
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_alloc_pages(u32 count)
{
    u64 run = 0;
    u64 start = 0;
    u64 flags;
    u64 result = 0;

    if (count == 0)
        return 0;

    spin_lock_irqsave(&pmm_lock, &flags);

    for (u64 pfn = FIRST_FREE_PFN; pfn < MAX_PAGES; ++pfn) {
        if (bit_test(pfn)) {
            run = 0;
            continue;
        }

        if (run == 0)
            start = pfn;
        run++;

        if (run == count) {
            for (u64 p = start; p <= pfn; ++p) {
                bit_set(p);
                refcount[p] = 1;
                free_pages--;
            }
            alloc_hint = pfn + 1;
            result = start * PAGE_SIZE;
            goto out;
        }
    }

out:
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

void pmm_free_page(u64 addr)
{
    u64 pfn;
    u64 flags;

    if ((addr & (PAGE_SIZE - 1)) != 0)
        return;
    spin_lock_irqsave(&pmm_lock, &flags);

    pfn = addr / PAGE_SIZE;
    if (pfn < FIRST_FREE_PFN || pfn >= MAX_PAGES)
        return;
    if (!bit_test(pfn))
        return;

    if (refcount[pfn] > 1) {
        refcount[pfn]--;
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }

    refcount[pfn] = 0;
    bit_clear(pfn);
    free_pages++;
    if (pfn < alloc_hint)
        alloc_hint = pfn;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_ref_inc(u64 addr)
{
    u64 pfn = addr / PAGE_SIZE;
    u64 flags;

    if ((addr & (PAGE_SIZE - 1)) != 0 || pfn >= MAX_PAGES)
        return;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (bit_test(pfn) && refcount[pfn] < 0xFFFF)
        refcount[pfn]++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

u64 pmm_ref_count(u64 addr)
{
    u64 pfn = addr / PAGE_SIZE;

    if ((addr & (PAGE_SIZE - 1)) != 0 || pfn >= MAX_PAGES)
        return 0;
    return refcount[pfn];
}

void pmm_free_pages(u64 addr, u32 count)
{
    u64 start;
    u64 flags;

    if (count == 0 || (addr & (PAGE_SIZE - 1)) != 0)
        return;

    start = addr / PAGE_SIZE;
    if (start < FIRST_FREE_PFN)
        return;
    spin_lock_irqsave(&pmm_lock, &flags);

    for (u32 i = 0; i < count; ++i) {
        u64 pfn = start + i;

        if (pfn >= MAX_PAGES)
            break;
        if (bit_test(pfn)) {
            if (refcount[pfn] > 1) {
                refcount[pfn]--;
            } else {
                refcount[pfn] = 0;
                bit_clear(pfn);
                free_pages++;
            }
        }
    }

    if (start < alloc_hint)
        alloc_hint = start;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

u64 pmm_free_count(void)
{
    return free_pages;
}

u64 pmm_total_count(void)
{
    return total_pages;
}

void pmm_self_test(void)
{
    u64 p1 = pmm_alloc_page();
    u64 p2 = pmm_alloc_page();
    u64 p3 = pmm_alloc_page();
    volatile u64 *a;
    volatile u64 *b;
    volatile u64 *c;
    const u32 words = PAGE_SIZE / sizeof(u64);
    int ok = 1;

    if (p1 == 0 || p2 == 0 || p3 == 0) {
        kprintf("[pmm] self-test: FAIL (out of memory)\n");
        return;
    }

    a = (volatile u64 *)p1;
    b = (volatile u64 *)p2;
    c = (volatile u64 *)p3;

    for (u32 i = 0; i < words; ++i) {
        a[i] = 0xAAAAAAAA00000000ULL | i;
        b[i] = 0xBBBBBBBB00000000ULL | i;
        c[i] = 0xCCCCCCCC00000000ULL | i;
    }

    for (u32 i = 0; i < words; ++i) {
        if (a[i] != (0xAAAAAAAA00000000ULL | i))
            ok = 0;
        if (b[i] != (0xBBBBBBBB00000000ULL | i))
            ok = 0;
        if (c[i] != (0xCCCCCCCC00000000ULL | i))
            ok = 0;
    }

    /* Contiguous run test (heap regions / kernel stacks need this). */
    u64 contig = pmm_alloc_pages(4);
    if (contig == 0) {
        ok = 0;
    } else {
        volatile u32 *cp = (volatile u32 *)contig;

        for (u32 i = 0; i < (PAGE_SIZE / 4) * 4; ++i)
            cp[i] = 0xC0FFEE00u | i;
        for (u32 i = 0; i < (PAGE_SIZE / 4) * 4; ++i) {
            if (cp[i] != (0xC0FFEE00u | i))
                ok = 0;
        }
        pmm_free_pages(contig, 4);
    }

    u64 free_before = free_pages;
    pmm_free_page(p2);
    if (free_pages != free_before + 1)
        ok = 0;

    kprintf("[pmm] self-test: %s\n", ok ? "PASS" : "FAIL");
    kprintf("[pmm] pages: %p %p %p, contiguous=%p free after test: %lu\n",
            (void *)p1, (void *)p2, (void *)p3, (void *)contig, free_pages);
}
