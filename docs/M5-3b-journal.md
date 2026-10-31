# M5-3b — LantestFS Journaling 与备份超级块

> 状态：**已完成并通过实测**（QEMU + 主 SB 损坏恢复 + 伪造 journal replay）  
> 日期：2026-10-29  
> 关联文档：[M5-lantestfs.md](./M5-lantestfs.md) · [M4-page-cache.md](./M4-page-cache.md)

---

## 1. 目标

让 LantestFS 从“尽力而为”升级为“能抗掉电/主超级块损坏”：

1. 磁盘布局 v2，预留 Journal 与 backup superblock；
2. 所有 cache 写入先写 Redo Log，提交后再 checkpoint 到 home block；
3. 挂载时检测 committed journal 并 replay；
4. 主超级块损坏时从 backup 自动恢复；
5. 兼容 LantestFS v1：旧 magic 自动重格式化（v2 布局）。

---

## 2. 布局（16 MiB / 4096 blocks）

```
block 0            primary superblock
block 1            block bitmap
block 2            inode bitmap
block 3..10        inode table
block 11..4061     data
block 4062         backup superblock
block 4063         journal header
block 4064..4094   journal data slots (31)
block 4095         reserved for virtio-blk boot_count self-test
```

- 4062..4095 在 block bitmap 中标记为已用；
- 最后一块必须保留给 virtio self-test 的 boot_count，否则持久化测试会被覆盖（实际踩过）。

---

## 3. Journal 格式

```c
struct lfs_journal_header {
    u32 magic;       /* LJRN */
    u32 commit;      /* CMIT */
    u32 count;
    u32 target;      /* home block */
    u32 slot;        /* journal data slot (1) */
    u8 pad[...];
};
```

当前是单块事务、单 slot 的简化 Redo Log。

---

## 4. 写入流程

`lfs_cache_write()` → `lfs_journal_write()`：

1. 数据块写入 journal data slot；
2. 写 journal header（无 commit）；
3. 写 header 并置 `CMIT`；
4. checkpoint 到 home block；
5. 清空 journal header。

崩溃点分析：

- 步骤 1/2 崩溃：无 commit，忽略；
- 步骤 3 后崩溃：header 有 commit，挂载时 replay；
- 步骤 4 后崩溃：replay 幂等；
- 步骤 5 后崩溃：干净。

`lfs_format()` 期间 `lfs_journal_enabled=0`，避免格式化自身走日志。

---

## 5. 挂载与恢复

### 5.1 主超级块损坏

读取 block 0，验证 magic/version/layout；失败则读取 block 4062：

```
[lfs] primary superblock restored from backup
[lfs] mounted: magic=LNTSTFS2 blocks=4096 inodes=256 root=1 journal=4063
```

- 备份有效：写回 block 0；
- 备份无效：重新 format（最后手段）。

### 5.2 Journal replay

读取 journal header：

- magic/commit/count 匹配，target 合法；
- 从 slot 读数据块，写回 target；
- 清空 journal header；
- 打印 `[lfs] journal replay: block N`。

---

## 6. 实测

### 6.1 干净启动

```
[lfs] formatted: 4096 blocks, data_start=11, inodes=256
[lfs] mounted: magic=LNTSTFS2 blocks=4096 inodes=256 root=1 journal=4063
[lfs] self-test: PASS (boot_count=1)
```

### 6.2 持久化

```
persist1: lfs self-test boot_count=1
persist2: lfs self-test boot_count=2
M5-2 persistence test: PASS (virtio + LFS boot_count 1 -> 2)
```

### 6.3 主超级块损坏

清零 block 0 后启动：

```
[lfs] primary superblock restored from backup
[lfs] mounted: ...
[lfs] self-test: PASS (boot_count=4)
Lantest shell (M4-13) - type 'help'
```

### 6.4 Journal replay

手工构造 committed journal（target=100，slot=1）后启动：

```
[lfs] journal replay: block 100
[lfs] mounted: magic=LNTSTFS2 ... journal=4063
Lantest shell (M4-13) - type 'help'
```

- replay 真的写回 home block 并清空 journal；
- 系统继续正常挂载。

### 6.5 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 踩坑

| 问题 | 处理 |
| --- | --- |
| backup 放最后一块，覆盖 virtio boot_count 扇区，持久化测试变 1 | backup 移到 journal 前一块（4062），最后一块保留给 virtio |
| replay 调用 `lfs_cache_get` 时函数尚未声明 | 改为局部 static slot buffer + raw read/write |
| v1 磁盘没有 journal 预留位 | magic/version 升到 `LNTSTFS2`/2，旧盘重格式化 |
| journal 占用最后数据块 | 位图预留 backup..last，共 34 块 |

---

## 8. 当前限制

1. **单块事务**：多块元数据更新不是原子的，崩溃仍可能留下跨块不一致；
2. 没有 group commit/事务边界，每个 home block 都独立提交；
3. 所有 cache 写入（含普通文件数据）都走 journal，5 倍写放大；
4. Journal 只有一个 slot，不能记录多块事务；
5. 没有 checksum，torn journal 只能靠 magic/commit 粗判；
6. 没有 journal wrap/循环使用；
7. 没有 fsck；
8. backup superblock 只在 format 时写，后续 superblock 不更新（当前 superblock 是静态的）；
9. 单 CPU，无文件系统锁。

---

## 9. 下一步

- 多块事务 + group commit + checksum；
- Journal wrap + replay 顺序/幂等性；
- fsck/一致性检查；
- Page Cache 脏页写回与 LRU；
- 用户态 stdio/libc；
- 多核锁。

---

## 10. 复现清单

- [x] LantestFS v2 magic/version
- [x] backup superblock @4062
- [x] journal header @4063 + slot @4064
- [x] 位图预留 backup..last
- [x] cache write → journal → commit → home
- [x] mount replay committed journal
- [x] primary SB 损坏恢复
- [x] 最后一块留给 virtio self-test
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告