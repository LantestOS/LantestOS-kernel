# M5 Step 2 — LantestFS 持久化文件系统

> 状态：**M5-2 已完成并通过实测**（QEMU 10.2.1 + virtio-blk 16 MiB 磁盘）  
> 日期：2026-10-28  
> 关联文档：[M5-block.md](./M5-block.md) · [design.md](./design.md)

---

## 1. 本步目标

把 M5-1 的裸块设备变成真正可持久化的文件系统：

1. 设计 **LantestFS** 磁盘布局（4 KiB 块）；
2. 自研超级块 / inode / 块位图 / inode 位图 / 目录项；
3. 8 块 **buffer cache**（读缓存 + write-through）；
4. `format`（空盘自动格式化）与 `mount`（从磁盘重建 dentry 树）；
5. VFS 增加 **后端抽象**：ramfs 与 LantestFS 共用路径/dentry 层；
6. 持久化 `open/read/write/close/mkdir/unlink`；
7. 验证：Shell 里 `echo hello > /test.txt` → 重启 → `cat /test.txt` 输出 `hello`。

---

## 2. 磁盘布局（16 MiB / 4096 块）

| 块 | 用途 |
| --- | --- |
| 0 | 超级块（magic `LNTSTFS1`、版本、块大小、总块数、inode 表/位图位置、root ino） |
| 1 | 块位图（4096 bit，标记已分配块） |
| 2 | inode 位图（256 bit） |
| 3..10 | inode 表（256 × 128 B = 32 KiB） |
| 11..4095 | 数据块（目录项 / 文件内容） |

磁盘结构体全部 `packed` 并用 `static_assert` 锁定大小：

```c
struct lfs_superblock { ... };            /* 4096 B */
struct lfs_inode_disk { ... };            /* 128 B */
struct lfs_dirent { u32 ino; char name[60]; };  /* 64 B */
```

inode 支持 **12 个直接块指针 + 一级间接块**（M5-3a），单文件最大约 4.2 MiB。

---

## 3. Buffer cache

- 8 个 4 KiB cache entry；
- 读：命中直接返回，未命中淘汰最久未使用项并 `virtio_blk_read`；
- 写：**write-through** 立即写回磁盘，同时更新已缓存副本；
- 元数据（超级块、位图、inode 表、目录块、数据块）全部走 cache。

写入顺序（尽力而为）：

```
分配块（位图）→ 写数据块 → 更新 inode → 更新目录项
```

没有 journaling；M5 阶段先保证“单次写完成后一致”，断电极端场景可能留下已分配但未引用的块（泄漏，而非数据错乱）。

---

## 4. VFS 后端抽象

VFS 的路径/dentry 层保持通用，新增：

```c
struct vfs_fs_ops {
    struct dentry *(*create_file)(struct dentry *parent, const char *name);
    struct dentry *(*mkdir)(struct dentry *parent, const char *name);
    void (*unlink_dentry)(struct dentry *dentry);
    void (*truncate)(struct inode *inode);
};
```

- `vfs_init()` 检测 virtio-blk：
  - 有设备 → `lfs_mount()`，成功则后端 = LantestFS；
  - 无设备 / 挂载失败 → 回退 ramfs（RAM-only）。
- `vfs_open` 使用 `inode->i_fop`（ramfs / lfs 各自 fops）；
- `vfs_mkdir` / `vfs_open(O_CREAT)` / `vfs_unlink` 通过后端分派；
- `unlink_dentry` 接收 dentry，是因为 LantestFS 需要 `parent + name` 才能从目录文件里删除对应 64 字节目录项。

---

## 5. format / mount

### format（空盘）

1. 写超级块；
2. 块位图：标记 0..`data_start-1` 已用；
3. inode 位图：标记 root inode (1) 已用；
4. 清零 inode 表，写 root inode（`S_IFDIR`）；
5. 打印 `[lfs] formatted: 4096 blocks, data_start=11, inodes=256`。

### mount（有效盘）

1. 读 block 0，校验 magic/版本/块大小/布局；
2. 从 root inode 递归读目录项，重建 VFS dentry 树；
3. 打印 `[lfs] mounted: magic=LNTSTFS1 blocks=4096 inodes=256 root=1`。

---

## 6. 目录与文件操作

- **目录项**：`{ ino, name[60] }`，目录文件大小 = 64 × 项数；
- `lfs_dir_add`：追加目录项，必要时分配新数据块；
- `lfs_dir_remove`：找到项后把后续项前移 64 字节，缩小目录大小；
- `lfs_dir_read`：按 `file->f_pos` 返回一个 `struct dirent`（`ls` 用）；
- **文件 read/write**：按 `f_pos` 访问 12 个直接块；写时按需分配块、更新 inode size；
- **unlink + delete-on-close**：删除目录项 + `i_nlink--`；最后一个 open fd 关闭时释放数据块/inode；
- **O_TRUNC**：释放全部直接块，size=0。

---

## 7. 启动自检与持久化证据

每次启动在根目录创建/递增 `/lfs-test`：

```
[lfs] formatted: 4096 blocks, data_start=11, inodes=256
[lfs] mounted: magic=LNTSTFS1 blocks=4096 inodes=256 root=1
[vfs] backend: LantestFS (persistent)
[lfs] self-test: PASS (boot_count=1)
```

第二次启动：

```
[lfs] mounted: magic=LNTSTFS1 blocks=4096 inodes=256 root=1
[lfs] self-test: PASS (boot_count=2)
```

（virtio 的裸盘自检已移到磁盘 **最后一个 sector**，避免与 LFS 超级块冲突。）

### 持久化 /bin/hello（M4-5）

- 第一次启动：内核把嵌入的 `hello.elf` 写入 LantestFS；
- 第二次启动：`/bin/hello` 已在磁盘上，内核打印 `[M4] /bin/hello already on disk`，不再重写；
- Shell / `execve` 直接读取磁盘上的 ELF；
- Shell 现在支持 `run /bin/hello one two`，参数通过初始栈 argc/argv 传递。

### 真实 Shell 场景

第一次启动：

```
lantest> echo hello > /test.txt
echo: wrote /test.txt
lantest> cat /test.txt
hello
```

重启后：

```
lantest> ls
lfs-test
bin/
hello.txt
big.bin
test.txt
lantest> cat /test.txt
hello
```

`LFS_PERSIST_TEST: PASS`。

### 自动化

- `make test` → **PASS**（含 LantestFS 挂载/自检、fork/exec、VFS、uaccess）；
- `make test-persist` → **PASS**（virtio + LFS boot_count 1→2）；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 8. 踩坑记录

| 问题 | 原因 / 处理 |
| --- | --- |
| 首次挂载直接失败 | `lfs_mount` 读 superblock 前 `lfs_sb.total_blocks` 还是 0，`lfs_raw_read` 校验把 block0 判越界；先填 total_blocks 再读 |
| magic 比较失败 | `"LNTSTFS1"` 是 8 字节、无 NUL，不能用 `str_eq`；改用 `mem_cmp(...,8)` |
| format 写 inode 失败 | `lfs_format` 在 `lfs_sb = sb` 之前就调用 `lfs_write_inode`，inode table 偏移为 0；先赋值再操作 |
| unlink 只改 inode | VFS 后端原本只传 `struct inode *`，LFS 拿不到父目录和文件名；改为 `unlink_dentry(struct dentry *)`，先删目录项再减 nlink |
| buffer cache LRU 选错 victim | 有空闲 entry 时仍可能选有效项；修正为先保留 invalid entry，再按 age 淘汰 |
| 空盘/已格式化盘 | mount 检查 magic：无效则 format；有效则直接挂载 |

---

## 9. 已知限制

1. 单文件最多 **12 直接块 + 1 级间接块 ≈ 4.2 MiB**；没有双间接/双超级块/fsck。
2. 没有 journaling / fsck；断电一致性是“尽力而为”；
3. `unlink` 只支持普通文件，不支持目录/硬链接；
4. 没有权限检查、没有 mount/unmount（单根 LantestFS）；
5. block cache 固定 8 项、write-through，无延迟写/回写；
6. 目录项删除是搬移，目录碎片不回收；
7. 空盘会自动格式化；如果 magic 被破坏，会丢失原有数据（后续加备份超级块）。

---

## 10. 下一步

### M5-3 / M4-5

1. 双超级块 + 简单 fsck；
2. 双超级块 + 简单 fsck；
3. 目录 unlink/`getdents` 优化；
4. M4-5：argv/envp、键盘 IRQ、COW、管道；
5. 之后可做 LantestFS 的 journaling / 延迟分配。

---

## 11. 复现清单

- [x] 超级块 / inode / 位图 / 目录项磁盘布局
- [x] 8 块 buffer cache + write-through
- [x] format（空盘）与 mount（重建 dentry 树）
- [x] VFS 后端抽象（ramfs / LantestFS）
- [x] 文件/目录 create、read、write、unlink、O_TRUNC
- [x] `/lfs-test` boot_count 1→2
- [x] Shell `echo > /test.txt` → 重启 → `cat` 读到 `hello`
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
