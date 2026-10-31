/*
 * Lantest OS - VFS core: path resolution, open/mkdir/unlink and file helpers
 * (M4-2..M5-2). Filesystem-specific behavior goes through vfs_fs operations.
 */
#include <types.h>
#include <kprintf.h>
#include <kmalloc.h>
#include <errno.h>
#include <vfs.h>
#include <ramfs.h>
#include <lantestfs.h>
#include <virtio_blk.h>
#include <fs.h>
#include <spinlock.h>

static struct superblock vfs_sb;
static spinlock_t vfs_lock = SPINLOCK_INIT;
const struct vfs_fs_ops *vfs_fs;

void vfs_init(void)
{
    struct dentry *root = NULL;

    if (virtio_blk_present())
        root = lfs_mount();

    if (root != NULL) {
        vfs_fs = &lfs_vfs_ops;
        kprintf("[vfs] backend: LantestFS (persistent)\n");
    } else {
        if (virtio_blk_present())
            kprintf("[vfs] LantestFS mount failed; falling back to ramfs\n");
        ramfs_init();
        root = ramfs_root();
        vfs_fs = &ramfs_vfs_ops;
        kprintf("[vfs] backend: ramfs (RAM-only)\n");
    }

    vfs_sb.s_root = root;
    vfs_sb.s_inode_count = 1;

    if (root == NULL)
        return;

    kprintf("[M4] VFS: superblock + root inode/dentry ready (%s)\n",
            vfs_fs == &lfs_vfs_ops ? "lantestfs" : "ramfs");
    kprintf("[M4] VFS: root mount \"/\" inode=%lu\n",
            root->d_inode != NULL ? root->d_inode->i_ino : 0);
}

static struct dentry *lookup_child(struct dentry *dir, const char *name)
{
    struct dentry *child = dir->d_first_child;

    while (child != NULL) {
        const char *a = child->d_name;
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return child;
        child = child->d_next_sibling;
    }

    return NULL;
}

static struct dentry *vfs_lookup_locked(const char *path)
{
    struct dentry *dir;
    const char *p;

    if (path == NULL || path[0] != '/')
        return NULL;

    dir = vfs_sb.s_root;
    if (dir == NULL)
        return NULL;

    p = path;
    while (*p != '\0') {
        char name[VFS_NAME_MAX];
        u32 len = 0;

        while (*p == '/')
            ++p;
        if (*p == '\0')
            break;

        while (*p != '\0' && *p != '/') {
            if (len + 1 >= VFS_NAME_MAX)
                return NULL;
            name[len++] = *p++;
        }
        name[len] = '\0';

        if (dir->d_inode == NULL || (dir->d_inode->i_mode & S_IFDIR) == 0)
            return NULL;

        dir = lookup_child(dir, name);
        if (dir == NULL)
            return NULL;
    }

    return dir;
}

static int split_path(const char *path, char *parent, char *name)
{
    const char *last = NULL;
    const char *p = path;
    u32 len;

    while (*p != '\0') {
        if (*p == '/')
            last = p;
        ++p;
    }

    if (last == NULL)
        return -1;

    len = (u32)(last - path);
    if (len == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        for (u32 i = 0; i < len; ++i)
            parent[i] = path[i];
        parent[len] = '\0';
    }

    if (last[1] == '\0')
        return -1;

    {
        u32 n = 0;
        while (last[1 + n] != '\0') {
            if (n + 1 >= VFS_NAME_MAX)
                return -1;
            name[n] = last[1 + n];
            ++n;
        }
        name[n] = '\0';
    }

    return 0;
}

static struct file *open_inode(struct inode *inode, u32 flags)
{
    struct file *file;

    if (inode == NULL || inode->i_fop == NULL)
        return NULL;

    file = file_alloc(inode->i_fop, flags, inode);
    if (file == NULL)
        return NULL;
    file->f_inode = inode;

    inode->i_open_count++;
    return file;
}

struct dentry *vfs_lookup(const char *path)
{
    struct dentry *d;
    u64 flags;

    spin_lock_irqsave(&vfs_lock, &flags);
    d = vfs_lookup_locked(path);
    spin_unlock_irqrestore(&vfs_lock, flags);
    return d;
}

struct file *vfs_open(const char *path, u32 flags)
{
    struct dentry *dentry;
    struct inode *inode;
    struct file *file;
    u64 lockflags;

    spin_lock_irqsave(&vfs_lock, &lockflags);

    dentry = vfs_lookup_locked(path);
    if (dentry != NULL) {
        inode = dentry->d_inode;
        if (inode == NULL) {
            spin_unlock_irqrestore(&vfs_lock, lockflags);
            return NULL;
        }
        if ((flags & O_TRUNC) != 0 && vfs_fs != NULL &&
            vfs_fs->truncate != NULL)
            vfs_fs->truncate(inode);
        file = open_inode(inode, flags);
        spin_unlock_irqrestore(&vfs_lock, lockflags);
        return file;
    }

    if ((flags & O_CREAT) == 0) {
        spin_unlock_irqrestore(&vfs_lock, lockflags);
        return NULL;
    }

    {
        char parent_path[VFS_PATH_MAX];
        char name[VFS_NAME_MAX];
        struct dentry *parent;

        if (split_path(path, parent_path, name) != 0) {
            spin_unlock_irqrestore(&vfs_lock, lockflags);
            return NULL;
        }

        parent = vfs_lookup_locked(parent_path);
        if (parent == NULL || parent->d_inode == NULL ||
            (parent->d_inode->i_mode & S_IFDIR) == 0 ||
            vfs_fs == NULL || vfs_fs->create_file == NULL) {
            spin_unlock_irqrestore(&vfs_lock, lockflags);
            return NULL;
        }

        dentry = vfs_fs->create_file(parent, name);
        if (dentry == NULL || dentry->d_inode == NULL) {
            spin_unlock_irqrestore(&vfs_lock, lockflags);
            return NULL;
        }
        inode = dentry->d_inode;
    }

    file = open_inode(inode, flags);
    spin_unlock_irqrestore(&vfs_lock, lockflags);
    return file;
}

int vfs_mkdir(const char *path, u32 mode)
{
    struct dentry *existing;
    struct dentry *parent;
    char parent_path[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    u64 flags;

    (void)mode;

    if (path == NULL || path[0] != '/')
        return -EINVAL;

    spin_lock_irqsave(&vfs_lock, &flags);
    existing = vfs_lookup_locked(path);
    if (existing != NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -EEXIST;
    }

    if (split_path(path, parent_path, name) != 0) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -EINVAL;
    }

    parent = vfs_lookup_locked(parent_path);
    if (parent == NULL || parent->d_inode == NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -ENOENT;
    }
    if ((parent->d_inode->i_mode & S_IFDIR) == 0) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -ENOTDIR;
    }

    if (vfs_fs == NULL || vfs_fs->mkdir == NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -EINVAL;
    }

    if (vfs_fs->mkdir(parent, name) == NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -ENOMEM;
    }

    spin_unlock_irqrestore(&vfs_lock, flags);
    return 0;
}

int vfs_unlink(const char *path)
{
    struct dentry *dentry;
    struct dentry *parent;
    struct dentry **pp;
    struct inode *inode;
    u64 flags;

    if (path == NULL || path[0] != '/')
        return -EINVAL;

    spin_lock_irqsave(&vfs_lock, &flags);
    dentry = vfs_lookup_locked(path);
    if (dentry == NULL || dentry->d_inode == NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -ENOENT;
    }

    inode = dentry->d_inode;
    if ((inode->i_mode & S_IFDIR) != 0) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -EISDIR;
    }

    parent = dentry->d_parent;
    if (parent == NULL) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -EINVAL;
    }

    pp = &parent->d_first_child;
    while (*pp != NULL && *pp != dentry)
        pp = &(*pp)->d_next_sibling;
    if (*pp != dentry) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -ENOENT;
    }

    inode->i_dentry = NULL;
    if (vfs_fs != NULL && vfs_fs->unlink_dentry != NULL)
        vfs_fs->unlink_dentry(dentry);

    *pp = dentry->d_next_sibling;
    dentry->d_inode = NULL;
    dentry->d_next_sibling = NULL;
    kfree(dentry);
    spin_unlock_irqrestore(&vfs_lock, flags);

    return 0;
}

int vfs_read_file(const char *path, u8 *buf, u64 cap, u64 *out_size)
{
    struct dentry *dentry;
    struct inode *inode;
    struct file *file;
    u64 done = 0;

    if (path == NULL || buf == NULL || out_size == NULL)
        return -EINVAL;

    dentry = vfs_lookup(path);
    if (dentry == NULL || dentry->d_inode == NULL)
        return -ENOENT;

    inode = dentry->d_inode;
    if ((inode->i_mode & S_IFDIR) != 0)
        return -EISDIR;
    if (inode->i_size > cap)
        return -EINVAL;

    file = open_inode(inode, O_RDONLY);
    if (file == NULL)
        return -ENOMEM;

    vfs_io_lock();
    while (done < inode->i_size) {
        i64 n = file->f_op->read(file, buf + done, inode->i_size - done);

        if (n <= 0)
            break;
        done += (u64)n;
    }
    vfs_io_unlock();

    file_close(file);
    *out_size = done;

    return done == inode->i_size ? 0 : -EINVAL;
}

int vfs_write_blob(const char *path, const void *data, u64 size)
{
    struct file *file;
    i64 n;

    file = vfs_open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (file == NULL)
        return -ENOENT;

    vfs_io_lock();
    n = file->f_op->write(file, data, size);
    vfs_io_unlock();
    file_close(file);

    return n == (i64)size ? 0 : -EINVAL;
}

void vfs_io_lock(void)
{
    spin_lock(&vfs_lock);
}

void vfs_io_unlock(void)
{
    spin_unlock(&vfs_lock);
}

i64 vfs_read_at(struct file *file, u64 offset, void *buf, u64 count)
{
    u64 old_pos;
    i64 n;

    if (file == NULL || file->f_op == NULL || file->f_op->read == NULL)
        return -EBADF;
    if (count == 0)
        return 0;

    old_pos = file->f_pos;
    file->f_pos = offset;
    n = file->f_op->read(file, buf, count);
    file->f_pos = old_pos;
    return n;
}

i64 vfs_write_at(struct file *file, u64 offset, const void *buf, u64 count)
{
    u64 old_pos;
    i64 n;

    if (file == NULL || file->f_op == NULL || file->f_op->write == NULL)
        return -EBADF;
    if (count == 0)
        return 0;

    old_pos = file->f_pos;
    file->f_pos = offset;
    n = file->f_op->write(file, buf, count);
    file->f_pos = old_pos;
    return n;
}
