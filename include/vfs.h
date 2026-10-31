/*
 * Lantest OS - VFS core structures (self-defined, not a Linux layout)
 */
#pragma once

#include <types.h>
#include <fs.h>

#define VFS_NAME_MAX 64u
#define VFS_PATH_MAX 256u

/* inode mode: type bits + permission bits */
#define S_IFDIR 0040000u
#define S_IFREG 0100000u
#define VFS_MODE_DIR  (S_IFDIR | 0755u)
#define VFS_MODE_FILE (S_IFREG | 0644u)

/* directory entry types for struct dirent */
#define DT_UNKNOWN 0u
#define DT_REG     1u
#define DT_DIR     2u

struct superblock;
struct inode;
struct dentry;

struct inode {
    u64 i_ino;
    u32 i_mode;
    u64 i_size;
    u32 i_nlink;
    u32 i_open_count;
    void *i_data;                 /* fs-private data (ramfs chain / lfs ino) */
    struct superblock *i_sb;
    const struct file_operations *i_fop;
    struct dentry *i_dentry;
};

struct dentry {
    char d_name[VFS_NAME_MAX];
    struct inode *d_inode;
    struct dentry *d_parent;
    struct dentry *d_first_child;
    struct dentry *d_next_sibling;
};

struct superblock {
    struct dentry *s_root;
    u64 s_inode_count;
};

struct dirent {
    u64 d_ino;
    u32 d_type;
    u32 d_pad;
    char d_name[VFS_NAME_MAX];
};

/*
 * Filesystem backend operations. The VFS path/dentry layer is shared; only
 * on-disk (or ramfs) object lifecycle differs.
 */
struct vfs_fs_ops {
    struct dentry *(*create_file)(struct dentry *parent, const char *name);
    struct dentry *(*mkdir)(struct dentry *parent, const char *name);
    void (*unlink_dentry)(struct dentry *dentry);
    void (*truncate)(struct inode *inode);
};

extern const struct vfs_fs_ops *vfs_fs;

void vfs_init(void);

struct dentry *vfs_lookup(const char *path);
struct file *vfs_open(const char *path, u32 flags);
int vfs_mkdir(const char *path, u32 mode);
int vfs_unlink(const char *path);
int vfs_read_file(const char *path, u8 *buf, u64 cap, u64 *out_size);
i64 vfs_read_at(struct file *file, u64 offset, void *buf, u64 count);
i64 vfs_write_at(struct file *file, u64 offset, const void *buf, u64 count);
void vfs_io_lock(void);
void vfs_io_unlock(void);
int vfs_write_blob(const char *path, const void *data, u64 size);
