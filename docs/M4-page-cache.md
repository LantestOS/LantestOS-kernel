# M4-18 — Page Cache：共享只读文件页

> 状态：**已完成并通过实测**（QEMU + 两次 `ls` 命中）  
> 日期：2026-10-28  
> 关联文档：[M4-filebacked-elf-lazy.md](./M4-filebacked-elf-lazy.md) · [M4-demand-paging.md](./M4-demand-paging.md)

---

## 1. 目标

file-backed 按需加载已经能跑，但每次 demand 都从 LantestFS 读。现在加 Page Cache：

1. 按 `(inode, page_index)` 缓存 4 KiB 物理页；
2. 只读映射（ELF 代码段）命中时直接共享缓存物理页；
3. 多个进程/多次 exec 复用同一物理页；
4. 可写映射（数据/BSS）仍走私有页 + COW，不污染缓存；
5. 缓存页由 PMM refcount 管理：缓存持有一份引用，用户映射再 `pmm_ref_inc`。

---

## 2. 数据结构

```c
struct page_cache_entry {
    struct inode *inode;
    u64 page_index;
    u64 phys;
    struct page_cache_entry *next;
};
```

- 64 个 hash bucket，键 `(inode, page_index)`；
- `struct file` 增加 `f_inode`，VFS `open_inode()` 填充；
- 未命中：分配物理页、清零、插入缓存，再由调用者读文件；
- 命中：直接返回缓存物理页；
- 当前不做淘汰；缓存页常驻（1 GiB 物理内存足够实验）。

---

## 3. demand 集成

只读 file-backed VMA：

```c
phys = page_cache_get(v->file, foff / PAGE_SIZE, &hit);
if (!hit)
    vfs_read_at(v->file, foff, phys, n);

pmm_ref_inc(phys);
paging_map_user(t->cr3, page, phys, 0);   /* 只读 PTE */
```

- 代码页映射为只读；多个进程共享同一 `phys`；
- 缓存持有一份 ref，用户映射持有一份；
- 进程退出时 PTE 释放只减引用，缓存页保留；
- 第二次 exec `/bin/ls` 命中缓存，不再读 LFS。

可写 file-backed VMA：

- 不走缓存，直接分配私有页并读文件；
- fork 后由 COW 复制；
- 避免写操作污染共享缓存。

---

## 4. 实测

连续两次 `ls`：

```
[pcache] miss inode=... index=1 phys=0x...227000
[demand] vaddr=0x8000000000 phys=0x...227000 pcache-hit=0 flags=19
...
[pcache] hit inode=... index=1 phys=0x...227000
[pcache] hit inode=... index=3 phys=0x...216000
[pcache] hit inode=... index=2 phys=0x...228000
...
```

- 第一次执行：miss，读 LFS；
- 第二次执行：hit，PTE 直接映射同一物理页；
- 代码页 flags=0x19（R+X+FILE），无 W；
- Shell 自身代码页也被缓存；
- 页面内容与执行结果不变。

`PCACHE_HIT_TEST: PASS`。

---

## 5. 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. 没有缓存淘汰/LRU，也没有内存压力回收；
2. 没有脏页/写回；可写映射不共享；
3. 文件被 truncate/写后没有缓存失效；
4. 没有 inode 引用计数，unlink 后缓存仍持有 inode 指针；
5. 没有多进程同时 #PF 的锁（单 CPU 安全）；
6. hash 固定 64 桶，无动态扩容；
7. 只缓存 4 KiB 文件页，不支持大页；
8. 没有 page cache 统计接口（仅日志）。

---

## 7. 下一步

- Page Cache 淘汰/LRU + 写回；
- `munmap/mprotect`，共享可写映射；
- 完全惰性 execve（ELF 头也 file-backed）；
- 用户栈动态扩展；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 8. 复现清单

- [x] `struct file.f_inode`，VFS 填充
- [x] `(inode, page_index)` hash page cache
- [x] miss 分配/清零/插入，hit 复用
- [x] 只读代码页共享映射
- [x] PMM refcount：缓存引用 + 用户映射引用
- [x] 可写 file-backed 页仍私有/COW
- [x] 两次 `ls` 第二次命中
- [x] Shell 自身代码页缓存
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告