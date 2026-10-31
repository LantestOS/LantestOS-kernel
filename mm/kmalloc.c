/*
 * Lantest OS - kmalloc/kfree: free-list heap with block headers and merging
 *
 * The heap is split into regions obtained as physically contiguous page runs
 * from the PMM (8 pages = 32 KiB by default; larger runs for big requests).
 * Each region is carved into blocks:
 *
 *    +------------------+-------------------------------+------------------+
 *    | block header     | payload (16-byte aligned)     | next block ...   |
 *    | size|free|magic  |                               |                  |
 *    +------------------+-------------------------------+------------------+
 *
 * Allocation is first-fit; free blocks split when the remainder is large
 * enough; kfree merges with the following and preceding free blocks. The
 * allocator is single-CPU and not reentrant, which is enough for M3.
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>
#include <pmm.h>
#include <kmalloc.h>
#include <spinlock.h>

#define BLOCK_MAGIC   0x4B424C4Bu   /* "KBLK" */
#define BLOCK_HDR     16u           /* sizeof(struct block) */
#define MIN_SPLIT     (BLOCK_HDR + 16u)
#define REGION_PAGES  8u            /* 32 KiB default regions */
#define MAX_REGIONS   16u

#define ALIGN16(x)    (((x) + 15u) & ~(u64)15u)

struct block {
    u32 size;        /* total block size, including this header */
    u32 free;
    u32 magic;
    u32 reserved;
};

struct region {
    u64 base;
    u64 size;
};

static struct region regions[MAX_REGIONS];
static u32 region_count;
static spinlock_t heap_lock = SPINLOCK_INIT;

static u8 *region_end(u32 index)
{
    return (u8 *)regions[index].base + regions[index].size;
}

static void region_init(u64 base, u64 size)
{
    struct block *b = (struct block *)base;

    b->size = (u32)size;
    b->free = 1;
    b->magic = BLOCK_MAGIC;
    b->reserved = 0;
}

static int add_region(u64 bytes)
{
    u32 pages;
    u64 base;

    if (region_count >= MAX_REGIONS)
        return 0;

    pages = (u32)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pages < REGION_PAGES)
        pages = REGION_PAGES;

    base = pmm_alloc_pages(pages);
    if (base == 0)
        return 0;

    regions[region_count].base = base;
    regions[region_count].size = (u64)pages * PAGE_SIZE;
    region_count++;

    region_init(base, (u64)pages * PAGE_SIZE);
    return 1;
}

void kmalloc_init(void)
{
    region_count = 0;
    if (!add_region((u64)REGION_PAGES * PAGE_SIZE))
        panic("kmalloc_init: out of physical memory");
}

static struct block *find_free_block(u64 need)
{
    for (u32 ri = 0; ri < region_count; ++ri) {
        u8 *p = (u8 *)regions[ri].base;
        u8 *end = region_end(ri);

        while (p + BLOCK_HDR <= end) {
            struct block *b = (struct block *)p;

            if (b->magic != BLOCK_MAGIC)
                panic("kmalloc: corrupt block header");
            if (b->size < BLOCK_HDR || p + b->size > end)
                panic("kmalloc: corrupt block size");
            if (b->free && (u64)b->size >= need)
                return b;

            p += b->size;
        }
    }

    return NULL;
}

void *kmalloc(usize size)
{
    u64 need;
    struct block *b;
    u64 flags;

    if (size == 0)
        return NULL;

    need = ALIGN16((u64)size) + BLOCK_HDR;

    spin_lock_irqsave(&heap_lock, &flags);
    b = find_free_block(need);
    if (b == NULL) {
        u32 pages = (u32)((need + PAGE_SIZE - 1) / PAGE_SIZE);
        if (pages < REGION_PAGES)
            pages = REGION_PAGES;
        if (!add_region((u64)pages * PAGE_SIZE)) {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }

        b = find_free_block(need);
        if (b == NULL) {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }
    }

    if ((u64)b->size >= need + MIN_SPLIT) {
        struct block *nb = (struct block *)((u8 *)b + need);

        nb->size = b->size - (u32)need;
        nb->free = 1;
        nb->magic = BLOCK_MAGIC;
        nb->reserved = 0;
        b->size = (u32)need;
    }

    b->free = 0;
    spin_unlock_irqrestore(&heap_lock, flags);
    return (u8 *)b + BLOCK_HDR;
}

static struct region *region_of(void *ptr)
{
    for (u32 i = 0; i < region_count; ++i) {
        u8 *base = (u8 *)regions[i].base;

        if ((u8 *)ptr >= base && (u8 *)ptr < base + regions[i].size)
            return &regions[i];
    }

    return NULL;
}

void kfree(void *ptr)
{
    struct block *b;
    struct region *r;
    u8 *end;
    u64 flags;

    if (ptr == NULL)
        return;

    spin_lock_irqsave(&heap_lock, &flags);

    if (((usize)ptr & 15u) != 0)
        panic("kfree: unaligned pointer");

    b = (struct block *)((u8 *)ptr - BLOCK_HDR);
    if (b->magic != BLOCK_MAGIC)
        panic("kfree: bad block magic");
    if (b->free)
        panic("kfree: double free");

    r = region_of(b);
    if (r == NULL)
        panic("kfree: pointer outside heap");

    end = (u8 *)r->base + r->size;
    if ((u8 *)b < (u8 *)r->base || (u8 *)b + b->size > end)
        panic("kfree: block outside its region");

    b->free = 1;

    /* Merge with following free blocks. */
    for (;;) {
        struct block *nb = (struct block *)((u8 *)b + b->size);

        if ((u8 *)nb + BLOCK_HDR > end)
            break;
        if (nb->magic != BLOCK_MAGIC)
            panic("kfree: corrupt next block");
        if (!nb->free)
            break;
        b->size += nb->size;
    }

    /* Merge with a preceding free block, if any. */
    {
        u8 *p = (u8 *)r->base;
        struct block *prev = NULL;

        while (p < (u8 *)b) {
            struct block *cur = (struct block *)p;

            if (cur->magic != BLOCK_MAGIC)
                panic("kfree: corrupt block chain");
            prev = cur;
            p += cur->size;
        }

        if (prev != NULL && prev->free) {
            prev->size += b->size;
        }
    }

    spin_unlock_irqrestore(&heap_lock, flags);
}

void kmalloc_stats(u64 *region_count_out, u64 *used_bytes,
                   u64 *free_bytes, u64 *largest_free)
{
    u64 used = 0;
    u64 freed = 0;
    u64 largest = 0;

    for (u32 ri = 0; ri < region_count; ++ri) {
        u8 *p = (u8 *)regions[ri].base;
        u8 *end = region_end(ri);

        while (p + BLOCK_HDR <= end) {
            struct block *b = (struct block *)p;

            if (b->magic != BLOCK_MAGIC)
                break;
            if (b->free) {
                freed += b->size;
                if (b->size > largest)
                    largest = b->size;
            } else {
                used += b->size;
            }
            p += b->size;
        }
    }

    if (region_count_out != NULL)
        *region_count_out = region_count;
    if (used_bytes != NULL)
        *used_bytes = used;
    if (free_bytes != NULL)
        *free_bytes = freed;
    if (largest_free != NULL)
        *largest_free = largest;
}

static int pattern_check(void *ptr, usize size, u32 tag)
{
    u8 *p = (u8 *)ptr;

    for (usize i = 0; i < size; ++i) {
        if (p[i] != (u8)(tag + i * 7u))
            return 0;
    }

    return 1;
}

static void pattern_fill(void *ptr, usize size, u32 tag)
{
    u8 *p = (u8 *)ptr;

    for (usize i = 0; i < size; ++i)
        p[i] = (u8)(tag + i * 7u);
}

void kmalloc_self_test(void)
{
    u64 regions0, used0, free0, largest0;
    u64 regions1, used1, free1, largest1;
    u64 total_region_bytes = 0;
    u64 max_region_bytes = 0;
    int ok = 1;

    kmalloc_stats(&regions0, &used0, &free0, &largest0);

    void *a = kmalloc(16);
    void *b = kmalloc(64);
    void *c = kmalloc(1000);
    void *d = kmalloc(3000);
    void *e = kmalloc(5000);

    if (a == NULL || b == NULL || c == NULL || d == NULL || e == NULL)
        ok = 0;

    if (ok) {
        pattern_fill(a, 16, 0x11);
        pattern_fill(b, 64, 0x22);
        pattern_fill(c, 1000, 0x33);
        pattern_fill(d, 3000, 0x44);
        pattern_fill(e, 5000, 0x55);

        if (!pattern_check(a, 16, 0x11) || !pattern_check(b, 64, 0x22) ||
            !pattern_check(c, 1000, 0x33) || !pattern_check(d, 3000, 0x44) ||
            !pattern_check(e, 5000, 0x55))
            ok = 0;
    }

    kfree(b);
    kfree(a);
    kfree(e);
    kfree(d);
    kfree(c);

    kmalloc_stats(&regions1, &used1, &free1, &largest1);

    for (u32 i = 0; i < region_count; ++i) {
        total_region_bytes += regions[i].size;
        if (regions[i].size > max_region_bytes)
            max_region_bytes = regions[i].size;
    }

    /* Everything must be free again and each region fully coalesced. */
    if (used1 != 0 || free1 != total_region_bytes || largest1 != max_region_bytes)
        ok = 0;

    kprintf("[kmalloc] self-test: %s\n", ok ? "PASS" : "FAIL");
    kprintf("[kmalloc] regions=%lu used=%lu free=%lu largest=%lu\n",
            regions1, used1, free1, largest1);
    (void)used0;
    (void)free0;
    (void)largest0;
}
