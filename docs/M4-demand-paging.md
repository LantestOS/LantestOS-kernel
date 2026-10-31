# M4-15 — 按需分页（Demand Paging）

> 状态：**已完成并通过实测**（QEMU + `mmaptest`）  
> 日期：2026-10-28  
> 关联文档：[M4-cow-vma.md](./M4-cow-vma.md) · [design.md](./design.md)

---

## 1. 目标

把 mmap 从“立刻分配全部物理页”改成真正的按需分页：

1. `mmap(size)` 只创建 VMA，不分配任何物理页；
2. 用户第一次读/写该区间 → #PF（present=0）；
3. #PF 发现地址落在 VMA 内 → 分配清零物理页 → 建立 PTE → 重试指令；
4. COW 与 demand 共用同一个 #PF 入口，先 COW 后 demand；
5. VMA 成为 mmap 区间的唯一描述，物理页只在访问时出现。

---

## 2. mmap 只建 VMA

```c
mmap(size):
    start = align_up(task->mmap_next);
    end   = start + align_up(size);
    vma_add(task, start, end, VM_READ | VM_WRITE | VM_ANON);
    task->mmap_next = end;
    return start;
```

- **不再调用 `paging_ensure_user_page`**；
- 不分配页表页，也不分配数据页；
- 返回的地址在页表里是 not-present，但落在 VMA 区间内。

---

## 3. #PF 处理顺序

`isr_handler()` 中：

```c
if (int_no == 14) {
    cr2 = read_cr2();
    if (cr2 in user range &&
        (paging_handle_cow(cr2) || paging_handle_demand(cr2)))
        return;      /* iretq 重试原指令 */
}
```

- 先 COW：present + COW 位；
- 再 demand：present=0 且地址属于 VMA；
- 若 present 但不是 COW，或地址不在 VMA，则走原有异常/fixup/panic；
- 内核 `copy_to_user` 访问 not-present mmap 页也会走 demand。

---

## 4. demand 处理

```c
int paging_handle_demand(u64 vaddr)
{
    v = vma_find(task_current(), vaddr);
    if (!v) return 0;

    pte = paging_user_pte(cr3, vaddr);
    if (pte && (*pte & PRESENT)) return 0;

    phys = paging_ensure_user_page(cr3, vaddr);  /* 分配 + 清零 + 映射 */
    if (!phys) return 0;

    invlpg(vaddr);
    return 1;
}
```

- `paging_ensure_user_page()` 分配 4 KiB 页并清零，防止泄露旧数据；
- 建立 present + user + writable 的 PTE；
- 返回后 CPU 重试触发缺页的指令。

---

## 5. 实测

`mmaptest`：mmap 8 KiB，写入 8192 字节。

```
[demand] vaddr=0x0000008000080000 phys=0x0000000000250000 flags=7
[demand] vaddr=0x0000008000081000 phys=0x0000000000251000 flags=7
mmaptest: PASS
```

- mmap 时物理页数没有变化；
- 只有实际写入的两页各触发一次 demand；
- 随后 fork，mmap 页参与 COW；
- `cowtest`/`mmaptest` 均 PASS。

如果 mmap 1 GiB 而只写一个字节，现在也只会分配 1 个物理页，而不是 1 GiB。

---

## 6. 回归

- `make test` → **PASS**（含 cowtest/mmaptest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 已知限制

1. 只有匿名 mmap；没有 file-backed mmap；
2. 没有 `munmap/mprotect`；
3. VMA 尚未覆盖 ELF/stack，因此 execve 仍是 eager 加载；
4. 没有 swap；
5. demand 页权限统一为可写；VMA flags 尚未驱动 PTE 权限；
6. 没有按页的 VMA 拆分/合并；
7. #PF 对“用户地址但不在 VMA”仍直接 panic，而不是 SIGSEGV；
8. 单 CPU，无锁。

---

## 8. 下一步

- file-backed mmap：VMA 记录 fd/offset，#PF 从 LantestFS 读页；
- `munmap/mprotect`；
- ELF 按需加载：PT_LOAD 只建 VMA，#PF 读文件；
- **M5-3b**：双超级块 + fsck + journaling；
- SIGSEGV/非法 VMA 访问处理。

---

## 9. 复现清单

- [x] mmap 只建 VMA、不分配物理页
- [x] #PF 检测 VMA 区间并 demand 分配
- [x] 清零新页，建立 PTE，invlpg
- [x] COW 优先，demand 次之
- [x] 内核 copy_to_user 的 demand 路径
- [x] `mmaptest` 8 KiB 只分配 2 页并 PASS
- [x] cowtest/mmaptest、make test/test-persist/test-exception PASS
- [x] 构建零警告
