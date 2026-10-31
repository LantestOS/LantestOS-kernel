/*
 * Lantest OS - page cache with LRU + dirty writeback (M4-21)
 *
 * Read-only and MAP_SHARED file pages share cache pages. Entries keep one
 * file reference and one physical page reference. When the cache is full,
 * the LRU tail is evicted; dirty entries are written back with vfs_write_at.
 */
#include <types.h>
#include <kprintf.h>
#include <pmm.h>
#include <kmalloc.h>
#include <fs.h>
#include <vfs.h>
#include <page_cache.h>
#include <spinlock.h>

#define PCACHE_BUCKETS 64u
#define PCACHE_MAX_ENTRIES 64u

struct page_cache_entry {
    struct inode *inode;
    struct file *file;
    u64 index;
    u64 phys;
    u32 dirty;
    struct page_cache_entry *next;
    struct page_cache_entry *lru_prev;
    struct page_cache_entry *lru_next;
};

static struct page_cache_entry *buckets[PCACHE_BUCKETS];
static struct page_cache_entry *lru_head;
static struct page_cache_entry *lru_tail;
static u64 entry_count;
static u64 cache_hits;
static u64 cache_misses;
static u64 cache_evicts;
static spinlock_t pcache_lock = SPINLOCK_INIT;

static u64 pcache_hash(struct inode *inode, u64 index)
{
    u64 h = (u64)(usize)inode;

    h ^= index * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return h % PCACHE_BUCKETS;
}

static void pcache_lru_unlink(struct page_cache_entry *e)
{
    if (e->lru_prev != NULL)
        e->lru_prev->lru_next = e->lru_next;
    else
        lru_head = e->lru_next;
    if (e->lru_next != NULL)
        e->lru_next->lru_prev = e->lru_prev;
    else
        lru_tail = e->lru_prev;
    e->lru_prev = NULL;
    e->lru_next = NULL;
}

static void pcache_lru_push(struct page_cache_entry *e)
{
    e->lru_prev = NULL;
    e->lru_next = lru_head;
    if (lru_head != NULL)
        lru_head->lru_prev = e;
    lru_head = e;
    if (lru_tail == NULL)
        lru_tail = e;
}

static void pcache_lru_touch(struct page_cache_entry *e)
{
    if (lru_head == e)
        return;
    pcache_lru_unlink(e);
    pcache_lru_push(e);
}

static void pcache_hash_remove(struct page_cache_entry *e)
{
    struct page_cache_entry **link = &buckets[pcache_hash(e->inode, e->index)];

    while (*link != NULL) {
        if (*link == e) {
            *link = e->next;
            return;
        }
        link = &(*link)->next;
    }
}

/*
 * Detach one entry under pcache_lock and RETURN its file reference.
 * lfs_release() takes vfs_lock, while the syscall write path takes
 * vfs_lock and then pcache_lock (mark_dirty). Closing inside the
 * eviction would invert that order and ABBA-deadlock; the caller must
 * file_close() after dropping pcache_lock.
 */
static struct file *pcache_detach_entry(struct page_cache_entry *e)
{
    struct file *f;

    pcache_lru_unlink(e);
    pcache_hash_remove(e);
    f = e->file;
    e->file = NULL;
    pmm_free_page(e->phys);
    kfree(e);
    entry_count--;
    return f;
}

static int pcache_writeback(struct page_cache_entry *e)
{
    i64 n;

    if (!e->dirty)
        return 0;
    if (e->file == NULL)
        return -1;

    n = vfs_write_at(e->file, e->index * PAGE_SIZE,
                     (void *)(usize)e->phys, PAGE_SIZE);
    if (n < 0)
        return -1;
    e->dirty = 0;
    return 0;
}

/* Returns a file to close after pcache_lock is dropped, or NULL. */
static struct file *pcache_evict_one(void)
{
    struct page_cache_entry *e = lru_tail;

    while (e != NULL) {
        if (pmm_ref_count(e->phys) <= 1) {
            if (pcache_writeback(e) == 0) {
                cache_evicts++;
                return pcache_detach_entry(e);
            }
            return NULL;
        }
        e = e->lru_prev;
    }
    return NULL;
}

static struct page_cache_entry *pcache_lookup(struct inode *inode, u64 index)
{
    struct page_cache_entry *e;

    for (e = buckets[pcache_hash(inode, index)]; e != NULL; e = e->next) {
        if (e->inode == inode && e->index == index)
            return e;
    }
    return NULL;
}

u64 page_cache_phys(struct inode *inode, u64 page_index)
{
    u64 flags;
    u64 result = 0;

    spin_lock_irqsave(&pcache_lock, &flags);
    if (inode != NULL) {
        struct page_cache_entry *e = pcache_lookup(inode, page_index);
        if (e != NULL)
            result = e->phys;
    }
    spin_unlock_irqrestore(&pcache_lock, flags);
    return result;
}

u64 page_cache_get(struct file *file, u64 page_index, int *hit)
{
    struct inode *inode;
    struct page_cache_entry *e;
    u64 phys;
    u64 bucket;
    u64 flags;

    if (hit != NULL)
        *hit = 0;
    if (file == NULL || file->f_inode == NULL)
        return 0;

    inode = file->f_inode;
    spin_lock_irqsave(&pcache_lock, &flags);

    e = pcache_lookup(inode, page_index);
    if (e != NULL) {
        if (hit != NULL)
            *hit = 1;
        cache_hits++;
        pcache_lru_touch(e);
        if (cache_hits <= 8)
            kprintf("[pcache] hit inode=%p index=%lu phys=%p\n",
                    (void *)inode, page_index, (void *)e->phys);
        phys = e->phys;
        goto out;
    }

    while (entry_count >= PCACHE_MAX_ENTRIES) {
        u64 before = entry_count;
        struct file *evicted = pcache_evict_one();

        if (evicted != NULL) {
            spin_unlock_irqrestore(&pcache_lock, flags);
            file_close(evicted);
            spin_lock_irqsave(&pcache_lock, &flags);
        }
        if (entry_count == before)
            break;
    }

    phys = pmm_alloc_page();
    if (phys == 0)
        goto out;
    for (u32 i = 0; i < PAGE_SIZE; ++i)
        ((u8 *)(usize)phys)[i] = 0;

    e = (struct page_cache_entry *)kmalloc(sizeof(struct page_cache_entry));
    if (e == NULL) {
        pmm_free_page(phys);
        phys = 0;
        goto out;
    }

    e->inode = inode;
    e->file = file;
    e->index = page_index;
    e->phys = phys;
    e->dirty = 0;
    file_get(file);

    bucket = pcache_hash(inode, page_index);
    e->next = buckets[bucket];
    buckets[bucket] = e;
    pcache_lru_push(e);
    entry_count++;
    cache_misses++;

    if (cache_misses <= 8)
        kprintf("[pcache] miss inode=%p index=%lu phys=%p\n",
                (void *)inode, page_index, (void *)phys);

out:
    spin_unlock_irqrestore(&pcache_lock, flags);
    return phys;
}

void page_cache_mark_dirty(struct inode *inode, u64 page_index)
{
    struct page_cache_entry *e;
    u64 flags;

    spin_lock_irqsave(&pcache_lock, &flags);
    e = pcache_lookup(inode, page_index);
    if (e != NULL)
        e->dirty = 1;
    spin_unlock_irqrestore(&pcache_lock, flags);
}

void page_cache_flush(struct inode *inode, u64 start_page, u64 end_page)
{
    u64 flags;

    spin_lock_irqsave(&pcache_lock, &flags);
    for (u32 b = 0; b < PCACHE_BUCKETS; ++b) {
        struct page_cache_entry *e = buckets[b];

        while (e != NULL) {
            if (e->inode == inode && e->index >= start_page &&
                e->index < end_page && e->dirty) {
                if (pcache_writeback(e) == 0)
                    kprintf("[pcache] writeback inode=%p index=%lu\n",
                            (void *)inode, e->index);
            }
            e = e->next;
        }
    }
    spin_unlock_irqrestore(&pcache_lock, flags);
}
