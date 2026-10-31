# M4-20 — 用户态 malloc：跑在 mmap/munmap 上

> 状态：**已完成并通过实测**（QEMU + malloctest）  
> 日期：2026-10-28  
> 关联文档：[M4-munmap-mprotect.md](./M4-munmap-mprotect.md) · [M4-demand-paging.md](./M4-demand-paging.md)

---

## 1. 目标

不再用静态数组，给用户态一个真正的堆分配器：

1. `malloc(size)`：小对象从 64 KiB mmap chunk 切分，大对象（≥8 KiB）单独 mmap；
2. `free(ptr)`：小对象回收到 free list；大对象直接 `munmap`；
3. `calloc(count, size)`：零初始化；
4. `realloc(ptr, size)`：原地或分配新块 + 拷贝 + free；
5. 用 malloctest 压力测试：
   - 小对象写入/读取；
   - calloc 全零；
   - realloc 保留旧数据；
   - 64 KiB 大块写满 64 KiB（触发 demand paging）；
   - free 大块真正 munmap；
   - 释放后再次 malloc 复用。

---

## 2. 分配器结构

```c
struct malloc_block {
    u64 size;
    struct malloc_block *next;
    struct malloc_block *prev;
    u32 free;
    u32 large;
};
```

- 块头 32 字节，用户指针 = 块地址 + 32；
- 16 字节对齐；
- `malloc_free_list` 单链表，first-fit；
- 小块从 64 KiB chunk 分配并可按需切分；
- 大块独立 mmap，`large=1`，free 时 `sys_munmap`；
- 小 chunk 暂不整体 unmap（进程退出时由内核回收 VMA）。

---

## 3. malloc

```c
if (size >= MALLOC_LARGE) {
    base = sys_mmap(size + sizeof(block));
    return base + sizeof(block);
}

for (b in free_list)
    if (b->free && b->size >= size)
        return split_and_mark(b, size);

base = sys_mmap(64 KiB);
add block to free list;
return split_and_mark(base, size);
```

- 每个 mmap 只建 VMA，物理页在首次写时 demand 分配；
- 64 KiB 大块实际只有被写的页占用物理内存（malloctest 写满全部页）。

---

## 4. free / calloc / realloc

```c
free(p):
    block = p - 32
    if (block->large) sys_munmap(block, block->size + 32)
    else block->free = 1

calloc(n, sz):
    p = malloc(n*sz); 零填充

realloc(p, new):
    if block->size >= new: return p
    np = malloc(new); 拷贝 min(old,new); free(p); return np
```

- 大块 free 真正触发 VMA 拆分/删除与物理页 refcount--；
- 小块 free 仅回收 free list（暂不合并）。

---

## 5. malloctest

```c
a = malloc(100); b = malloc(200); 写入/校验
z = calloc(64, 8); 校验 512 字节全零
a = realloc(a, 256); 校验旧数据
big = malloc(64 * 1024); 写满 64 KiB
free(big);        /* munmap 大块 */
free(a); free(b); free(z);
a = malloc(64);   /* 复用 */
```

实测：

```
[M4] malloctest task: pid=13
[M3b] entering Ring 3: user-malloctest ...
malloctest: PASS
[proc] exit: pid=13 name=user-malloctest status=0 -> ZOMBIE
```

- 没有 panic / SIGSEGV；
- 大块写满 64 KiB 触发 16 页 demand；
- free 大块 munmap 成功；
- 小对象复用 free list；
- `make test` 中 malloctest PASS。

---

## 6. 回归

- `make test` → **PASS**（含 malloctest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制

1. 小块不合并/不整体 munmap，长期运行会产生碎片与 chunk 常驻；
2. first-fit 单链表，O(n)；
3. 无 size class/slab；
4. 无线程安全（单 CPU）；
5. 大块阈值固定 8 KiB，无 mmap 阈值调优；
6. 没有 `brk/sbrk`；
7. malloc 元数据不校验 magic，越界写会破坏分配器；
8. 没有对 mmap/munmap 失败路径的完整回滚。

---

## 8. 下一步

- malloc 小块合并、按 chunk 整体 munmap；
- **M5-3b**：LantestFS journaling + 双超级块；
- Page Cache LRU/脏页写回；
- 用户态 stdio/libc 雏形；
- `msync/madvise`。

---

## 9. 复现清单

- [x] 64 KiB chunk + free list
- [x] 大对象独立 mmap
- [x] free 大对象 munmap
- [x] calloc 零初始化
- [x] realloc 保留数据
- [x] 64 KiB 写满触发 demand paging
- [x] 释放后复用
- [x] malloctest PASS
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告