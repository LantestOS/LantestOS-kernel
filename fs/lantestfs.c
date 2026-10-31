/*
 * Lantest OS - LantestFS persistent filesystem (M5-2)
 *
 * Layout on a 16 MiB virtio-blk disk (4 KiB blocks):
 *   block 0            superblock
 *   block 1            block bitmap
 *   block 2            inode bitmap
 *   blocks 3..10       inode table (256 inodes * 128 bytes)
 *   blocks 11..        data blocks
 *
 * Files use up to 12 direct 4 KiB blocks (48 KiB) for now. All metadata is
 * read/written through an 8-entry read cache; writes are write-through so a
 * shell-created file survives a reboot without an explicit sync.
 */
#include <types.h>
#include <kprintf.h>
#include <kmalloc.h>
#include <errno.h>
#include <vfs.h>
#include <fs.h>
#include <lantestfs.h>
#include <virtio_blk.h>

#define LFS_BLOCK_SIZE 4096u
#define LFS_SECTORS_PER_BLOCK 8u
#define LFS_MAGIC "LNTSTFS2"
#define LFS_VERSION 2u

#define LFS_INODE_COUNT 256u
#define LFS_INODE_SIZE 128u
#define LFS_INODES_PER_BLOCK (LFS_BLOCK_SIZE / LFS_INODE_SIZE)
#define LFS_INODE_TABLE_START 3u
#define LFS_INODE_TABLE_BLOCKS 8u
#define LFS_INODE_BITMAP_BLOCK 2u
#define LFS_BLOCK_BITMAP_BLOCK 1u
#define LFS_DATA_START 11u
#define LFS_DIRENT_SIZE 64u
#define LFS_NAME_MAX 60u
#define LFS_DIRECT_BLOCKS 12u
#define LFS_INDIRECT_ENTRIES (LFS_BLOCK_SIZE / 4u)
#define LFS_MAX_BLOCKS (LFS_DIRECT_BLOCKS + LFS_INDIRECT_ENTRIES)
#define LFS_CACHE_ENTRIES 8u
#define LFS_JOURNAL_BLOCKS 32u
#define LFS_JOURNAL_MAGIC 0x4C4A524Eu   /* LJRN */
#define LFS_JOURNAL_COMMIT 0x434D4954u  /* CMIT */

struct lfs_superblock {
    u8 magic[8];
    u32 version;
    u32 block_size;
    u32 total_blocks;
    u32 data_start;
    u32 inode_count;
    u32 inode_table_start;
    u32 inode_table_blocks;
    u32 inode_bitmap_block;
    u32 block_bitmap_block;
    u32 root_ino;
    u32 reserved;
    u8 pad[LFS_BLOCK_SIZE - (8 + 11 * 4)];
} __attribute__((packed));

struct lfs_inode_disk {
    u32 mode;
    u32 nlink;
    u32 size;
    u32 flags;
    u32 direct[LFS_DIRECT_BLOCKS];
    u32 indirect;
    u8 pad[LFS_INODE_SIZE - (4 * 4 + LFS_DIRECT_BLOCKS * 4 + 4)];
} __attribute__((packed));

struct lfs_dirent {
    u32 ino;
    char name[LFS_NAME_MAX];
} __attribute__((packed));

static_assert(sizeof(struct lfs_superblock) == LFS_BLOCK_SIZE,
              "lfs_superblock must be one block");
static_assert(sizeof(struct lfs_inode_disk) == LFS_INODE_SIZE,
              "lfs_inode_disk must be 128 bytes");
static_assert(sizeof(struct lfs_dirent) == LFS_DIRENT_SIZE,
              "lfs_dirent must be 64 bytes");

struct lfs_cache_entry {
    u32 block;
    u32 age;
    int valid;
    u8 data[LFS_BLOCK_SIZE];
};

struct lfs_journal_header {
    u32 magic;
    u32 commit;
    u32 count;
    u32 target;
    u32 slot;
    u8 pad[LFS_BLOCK_SIZE - 20];
} __attribute__((packed));

static struct lfs_superblock lfs_sb;
static struct lfs_cache_entry lfs_cache[LFS_CACHE_ENTRIES];
static u32 lfs_cache_age;
static int lfs_ready;
static int lfs_is_mounted;
static int lfs_journal_enabled;
static struct dentry *lfs_root_dentry;
static u8 lfs_zero[LFS_BLOCK_SIZE];

extern const struct file_operations lfs_file_ops;

static const u8 lfs_selftest_magic[8] = {
    'L', 'F', 'S', 'B', 'O', 'O', 'T', '!'
};

static void mem_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    for (u32 i = 0; i < n; ++i)
        d[i] = s[i];
}

static void mem_zero(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;

    for (u32 i = 0; i < n; ++i)
        d[i] = 0;
}

static int mem_cmp(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    for (u32 i = 0; i < n; ++i) {
        if (x[i] != y[i])
            return 1;
    }
    return 0;
}

static int name_len_ok(const char *s)
{
    u32 n = 0;
    while (s[n] != '\0') {
        if (n + 1 >= LFS_NAME_MAX)
            return 0;
        ++n;
    }
    return n > 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void name_copy(char *dst, const char *src)
{
    u32 i = 0;

    while (src[i] != '\0' && i + 1 < LFS_NAME_MAX) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static u32 inode_table_block(u32 ino)
{
    return LFS_INODE_TABLE_START + (ino - 1) / LFS_INODES_PER_BLOCK;
}

static u32 inode_table_offset(u32 ino)
{
    return ((ino - 1) % LFS_INODES_PER_BLOCK) * LFS_INODE_SIZE;
}

/* ---------------------------- block I/O ---------------------------------- */

static int lfs_raw_read(u32 block, void *buf)
{
    if (block >= lfs_sb.total_blocks)
        return -1;
    return virtio_blk_read((u64)block * LFS_SECTORS_PER_BLOCK, buf,
                           LFS_SECTORS_PER_BLOCK);
}

static int lfs_raw_write(u32 block, const void *buf)
{
    if (block >= lfs_sb.total_blocks)
        return -1;
    return virtio_blk_write((u64)block * LFS_SECTORS_PER_BLOCK, buf,
                            LFS_SECTORS_PER_BLOCK);
}

static u32 lfs_journal_header_block(void)
{
    return lfs_sb.total_blocks - 1u - LFS_JOURNAL_BLOCKS;
}

static u32 lfs_backup_block(void)
{
    /* Backup sits just before the journal; the final block is reserved for
     * the virtio-blk self-test (last sector). */
    return lfs_sb.total_blocks - 2u - LFS_JOURNAL_BLOCKS;
}

static int lfs_journal_write(u32 block, const u8 *data)
{
    struct lfs_journal_header h;
    u32 hb;

    if (!lfs_journal_enabled)
        return lfs_raw_write(block, data);

    hb = lfs_journal_header_block();
    if (block >= hb || block >= lfs_sb.total_blocks)
        return lfs_raw_write(block, data);

    mem_zero(&h, sizeof(h));
    h.magic = LFS_JOURNAL_MAGIC;
    h.count = 1;
    h.target = block;
    h.slot = 1;

    /* 1. data block into the journal */
    if (lfs_raw_write(hb + h.slot, data) != 0)
        return -1;

    /* 2. header without commit */
    if (lfs_raw_write(hb, &h) != 0)
        return -1;

    /* 3. commit */
    h.commit = LFS_JOURNAL_COMMIT;
    if (lfs_raw_write(hb, &h) != 0)
        return -1;

    /* 4. checkpoint to the home block */
    if (lfs_raw_write(block, data) != 0)
        return -1;

    /* 5. clear the journal */
    (void)lfs_raw_write(hb, lfs_zero);
    return 0;
}

static void lfs_journal_replay(void)
{
    struct lfs_journal_header h;
    u32 hb = lfs_journal_header_block();

    if (lfs_raw_read(hb, &h) != 0)
        return;
    if (h.magic != LFS_JOURNAL_MAGIC || h.commit != LFS_JOURNAL_COMMIT ||
        h.count != 1 || h.slot == 0)
        return;

    if (h.target >= lfs_sb.total_blocks || h.target >= hb ||
        hb + h.slot >= lfs_sb.total_blocks)
        return;

    {
        static u8 slot_data[LFS_BLOCK_SIZE];

        if (lfs_raw_read(hb + h.slot, slot_data) != 0)
            return;
        if (lfs_raw_write(h.target, slot_data) != 0)
            return;
    }

    (void)lfs_raw_write(hb, lfs_zero);
    kprintf("[lfs] journal replay: block %lu\n", (u64)h.target);
}

static void lfs_cache_invalidate(void)
{
    for (u32 i = 0; i < LFS_CACHE_ENTRIES; ++i)
        lfs_cache[i].valid = 0;
    lfs_cache_age = 0;
}

static void lfs_cache_drop(u32 block)
{
    for (u32 i = 0; i < LFS_CACHE_ENTRIES; ++i) {
        if (lfs_cache[i].valid && lfs_cache[i].block == block)
            lfs_cache[i].valid = 0;
    }
}

static u8 *lfs_cache_get(u32 block)
{
    struct lfs_cache_entry *victim = NULL;

    for (u32 i = 0; i < LFS_CACHE_ENTRIES; ++i) {
        struct lfs_cache_entry *e = &lfs_cache[i];

        if (e->valid && e->block == block) {
            e->age = ++lfs_cache_age;
            return e->data;
        }
        if (!e->valid && victim == NULL)
            victim = e;
        if (e->valid &&
            (victim == NULL || (victim->valid && e->age < victim->age)))
            victim = e;
    }

    if (victim == NULL)
        return NULL;

    if (lfs_raw_read(block, victim->data) != 0) {
        victim->valid = 0;
        return NULL;
    }

    victim->block = block;
    victim->valid = 1;
    victim->age = ++lfs_cache_age;
    return victim->data;
}

static int lfs_cache_write(u32 block, const u8 *data)
{
    if (lfs_journal_write(block, data) != 0)
        return -1;

    for (u32 i = 0; i < LFS_CACHE_ENTRIES; ++i) {
        struct lfs_cache_entry *e = &lfs_cache[i];

        if (e->valid && e->block == block) {
            mem_copy(e->data, data, LFS_BLOCK_SIZE);
            e->age = ++lfs_cache_age;
            return 0;
        }
    }

    return 0;
}

/* ----------------------------- inodes ------------------------------------ */

static int lfs_read_inode(u32 ino, struct lfs_inode_disk *out)
{
    u32 block;
    u32 off;
    u8 *data;

    if (ino < 1 || ino > lfs_sb.inode_count)
        return -1;

    block = inode_table_block(ino);
    off = inode_table_offset(ino);
    data = lfs_cache_get(block);
    if (data == NULL)
        return -1;

    mem_copy(out, data + off, LFS_INODE_SIZE);
    return 0;
}

static int lfs_write_inode(u32 ino, const struct lfs_inode_disk *in)
{
    u32 block;
    u32 off;
    u8 *data;

    if (ino < 1 || ino > lfs_sb.inode_count)
        return -1;

    block = inode_table_block(ino);
    off = inode_table_offset(ino);
    data = lfs_cache_get(block);
    if (data == NULL)
        return -1;

    mem_copy(data + off, in, LFS_INODE_SIZE);
    return lfs_cache_write(block, data);
}

static u32 lfs_alloc_inode(void)
{
    u8 *bm = lfs_cache_get(lfs_sb.inode_bitmap_block);

    if (bm == NULL)
        return 0;

    for (u32 ino = 1; ino <= lfs_sb.inode_count; ++ino) {
        u32 bit = ino - 1;
        u8 mask = (u8)(1u << (bit % 8));

        if ((bm[bit / 8] & mask) == 0) {
            struct lfs_inode_disk empty;

            bm[bit / 8] |= mask;
            if (lfs_cache_write(lfs_sb.inode_bitmap_block, bm) != 0)
                return 0;

            mem_zero(&empty, sizeof(empty));
            if (lfs_write_inode(ino, &empty) != 0)
                return 0;
            return ino;
        }
    }

    return 0;
}

static void lfs_free_inode_bit(u32 ino)
{
    u8 *bm = lfs_cache_get(lfs_sb.inode_bitmap_block);
    u32 bit;
    u8 mask;

    if (bm == NULL || ino < 1 || ino > lfs_sb.inode_count)
        return;

    bit = ino - 1;
    mask = (u8)(1u << (bit % 8));
    bm[bit / 8] &= (u8)~mask;
    lfs_cache_write(lfs_sb.inode_bitmap_block, bm);
}

static u32 lfs_alloc_block(void)
{
    u8 *bm = lfs_cache_get(lfs_sb.block_bitmap_block);

    if (bm == NULL)
        return 0;

    for (u32 b = lfs_sb.data_start; b < lfs_sb.total_blocks; ++b) {
        u8 mask = (u8)(1u << (b % 8));

        if ((bm[b / 8] & mask) == 0) {
            bm[b / 8] |= mask;
            if (lfs_cache_write(lfs_sb.block_bitmap_block, bm) != 0)
                return 0;
            if (lfs_raw_write(b, lfs_zero) != 0)
                return 0;
            lfs_cache_drop(b);
            return b;
        }
    }

    return 0;
}

static void lfs_free_block(u32 block)
{
    u8 *bm = lfs_cache_get(lfs_sb.block_bitmap_block);

    if (bm == NULL || block < lfs_sb.data_start ||
        block >= lfs_sb.total_blocks)
        return;

    bm[block / 8] &= (u8)~(1u << (block % 8));
    lfs_cache_write(lfs_sb.block_bitmap_block, bm);
    lfs_cache_drop(block);
}

static u32 lfs_bmap(struct lfs_inode_disk *di, u32 index, int create)
{
    if (index < LFS_DIRECT_BLOCKS) {
        if (di->direct[index] == 0 && create) {
            u32 b = lfs_alloc_block();

            if (b == 0)
                return 0;
            di->direct[index] = b;
        }
        return di->direct[index];
    }

    if (index >= LFS_MAX_BLOCKS)
        return 0;

    index -= LFS_DIRECT_BLOCKS;

    if (di->indirect == 0) {
        u32 b;

        if (!create)
            return 0;
        b = lfs_alloc_block();
        if (b == 0)
            return 0;
        di->indirect = b;
    }

    {
        u32 *ind = (u32 *)lfs_cache_get(di->indirect);
        u32 b;

        if (ind == NULL)
            return 0;

        b = ind[index];
        if (b == 0 && create) {
            b = lfs_alloc_block();
            if (b == 0)
                return 0;

            /* lfs_alloc_block touches the bitmap cache and may evict the
             * indirect block; re-fetch it before writing the new pointer. */
            ind = (u32 *)lfs_cache_get(di->indirect);
            if (ind == NULL)
                return 0;
            ind[index] = b;
            if (lfs_cache_write(di->indirect, (const u8 *)ind) != 0)
                return 0;
        }

        return b;
    }
}

static void lfs_free_inode_data(const struct lfs_inode_disk *in)
{
    for (u32 i = 0; i < LFS_DIRECT_BLOCKS; ++i) {
        if (in->direct[i] != 0)
            lfs_free_block(in->direct[i]);
    }

    if (in->indirect != 0) {
        u32 *ind = (u32 *)lfs_cache_get(in->indirect);

        if (ind != NULL) {
            for (u32 i = 0; i < LFS_INDIRECT_ENTRIES; ++i) {
                if (ind[i] != 0)
                    lfs_free_block(ind[i]);
            }
        }
        lfs_free_block(in->indirect);
    }
}

/* --------------------------- directories -------------------------------- */

static int lfs_dir_get(u32 dir_ino, u32 index, struct lfs_dirent *de)
{
    struct lfs_inode_disk di;
    u64 off;
    u32 block;
    u8 *data;

    if (lfs_read_inode(dir_ino, &di) != 0)
        return -1;

    off = (u64)index * LFS_DIRENT_SIZE;
    if (off + LFS_DIRENT_SIZE > di.size)
        return 0;

    block = di.direct[off / LFS_BLOCK_SIZE];
    if (block == 0)
        return 0;

    data = lfs_cache_get(block);
    if (data == NULL)
        return -1;

    mem_copy(de, data + (off % LFS_BLOCK_SIZE), LFS_DIRENT_SIZE);
    return 1;
}

static int lfs_dir_put(u32 dir_ino, u32 index, const struct lfs_dirent *de)
{
    struct lfs_inode_disk di;
    u64 off;
    u32 block;
    u8 *data;

    if (lfs_read_inode(dir_ino, &di) != 0)
        return -1;

    off = (u64)index * LFS_DIRENT_SIZE;
    if (off + LFS_DIRENT_SIZE > di.size)
        return -1;

    block = di.direct[off / LFS_BLOCK_SIZE];
    if (block == 0)
        return -1;

    data = lfs_cache_get(block);
    if (data == NULL)
        return -1;

    mem_copy(data + (off % LFS_BLOCK_SIZE), de, LFS_DIRENT_SIZE);
    return lfs_cache_write(block, data);
}

static int lfs_dir_lookup(u32 dir_ino, const char *name, u32 *out_ino)
{
    struct lfs_inode_disk di;
    u32 count;

    if (lfs_read_inode(dir_ino, &di) != 0)
        return -1;

    count = di.size / LFS_DIRENT_SIZE;
    for (u32 i = 0; i < count; ++i) {
        struct lfs_dirent de;

        if (lfs_dir_get(dir_ino, i, &de) != 1)
            return -1;
        if (str_eq(de.name, name)) {
            if (out_ino != NULL)
                *out_ino = de.ino;
            return 1;
        }
    }

    return 0;
}

static int lfs_dir_add(u32 dir_ino, const char *name, u32 child_ino)
{
    struct lfs_inode_disk di;
    struct lfs_dirent de;
    u64 off;
    u32 index;
    u32 block_index;
    u32 block;
    u8 *data;

    if (name[0] == '\0' || lfs_read_inode(dir_ino, &di) != 0)
        return -1;
    if (di.size + LFS_DIRENT_SIZE > LFS_DIRECT_BLOCKS * LFS_BLOCK_SIZE)
        return -ENOSPC;

    index = di.size / LFS_DIRENT_SIZE;
    off = (u64)index * LFS_DIRENT_SIZE;
    block_index = (u32)(off / LFS_BLOCK_SIZE);

    if (di.direct[block_index] == 0) {
        block = lfs_alloc_block();
        if (block == 0)
            return -ENOSPC;
        di.direct[block_index] = block;
    }

    block = di.direct[block_index];
    data = lfs_cache_get(block);
    if (data == NULL)
        return -1;

    mem_zero(&de, sizeof(de));
    de.ino = child_ino;
    name_copy(de.name, name);
    mem_copy(data + (off % LFS_BLOCK_SIZE), &de, LFS_DIRENT_SIZE);
    if (lfs_cache_write(block, data) != 0)
        return -1;

    di.size += LFS_DIRENT_SIZE;
    return lfs_write_inode(dir_ino, &di);
}

static int lfs_dir_remove(u32 dir_ino, const char *name)
{
    struct lfs_inode_disk di;
    u32 count;
    u32 found = 0;
    struct lfs_dirent de;

    if (lfs_read_inode(dir_ino, &di) != 0)
        return -1;

    count = di.size / LFS_DIRENT_SIZE;
    for (u32 i = 0; i < count; ++i) {
        if (lfs_dir_get(dir_ino, i, &de) != 1)
            return -1;
        if (str_eq(de.name, name)) {
            found = i;
            break;
        }
    }
    if (count == 0)
        return -ENOENT;

    /* Verify the requested name was actually found. */
    if (lfs_dir_get(dir_ino, found, &de) != 1 || !str_eq(de.name, name))
        return -ENOENT;

    for (u32 i = found; i + 1 < count; ++i) {
        if (lfs_dir_get(dir_ino, i + 1, &de) != 1)
            return -1;
        if (lfs_dir_put(dir_ino, i, &de) != 0)
            return -1;
    }

    di.size -= LFS_DIRENT_SIZE;
    return lfs_write_inode(dir_ino, &di);
}

/* -------------------------- in-memory tree ------------------------------- */

static void attach_child(struct dentry *parent, struct dentry *child)
{
    if (parent == NULL)
        return;

    if (parent->d_first_child == NULL) {
        parent->d_first_child = child;
    } else {
        struct dentry *t = parent->d_first_child;
        while (t->d_next_sibling != NULL)
            t = t->d_next_sibling;
        t->d_next_sibling = child;
    }
}

static struct inode *vfs_inode_new(u32 ino, const struct lfs_inode_disk *di)
{
    struct inode *inode = (struct inode *)kmalloc(sizeof(struct inode));

    if (inode == NULL)
        return NULL;

    mem_zero(inode, sizeof(*inode));
    inode->i_ino = ino;
    inode->i_mode = di->mode;
    inode->i_size = di->size;
    inode->i_nlink = di->nlink;
    inode->i_open_count = 0;
    inode->i_data = (void *)(usize)ino;
    inode->i_fop = &lfs_file_ops;
    inode->i_dentry = NULL;
    return inode;
}

static struct dentry *lfs_build_dentry(u32 ino, const char *name,
                                       struct dentry *parent)
{
    struct lfs_inode_disk di;
    struct inode *inode;
    struct dentry *dentry;

    if (lfs_read_inode(ino, &di) != 0)
        return NULL;

    inode = vfs_inode_new(ino, &di);
    if (inode == NULL)
        return NULL;

    dentry = (struct dentry *)kmalloc(sizeof(struct dentry));
    if (dentry == NULL) {
        kfree(inode);
        return NULL;
    }
    mem_zero(dentry, sizeof(*dentry));
    name_copy(dentry->d_name, name);
    dentry->d_inode = inode;
    dentry->d_parent = parent;
    inode->i_dentry = dentry;
    attach_child(parent, dentry);

    if ((di.mode & S_IFDIR) != 0) {
        u32 count = di.size / LFS_DIRENT_SIZE;

        for (u32 i = 0; i < count; ++i) {
            struct lfs_dirent de;

            if (lfs_dir_get(ino, i, &de) != 1)
                continue;
            if (de.ino == 0 || de.name[0] == '\0')
                continue;
            if (str_eq(de.name, ".") || str_eq(de.name, ".."))
                continue;
            lfs_build_dentry(de.ino, de.name, dentry);
        }
    }

    return dentry;
}

/* ----------------------------- file ops ---------------------------------- */

static i64 lfs_dir_read(struct file *file, void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    u32 dir_ino;
    u32 index;
    struct lfs_dirent lde;
    struct lfs_inode_disk child;
    struct dirent de;

    if (inode == NULL)
        return -EINVAL;
    if (count < sizeof(struct dirent))
        return -EINVAL;

    dir_ino = (u32)(usize)inode->i_data;
    index = (u32)file->f_pos;
    if (lfs_dir_get(dir_ino, index, &lde) != 1)
        return 0;   /* EOF */

    mem_zero(&de, sizeof(de));
    de.d_ino = lde.ino;
    if (lfs_read_inode(lde.ino, &child) == 0 &&
        (child.mode & S_IFDIR) != 0)
        de.d_type = DT_DIR;
    else
        de.d_type = DT_REG;
    name_copy(de.d_name, lde.name);
    mem_copy(buf, &de, sizeof(de));
    file->f_pos++;
    return (i64)sizeof(de);
}

static i64 lfs_read(struct file *file, void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    struct lfs_inode_disk di;
    u32 ino;
    u64 pos;
    u64 size;
    u64 done = 0;
    u8 *dst = (u8 *)buf;

    if (inode == NULL)
        return -EINVAL;

    ino = (u32)(usize)inode->i_data;
    if (lfs_read_inode(ino, &di) != 0)
        return -EIO;

    if ((di.mode & S_IFDIR) != 0)
        return lfs_dir_read(file, buf, count);

    pos = file->f_pos;
    size = di.size;
    if (pos >= size)
        return 0;
    if (count > size - pos)
        count = size - pos;

    while (done < count) {
        u64 p = pos + done;
        u32 bi = (u32)(p / LFS_BLOCK_SIZE);
        u32 off = (u32)(p % LFS_BLOCK_SIZE);
        u64 n = LFS_BLOCK_SIZE - off;
        u32 block = lfs_bmap(&di, bi, 0);
        u8 *data;

        if (block == 0)
            break;
        if (n > count - done)
            n = count - done;
        data = lfs_cache_get(block);
        if (data == NULL)
            break;
        mem_copy(dst + done, data + off, (u32)n);
        done += n;
    }

    file->f_pos = pos + done;
    return (i64)done;
}

static i64 lfs_write(struct file *file, const void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    struct lfs_inode_disk di;
    u32 ino;
    u64 pos;
    u64 end;
    u64 done = 0;
    const u8 *src = (const u8 *)buf;

    if (inode == NULL)
        return -EINVAL;

    ino = (u32)(usize)inode->i_data;
    if (lfs_read_inode(ino, &di) != 0)
        return -EIO;
    if ((di.mode & S_IFDIR) != 0)
        return -EISDIR;

    pos = file->f_pos;
    end = pos + count;
    if (end < pos || end > (u64)LFS_MAX_BLOCKS * LFS_BLOCK_SIZE)
        return -ENOSPC;

    while (done < count) {
        u64 p = pos + done;
        u32 bi = (u32)(p / LFS_BLOCK_SIZE);
        u32 off = (u32)(p % LFS_BLOCK_SIZE);
        u64 n = LFS_BLOCK_SIZE - off;
        u32 block = lfs_bmap(&di, bi, 1);
        u8 *data;

        if (n > count - done)
            n = count - done;

        if (block == 0)
            return -ENOSPC;

        data = lfs_cache_get(block);
        if (data == NULL)
            return -EIO;
        mem_copy(data + off, src + done, (u32)n);
        if (lfs_cache_write(block, data) != 0)
            return -EIO;

        done += n;
    }

    if (end > di.size)
        di.size = (u32)end;
    if (lfs_write_inode(ino, &di) != 0)
        return -EIO;

    inode->i_size = di.size;
    file->f_pos = end;
    return (i64)count;
}

static void lfs_release(struct file *file)
{
    struct inode *inode = (struct inode *)file->private_data;
    struct lfs_inode_disk di;
    u32 ino;

    if (inode == NULL)
        return;

    ino = (u32)(usize)inode->i_data;
    if (inode->i_open_count > 0)
        inode->i_open_count--;

    if (inode->i_open_count > 0)
        return;

    if (lfs_read_inode(ino, &di) == 0 && di.nlink == 0) {
        lfs_free_inode_data(&di);
        lfs_free_inode_bit(ino);
        kfree(inode);
    }
}

const struct file_operations lfs_file_ops = {
    .read = lfs_read,
    .write = lfs_write,
    .ioctl = NULL,
    .release = lfs_release,
};

/* --------------------------- VFS backend -------------------------------- */

static struct dentry *lfs_new_object(struct dentry *parent, const char *name,
                                     u32 mode)
{
    struct lfs_inode_disk di;
    struct inode *parent_inode;
    struct inode *inode;
    struct dentry *dentry;
    u32 parent_ino;
    u32 ino;

    if (parent == NULL || parent->d_inode == NULL || name == NULL ||
        name[0] == '\0')
        return NULL;
    if (!name_len_ok(name))
        return NULL;

    parent_inode = parent->d_inode;
    parent_ino = (u32)(usize)parent_inode->i_data;

    {
        u32 existing_ino;
        if (lfs_dir_lookup(parent_ino, name, &existing_ino) > 0)
            return NULL;
    }

    ino = lfs_alloc_inode();
    if (ino == 0)
        return NULL;

    mem_zero(&di, sizeof(di));
    di.mode = mode;
    di.nlink = 1;
    di.size = 0;
    if (lfs_write_inode(ino, &di) != 0) {
        lfs_free_inode_bit(ino);
        return NULL;
    }

    if (lfs_dir_add(parent_ino, name, ino) != 0) {
        lfs_free_inode_bit(ino);
        return NULL;
    }

    inode = vfs_inode_new(ino, &di);
    if (inode == NULL)
        return NULL;

    dentry = (struct dentry *)kmalloc(sizeof(struct dentry));
    if (dentry == NULL) {
        kfree(inode);
        return NULL;
    }
    mem_zero(dentry, sizeof(*dentry));
    name_copy(dentry->d_name, name);
    dentry->d_inode = inode;
    dentry->d_parent = parent;
    inode->i_dentry = dentry;
    attach_child(parent, dentry);

    parent_inode->i_size += LFS_DIRENT_SIZE;
    return dentry;
}

static struct dentry *lfs_create_file(struct dentry *parent, const char *name)
{
    return lfs_new_object(parent, name, VFS_MODE_FILE);
}

static struct dentry *lfs_mkdir(struct dentry *parent, const char *name)
{
    return lfs_new_object(parent, name, VFS_MODE_DIR);
}

static void lfs_unlink_dentry(struct dentry *dentry)
{
    struct dentry *parent;
    struct inode *inode;
    struct lfs_inode_disk di;
    u32 parent_ino;
    u32 ino;

    if (dentry == NULL || dentry->d_inode == NULL)
        return;

    parent = dentry->d_parent;
    inode = dentry->d_inode;
    if (parent == NULL || parent->d_inode == NULL)
        return;

    parent_ino = (u32)(usize)parent->d_inode->i_data;
    lfs_dir_remove(parent_ino, dentry->d_name);
    if (parent->d_inode->i_size >= LFS_DIRENT_SIZE)
        parent->d_inode->i_size -= LFS_DIRENT_SIZE;

    ino = (u32)(usize)inode->i_data;
    if (lfs_read_inode(ino, &di) != 0)
        return;

    if (di.nlink > 0)
        di.nlink--;
    lfs_write_inode(ino, &di);
    inode->i_nlink = di.nlink;

    if (di.nlink == 0 && inode->i_open_count == 0) {
        lfs_free_inode_data(&di);
        lfs_free_inode_bit(ino);
        kfree(inode);
    }
}

static void lfs_truncate(struct inode *inode)
{
    struct lfs_inode_disk di;
    u32 ino;

    if (inode == NULL)
        return;

    ino = (u32)(usize)inode->i_data;
    if (lfs_read_inode(ino, &di) != 0)
        return;
    if ((di.mode & S_IFDIR) != 0)
        return;

    lfs_free_inode_data(&di);
    mem_zero(di.direct, sizeof(di.direct));
    di.indirect = 0;
    di.size = 0;
    lfs_write_inode(ino, &di);
    inode->i_size = 0;
}

const struct vfs_fs_ops lfs_vfs_ops = {
    .create_file = lfs_create_file,
    .mkdir = lfs_mkdir,
    .unlink_dentry = lfs_unlink_dentry,
    .truncate = lfs_truncate,
};

/* ----------------------------- format/mount ------------------------------ */

static int lfs_format(u32 total_blocks)
{
    struct lfs_superblock sb;
    struct lfs_inode_disk root;
    u8 *bitmap;

    mem_zero(&sb, sizeof(sb));
    mem_copy(sb.magic, LFS_MAGIC, 8);
    sb.version = LFS_VERSION;
    sb.block_size = LFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.data_start = LFS_DATA_START;
    sb.inode_count = LFS_INODE_COUNT;
    sb.inode_table_start = LFS_INODE_TABLE_START;
    sb.inode_table_blocks = LFS_INODE_TABLE_BLOCKS;
    sb.inode_bitmap_block = LFS_INODE_BITMAP_BLOCK;
    sb.block_bitmap_block = LFS_BLOCK_BITMAP_BLOCK;
    sb.root_ino = 1;

    lfs_sb = sb;
    lfs_journal_enabled = 0;
    lfs_cache_invalidate();
    if (lfs_raw_write(0, &sb) != 0)
        return -1;
    if (lfs_raw_write(lfs_backup_block(), &sb) != 0)
        return -1;
    if (lfs_raw_write(lfs_journal_header_block(), lfs_zero) != 0)
        return -1;

    /* Block bitmap: reserve superblock/bitmaps/inode table + journal/backup. */
    bitmap = lfs_cache_get(LFS_BLOCK_BITMAP_BLOCK);
    if (bitmap == NULL)
        return -1;
    mem_zero(bitmap, LFS_BLOCK_SIZE);
    for (u32 b = 0; b < LFS_DATA_START; ++b)
        bitmap[b / 8] |= (u8)(1u << (b % 8));
    for (u32 b = lfs_backup_block(); b < lfs_sb.total_blocks; ++b)
        bitmap[b / 8] |= (u8)(1u << (b % 8));
    if (lfs_cache_write(LFS_BLOCK_BITMAP_BLOCK, bitmap) != 0)
        return -1;

    /* Inode bitmap: inode 1 (root) is used. */
    bitmap = lfs_cache_get(LFS_INODE_BITMAP_BLOCK);
    if (bitmap == NULL)
        return -1;
    mem_zero(bitmap, LFS_BLOCK_SIZE);
    bitmap[0] |= 1u;   /* inode 1 -> bit 0 */
    if (lfs_cache_write(LFS_INODE_BITMAP_BLOCK, bitmap) != 0)
        return -1;

    /* Zero the whole inode table, then write the root inode. */
    for (u32 i = 0; i < LFS_INODE_TABLE_BLOCKS; ++i) {
        if (lfs_raw_write(LFS_INODE_TABLE_START + i, lfs_zero) != 0)
            return -1;
    }

    mem_zero(&root, sizeof(root));
    root.mode = VFS_MODE_DIR;
    root.nlink = 1;
    root.size = 0;
    if (lfs_write_inode(1, &root) != 0)
        return -1;

    kprintf("[lfs] formatted: %lu blocks, data_start=%u, inodes=%u\n",
            (u64)total_blocks, LFS_DATA_START, LFS_INODE_COUNT);
    return 0;
}

struct dentry *lfs_mount(void)
{
    struct lfs_superblock sb;
    struct lfs_inode_disk root;
    u32 total_blocks;

    if (!virtio_blk_present())
        return NULL;

    total_blocks = (u32)(virtio_blk_capacity() / LFS_SECTORS_PER_BLOCK);
    if (total_blocks < LFS_DATA_START + 16) {
        kprintf("[lfs] disk too small: %lu blocks\n", (u64)total_blocks);
        return NULL;
    }

    mem_zero(&lfs_sb, sizeof(lfs_sb));
    lfs_sb.total_blocks = total_blocks;   /* allow raw_read(0) before sb is valid */
    lfs_cache_invalidate();
    if (lfs_raw_read(0, &sb) != 0) {
        kprintf("[lfs] superblock read failed\n");
        return NULL;
    }

    if (mem_cmp(sb.magic, LFS_MAGIC, 8) != 0 ||
        sb.version != LFS_VERSION ||
        sb.block_size != LFS_BLOCK_SIZE ||
        sb.total_blocks != total_blocks ||
        sb.inode_count != LFS_INODE_COUNT ||
        sb.inode_table_start != LFS_INODE_TABLE_START ||
        sb.data_start != LFS_DATA_START) {
        struct lfs_superblock backup;

        lfs_sb.total_blocks = total_blocks;
        if (lfs_raw_read(lfs_backup_block(), &backup) == 0 &&
            mem_cmp(backup.magic, LFS_MAGIC, 8) == 0 &&
            backup.version == LFS_VERSION &&
            backup.block_size == LFS_BLOCK_SIZE &&
            backup.total_blocks == total_blocks &&
            backup.inode_count == LFS_INODE_COUNT &&
            backup.inode_table_start == LFS_INODE_TABLE_START &&
            backup.data_start == LFS_DATA_START) {
            sb = backup;
            (void)lfs_raw_write(0, &sb);
            kprintf("[lfs] primary superblock restored from backup\n");
        } else {
            if (lfs_format(total_blocks) != 0)
                return NULL;
            if (lfs_raw_read(0, &sb) != 0)
                return NULL;
        }
    }

    lfs_sb = sb;
    lfs_ready = 1;
    lfs_cache_invalidate();
    lfs_journal_replay();
    lfs_cache_invalidate();

    if (lfs_read_inode(lfs_sb.root_ino, &root) != 0)
        return NULL;
    if ((root.mode & S_IFDIR) == 0)
        return NULL;

    lfs_root_dentry = lfs_build_dentry(lfs_sb.root_ino, "/", NULL);
    if (lfs_root_dentry == NULL)
        return NULL;

    lfs_is_mounted = 1;
    lfs_journal_enabled = 1;
    kprintf("[lfs] mounted: magic=%s blocks=%lu inodes=%u root=%u journal=%u\n",
            LFS_MAGIC, (u64)lfs_sb.total_blocks, LFS_INODE_COUNT,
            lfs_sb.root_ino, lfs_journal_header_block());
    return lfs_root_dentry;
}

int lfs_mounted(void)
{
    return lfs_is_mounted;
}

int lfs_selftest(void)
{
    static const char path[] = "/lfs-test";
    u8 buf[16];
    u32 count = 0;
    struct file *file;

    if (!lfs_is_mounted)
        return 0;

    file = vfs_open(path, O_RDONLY);
    if (file != NULL) {
        i64 n = file->f_op->read(file, buf, sizeof(buf));

        file_close(file);
        if (n >= 12 && mem_cmp(buf, lfs_selftest_magic, 8) == 0) {
            count = (u32)buf[8] | ((u32)buf[9] << 8) |
                    ((u32)buf[10] << 16) | ((u32)buf[11] << 24);
        }
    }
    count++;

    file = vfs_open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (file == NULL) {
        kprintf("[lfs] self-test: create failed\n");
        return 0;
    }
    mem_zero(buf, sizeof(buf));
    mem_copy(buf, lfs_selftest_magic, 8);
    buf[8] = (u8)(count & 0xFF);
    buf[9] = (u8)((count >> 8) & 0xFF);
    buf[10] = (u8)((count >> 16) & 0xFF);
    buf[11] = (u8)((count >> 24) & 0xFF);
    if (file->f_op->write(file, buf, 12) != 12) {
        file_close(file);
        kprintf("[lfs] self-test: write failed\n");
        return 0;
    }
    file_close(file);

    file = vfs_open(path, O_RDONLY);
    if (file == NULL) {
        kprintf("[lfs] self-test: reopen failed\n");
        return 0;
    }
    mem_zero(buf, sizeof(buf));
    {
        i64 n = file->f_op->read(file, buf, sizeof(buf));

        file_close(file);
        if (n < 12) {
            kprintf("[lfs] self-test: read-back failed\n");
            return 0;
        }
    }

    if (mem_cmp(buf, lfs_selftest_magic, 8) != 0 ||
        ((u32)buf[8] | ((u32)buf[9] << 8) |
         ((u32)buf[10] << 16) | ((u32)buf[11] << 24)) != count) {
        kprintf("[lfs] self-test: verify mismatch\n");
        return 0;
    }

    kprintf("[lfs] self-test: PASS (boot_count=%lu)\n", (u64)count);
    return 1;
}
