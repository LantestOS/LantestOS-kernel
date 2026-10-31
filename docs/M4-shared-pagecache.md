# M4-21 — MAP_SHARED、脏页回写与 Page Cache LRU

> 状态：**已完成并通过实测**（QEMU + shmtest）  
> 日期：2026-10-29  
> 关联文档：[M4-page-cache.md](./M4-page-cache.md) · [M5-3b-journal.md](./M5-3b-journal.md)

---

## 1. 目标

1. Page Cache entry 增加 dirty 与 LRU 链表；
2. 缓存满时淘汰 LRU 尾页；脏页先写回再淘汰；
3. 新增 `VM_SHARED` 与 `mmap_shared(fd,off,len)`；
4. 共享映射的 PTE 带 `PTE_SHARED`，fork 时**不 COW**，父子共享同一物理页；
5. `msync(addr,len)` 把脏页通过 `vfs_write_at` 写回文件。

---

## 2. Page Cache Entry

```c
struct page_cache_entry {
    struct inode *inode;
    struct file *file;      /* 持有文件引用，供写回 */
    u64 index;
    u64 phys;
    u32 dirty;
    struct page_cache_entry *next;      /* hash */
    struct page_cache_entry *lru_prev;
    struct page_cache_entry *lru_next;
};
```

- 最多 64 个 entry；
- 命中：移到 LRU 头；
- 未命中：必要时从 LRU 尾淘汰；
  - 只淘汰 `pmm_ref_count(phys)==1` 的页（没有进程映射）；
  - dirty 页先 `vfs_write_at` 写回，成功后释放；
  - 全部被映射时允许暂时超出上限。

---

## 3. MAP_SHARED

新增 VMA flag：

```c
#define VM_SHARED 0x20u
```

`mmap_shared(fd, offset, len)`：

- VMA flags = `VM_READ|VM_WRITE|VM_FILE|VM_SHARED`；
- 不分配物理页；
- demand 时取 Page Cache 页，映射为 `PTE_WRITE|PTE_SHARED`；
- `page_cache_mark_dirty()` 标记 dirty。

### fork 语义

`paging_copy_user_space()`：

```c
if (flags & PTE_SHARED) {
    pmm_ref_inc(phys);
    paging_map_user(child, virt, phys, PTE_WRITE | PTE_SHARED);
    continue;   /* 不清 W、不置 COW */
}
```

- 父/子进程共享同一物理页；
- 子进程写入后父进程立即可见；
- 与私有页的 COW 路径完全分离。

---

## 4. msync

```c
msync(addr, len):
    for each VM_SHARED VMA overlapping [addr,addr+len):
        foff = v->file_offset + (start - v->start)
        page_cache_flush(inode, foff/4096, ...)
```

`page_cache_flush()` 遍历 hash：

- inode 与页区间匹配且 dirty；
- `vfs_write_at(file, index*4096, phys, 4096)`；
- 清除 dirty，打印 `[pcache] writeback ...`。

---

## 5. shmtest

```c
创建 /shmtest，写入 'A'
mmap_shared(fd, 0, 4096)
p[0] == 'A'

fork:
    child: p[0] = 'B'
parent:
    wait
    p[0] == 'B'        /* MAP_SHARED 跨进程可见 */
    msync(addr, 4096)

重新 open /shmtest，read 1 字节 == 'B'   /* 写回成功 */
```

实测：

```
[pcache] miss inode=... index=0 phys=0x...2b0000
...
[pcache] writeback inode=... index=0
shmtest: PASS
```

- 子进程写 'B'，父进程立刻看到 'B'；
- msync 后文件内容持久化为 'B'；
- 没有 panic/SIGSEGV；
- `make test` 中 shmtest PASS。

---

## 6. 回归

- `make test` → **PASS**（含 shmtest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制

1. dirty 采用“共享映射即标记”的保守策略，只读访问也会写回；
2. 没有后台 flush 线程，只有 msync/淘汰触发；
3. LRU 固定 64 entry，没有内存压力自适应；
4. 被映射的页不能被淘汰，缓存可能暂时超上限；
5. 没有 MAP_SHARED 匿名映射；
6. 没有 msync 异步/范围校验细化；
7. 写回失败会阻止淘汰，可能反复尝试；
8. 没有 page cache 全局统计/锁；
9. 文件 inode 无引用计数，unlink 后缓存条目仍可能持有 inode。

---

## 8. 下一步

- 后台 flush 内核线程 / 周期性 writeback；
- LRU 与内存压力联动；
- 多块 Journal transaction/group commit（M5-4）；
- SMP + spinlock；
- 用户态 stdio/libc。

---

## 9. 复现清单

- [x] page cache dirty/file/LRU
- [x] 64 entry 上限与 LRU 淘汰
- [x] 脏页淘汰前写回
- [x] VM_SHARED / mmap_shared
- [x] PTE_SHARED，fork 不 COW
- [x] 子进程写共享页，父进程可见
- [x] msync 写回文件
- [x] 重新读取文件内容为 'B'
- [x] shmtest PASS
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告