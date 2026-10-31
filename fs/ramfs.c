/*
 * Lantest OS - ramfs: in-memory filesystem (M4-2..M4-4)
 *
 * Regular files are a chain of 4 KiB blocks allocated from kmalloc. Inodes
 * and dentries live in kernel memory; the root directory is static storage.
 * M4-4 adds open counts and delete-on-close semantics for unlink().
 */
#include <types.h>
#include <kprintf.h>
#include <panic.h>
#include <kmalloc.h>
#include <errno.h>
#include <vfs.h>
#include <ramfs.h>

#define RAMFS_BLOCK_SIZE 4096u

struct ramfs_block {
    struct ramfs_block *next;
    u8 data[RAMFS_BLOCK_SIZE];
};

static struct superblock ramfs_sb;
static struct inode root_inode;
static struct dentry root_dentry;

static void *kmalloc_zero(u64 size)
{
    void *p = kmalloc((usize)size);

    if (p == NULL)
        return NULL;

    u8 *q = (u8 *)p;
    for (u64 i = 0; i < size; ++i)
        q[i] = 0;
    return p;
}

static void name_copy(char *dst, const char *src)
{
    u32 i = 0;

    while (src[i] != '\0' && i + 1 < VFS_NAME_MAX) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static struct ramfs_block *block_alloc(void)
{
    return (struct ramfs_block *)kmalloc_zero(sizeof(struct ramfs_block));
}

static struct ramfs_block *block_at(struct inode *inode, u64 index, int create)
{
    struct ramfs_block *b = (struct ramfs_block *)inode->i_data;
    struct ramfs_block *prev = NULL;

    for (u64 i = 0; i < index; ++i) {
        if (b == NULL) {
            if (!create)
                return NULL;
            b = block_alloc();
            if (b == NULL)
                return NULL;
            if (prev != NULL)
                prev->next = b;
            else
                inode->i_data = b;
        }
        prev = b;
        b = b->next;
    }

    if (b == NULL && create) {
        b = block_alloc();
        if (b == NULL)
            return NULL;
        if (prev != NULL)
            prev->next = b;
        else
            inode->i_data = b;
    }

    return b;
}

static void zero_range(struct inode *inode, u64 start, u64 end)
{
    while (start < end) {
        struct ramfs_block *b = block_at(inode, start / RAMFS_BLOCK_SIZE, 1);
        u64 off;
        u64 n;
        u8 *p;

        if (b == NULL)
            return;

        off = start % RAMFS_BLOCK_SIZE;
        n = RAMFS_BLOCK_SIZE - off;
        if (n > end - start)
            n = end - start;

        p = &b->data[off];
        for (u64 i = 0; i < n; ++i)
            p[i] = 0;
        start += n;
    }
}

static void free_inode_data(struct inode *inode)
{
    struct ramfs_block *b = (struct ramfs_block *)inode->i_data;

    while (b != NULL) {
        struct ramfs_block *next = b->next;
        kfree(b);
        b = next;
    }
    inode->i_data = NULL;
    inode->i_size = 0;
}

void ramfs_truncate(struct inode *inode)
{
    if (inode == NULL || (inode->i_mode & S_IFDIR) != 0)
        return;
    free_inode_data(inode);
}

static void free_inode(struct inode *inode)
{
    if (inode == NULL)
        return;
    if ((inode->i_mode & S_IFDIR) == 0)
        free_inode_data(inode);
    kfree(inode);
}

static void ramfs_release(struct file *file)
{
    struct inode *inode = (struct inode *)file->private_data;

    if (inode == NULL)
        return;

    if (inode->i_open_count > 0)
        inode->i_open_count--;

    if (inode->i_nlink == 0 && inode->i_open_count == 0)
        free_inode(inode);
}

void ramfs_unlink_inode(struct inode *inode)
{
    if (inode == NULL)
        return;

    if (inode->i_nlink > 0)
        inode->i_nlink--;

    if (inode->i_nlink == 0 && inode->i_open_count == 0)
        free_inode(inode);
}

static i64 ramfs_dir_read(struct file *file, void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    struct dentry *dir;
    struct dentry *child;
    u64 index = 0;
    struct dirent de;

    if (inode == NULL)
        return -EINVAL;
    if (count < sizeof(struct dirent))
        return -EINVAL;

    dir = inode->i_dentry;
    if (dir == NULL)
        return -EINVAL;

    child = dir->d_first_child;
    while (child != NULL && index < file->f_pos) {
        child = child->d_next_sibling;
        ++index;
    }
    if (child == NULL)
        return 0;

    for (u32 i = 0; i < sizeof(de); ++i)
        ((u8 *)&de)[i] = 0;

    de.d_ino = child->d_inode != NULL ? child->d_inode->i_ino : 0;
    if (child->d_inode != NULL && (child->d_inode->i_mode & S_IFDIR) != 0)
        de.d_type = DT_DIR;
    else
        de.d_type = DT_REG;
    de.d_pad = 0;
    name_copy(de.d_name, child->d_name);

    for (u32 i = 0; i < sizeof(de); ++i)
        ((u8 *)buf)[i] = ((u8 *)&de)[i];

    file->f_pos++;
    return (i64)sizeof(de);
}

static i64 ramfs_write(struct file *file, const void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    const u8 *src = (const u8 *)buf;
    u64 pos;
    u64 end;
    u64 done = 0;

    if (inode == NULL)
        return -EINVAL;
    if ((inode->i_mode & S_IFDIR) != 0)
        return -EISDIR;

    pos = file->f_pos;
    end = pos + count;
    if (end < pos)
        return -EINVAL;

    if (pos > inode->i_size)
        zero_range(inode, inode->i_size, pos);

    while (done < count) {
        u64 p = pos + done;
        struct ramfs_block *b = block_at(inode, p / RAMFS_BLOCK_SIZE, 1);
        u64 off;
        u64 n;

        if (b == NULL)
            return -ENOMEM;

        off = p % RAMFS_BLOCK_SIZE;
        n = RAMFS_BLOCK_SIZE - off;
        if (n > count - done)
            n = count - done;

        for (u64 i = 0; i < n; ++i)
            b->data[off + i] = src[done + i];

        done += n;
    }

    if (end > inode->i_size)
        inode->i_size = end;
    file->f_pos = end;
    return (i64)count;
}

static i64 ramfs_read(struct file *file, void *buf, u64 count)
{
    struct inode *inode = (struct inode *)file->private_data;
    u8 *dst = (u8 *)buf;
    u64 pos;
    u64 size;
    u64 done = 0;

    if (inode == NULL)
        return -EINVAL;
    if ((inode->i_mode & S_IFDIR) != 0)
        return ramfs_dir_read(file, buf, count);

    pos = file->f_pos;
    size = inode->i_size;
    if (pos >= size)
        return 0;

    if (count > size - pos)
        count = size - pos;

    while (done < count) {
        u64 p = pos + done;
        struct ramfs_block *b = block_at(inode, p / RAMFS_BLOCK_SIZE, 0);
        u64 off;
        u64 n;

        if (b == NULL)
            break;

        off = p % RAMFS_BLOCK_SIZE;
        n = RAMFS_BLOCK_SIZE - off;
        if (n > count - done)
            n = count - done;

        for (u64 i = 0; i < n; ++i)
            dst[done + i] = b->data[off + i];

        done += n;
    }

    file->f_pos = pos + done;
    return (i64)done;
}

const struct file_operations ramfs_file_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .ioctl = NULL,
    .release = ramfs_release,
};

static struct inode *inode_alloc(u32 mode)
{
    struct inode *inode = (struct inode *)kmalloc_zero(sizeof(struct inode));

    if (inode == NULL)
        return NULL;

    inode->i_ino = ++ramfs_sb.s_inode_count;
    inode->i_mode = mode;
    inode->i_size = 0;
    inode->i_nlink = 1;
    inode->i_open_count = 0;
    inode->i_data = NULL;
    inode->i_sb = &ramfs_sb;
    inode->i_fop = &ramfs_file_ops;
    inode->i_dentry = NULL;
    return inode;
}

static void attach_child(struct dentry *parent, struct dentry *child)
{
    if (parent->d_first_child == NULL) {
        parent->d_first_child = child;
    } else {
        struct dentry *t = parent->d_first_child;
        while (t->d_next_sibling != NULL)
            t = t->d_next_sibling;
        t->d_next_sibling = child;
    }
}

static struct dentry *dentry_alloc(struct dentry *parent, const char *name,
                                   struct inode *inode)
{
    struct dentry *d = (struct dentry *)kmalloc_zero(sizeof(struct dentry));

    if (d == NULL)
        return NULL;

    name_copy(d->d_name, name);
    d->d_inode = inode;
    d->d_parent = parent;
    d->d_first_child = NULL;
    d->d_next_sibling = NULL;

    if (parent != NULL)
        attach_child(parent, d);

    if (inode != NULL)
        inode->i_dentry = d;

    return d;
}

void ramfs_init(void)
{
    ramfs_sb.s_root = NULL;
    ramfs_sb.s_inode_count = 0;

    root_inode.i_ino = ++ramfs_sb.s_inode_count;
    root_inode.i_mode = VFS_MODE_DIR;
    root_inode.i_size = 0;
    root_inode.i_nlink = 1;
    root_inode.i_open_count = 0;
    root_inode.i_data = NULL;
    root_inode.i_sb = &ramfs_sb;
    root_inode.i_fop = &ramfs_file_ops;
    root_inode.i_dentry = &root_dentry;

    name_copy(root_dentry.d_name, "/");
    root_dentry.d_inode = &root_inode;
    root_dentry.d_parent = NULL;
    root_dentry.d_first_child = NULL;
    root_dentry.d_next_sibling = NULL;

    ramfs_sb.s_root = &root_dentry;
}

struct dentry *ramfs_root(void)
{
    return ramfs_sb.s_root;
}

struct dentry *ramfs_create_file(struct dentry *parent, const char *name)
{
    struct inode *inode;
    struct dentry *dentry;

    if (parent == NULL || parent->d_inode == NULL || name == NULL ||
        name[0] == '\0')
        return NULL;
    if ((parent->d_inode->i_mode & S_IFDIR) == 0)
        return NULL;

    inode = inode_alloc(VFS_MODE_FILE);
    if (inode == NULL)
        return NULL;

    dentry = dentry_alloc(parent, name, inode);
    if (dentry == NULL) {
        kfree(inode);
        return NULL;
    }

    return dentry;
}

struct dentry *ramfs_mkdir(struct dentry *parent, const char *name)
{
    struct inode *inode;
    struct dentry *dentry;

    if (parent == NULL || parent->d_inode == NULL || name == NULL ||
        name[0] == '\0')
        return NULL;
    if ((parent->d_inode->i_mode & S_IFDIR) == 0)
        return NULL;

    inode = inode_alloc(VFS_MODE_DIR);
    if (inode == NULL)
        return NULL;

    dentry = dentry_alloc(parent, name, inode);
    if (dentry == NULL) {
        kfree(inode);
        return NULL;
    }

    return dentry;
}

static void ramfs_vfs_unlink(struct dentry *dentry)
{
    if (dentry != NULL && dentry->d_inode != NULL)
        ramfs_unlink_inode(dentry->d_inode);
}

const struct vfs_fs_ops ramfs_vfs_ops = {
    .create_file = ramfs_create_file,
    .mkdir = ramfs_mkdir,
    .unlink_dentry = ramfs_vfs_unlink,
    .truncate = ramfs_truncate,
};
