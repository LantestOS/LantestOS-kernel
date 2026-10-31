/*
 * Lantest OS - in-memory filesystem (ramfs)
 */
#pragma once

#include <vfs.h>

void ramfs_init(void);
struct dentry *ramfs_root(void);
struct dentry *ramfs_create_file(struct dentry *parent, const char *name);
struct dentry *ramfs_mkdir(struct dentry *parent, const char *name);

/* Empty a regular file: free all data blocks and set size to 0. */
void ramfs_truncate(struct inode *inode);

/*
 * Unlink bookkeeping: decrement i_nlink and free the inode/data only when it
 * is no longer open (delete-on-close).
 */
void ramfs_unlink_inode(struct inode *inode);

extern const struct file_operations ramfs_file_ops;

extern const struct vfs_fs_ops ramfs_vfs_ops;
