# M4-19 — munmap / mprotect：内存管理闭环

> 状态：**已完成并通过实测**（QEMU + muntest）  
> 日期：2026-10-28  
> 关联文档：[M4-page-cache.md](./M4-page-cache.md) · [M4-cow-vma.md](./M4-cow-vma.md)

---

## 1. 目标

1. VMA 支持区间拆分、删除、权限修改；
2. `munmap(addr,len)`：清 PTE、物理页 refcount--、移除/拆分 VMA；
3. `mprotect(addr,len,prot)`：修改 VMA flags 与 PTE W 位；
4. mprotect 打开 W 时正确处理共享页（refcount>1 → COW）；
5. `muntest` 验证只读保护、RW 恢复、munmap 拆分。

---

## 2. VMA 区间操作

### 2.1 拆分

```c
vma_split_at(t, addr):
    find v where v->start < addr < v->end
    right = clone(v, addr, v->end)
    v->end = addr
    v->next = right
```

- clone 保留 file/file_offset/file_data_end；
- file-backed VMA 拆分时 `file_get()`；
- 原 VMA 保留左半部分。

### 2.2 munmap

```c
vma_split_at(start);
vma_split_at(end);
remove every VMA fully inside [start,end)
```

- 完全覆盖：删除；
- 中间挖洞：拆成左右 VMA；
- 删除时 `file_close()`。

### 2.3 mprotect

- 在 start/end 处拆分；
- 对完全落在区间内的 VMA：
  ```c
  v->flags = (v->flags & (VM_FILE | VM_ANON)) | prot;
  ```
- 保留 backing 属性，只改 R/W/X。

---

## 3. 页表操作

### 3.1 munmap

```c
for each page:
    pte = paging_user_pte(cr3, page);
    if present:
        *pte = 0;
        pmm_free_page(pte_phys);   /* refcount-- */
        invlpg(page);
```

- Page Cache 页有缓存引用，不会因 unmap 被释放；
- 私有页最后一个引用消失时真正回收；
- 页表页暂不回收。

### 3.2 mprotect

```c
if (!write) {
    PTE &= ~(WRITE | COW);
} else if (pmm_ref_count(phys) > 1) {
    PTE |= COW; PTE &= ~WRITE;   /* 下次写触发 COW */
} else {
    PTE |= WRITE; PTE &= ~COW;
}
invlpg(page);
```

- 共享代码页/缓存页 mprotect 可写时不会直接改共享页，而是置 COW；
- 私有页 refcount=1 时直接恢复 W；
- 清 W 时同时清 COW，写操作将触发 SIGSEGV。

---

## 4. 系统调用

| 调用 | 号 | 参数 | 返回 |
| --- | --- | --- | --- |
| `munmap` | 21 | `addr, len` | 0 / -EINVAL |
| `mprotect` | 22 | `addr, len, prot` | 0 / -EINVAL |

`prot`：1=READ，2=WRITE，4=EXEC。

---

## 5. muntest

```c
p = mmap(8192);
p[0] = 11; p[4096] = 22;

mprotect(p, 4096, PROT_READ);

if (fork() == 0) {
    p[0] = 33;   /* 子进程写只读页 -> SIGSEGV */
    exit(0);
}
wait();          /* status 139 */

mprotect(p, 4096, PROT_READ | PROT_WRITE);
p[0] = 44;       /* 恢复可写 */

munmap(p + 4096, 4096);
/* p[0] 仍为 44 */
```

实测：

```
[segv] pid=16 name=fork-child cr2=0x8000080000 err=7
[signal] pid=16 name=fork-child signal=11 -> terminate (status=139)
muntest: PASS
```

- `err=7` = present + write + user；
- 只读页写触发 SIGSEGV，子进程死亡、父进程继续；
- mprotect RW 后写入成功；
- munmap 中间/尾页拆分正常；
- `M5-2 boot test: PASS`。

---

## 6. 回归

- `make test` → **PASS**（含 muntest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制

1. 页表页不回收（只清 PTE）；
2. `mprotect` 不支持真正的 X/NX 位；
3. 没有 `mmap` MAP_SHARED/MAP_PRIVATE 语义；
4. 没有文件写回，mprotect 写 COW 只影响私有页；
5. munmap 不处理大页（无大页）；
6. VMA 只有拆分/合并，没有区间树，操作 O(n)；
7. 没有 `msync`/`madvise`；
8. 单 CPU，无锁。

---

## 8. 下一步

- Page Cache LRU/写回与脏页；
- 文件系统 Journaling/双超级块（M5-3b）；
- `msync/madvise`、共享映射；
- 用户态 malloc 可在 mmap/munmap 上实现；
- 完全惰性 execve。

---

## 9. 复现清单

- [x] VMA 拆分（file ref 正确）
- [x] vma_unmap_range 删除/挖洞
- [x] paging_unmap_range 清 PTE + refcount--
- [x] mprotect 改 VMA flags
- [x] mprotect 清 W / 恢复 W / 共享页 COW
- [x] munmap(21)、mprotect(22) 系统调用
- [x] muntest 只读保护 + SIGSEGV 139
- [x] muntest RW 恢复 + munmap
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告