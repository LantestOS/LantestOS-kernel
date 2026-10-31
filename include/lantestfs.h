/*
 * Lantest OS - LantestFS persistent filesystem (M5-2)
 */
#pragma once

#include <vfs.h>

struct dentry *lfs_mount(void);
int lfs_selftest(void);
int lfs_mounted(void);

extern const struct vfs_fs_ops lfs_vfs_ops;
