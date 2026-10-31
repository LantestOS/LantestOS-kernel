# M4-14 — COW fork、页引用计数与 VMA/mmap

> 状态：**已完成并通过实测**（QEMU + `cowtest` / `mmaptest`）  
> 日期：2026-10-28  
> 关联文档：[M4-job-control.md](./M4-job-control.md) · [M4-pgid-ctrl-c.md](./M4-pgid-ctrl-c.md)

---

## 1. 本步目标

1. PMM 每页引用计数，free 只在 refcount 归零时真正释放；
2. fork 不再深拷贝用户数据页：复制页表、共享物理页、清 W、置 COW；
3. #PF 处理 COW：写保护页触发时复制新页、恢复 W、旧页 refcount--；
4. VMA 抽象：每任务维护 `vm_area_struct` 链表；
5. 自定义 `mmap(size)`：分配匿名页、登记 VMA，fork 复制 VMA 且 mmap 页同样 COW。

---

## 2. 页引用计数

```c
static u16 refcount[MAX_PAGES];
```

- `pmm_alloc_page()` / `pmm_alloc_pages()`：refcount = 1；
- `pmm_ref_inc(phys)`：fork 共享页时 +1；
- `pmm_free_page(phys)`：
  - refcount > 1 → 只减一，不释放；
  - refcount == 1 → 清零、清 bitmap、free_pages++；
- `pmm_free_pages()` 也按引用计数逐页处理；
- `paging_destroy_user_space()` 对共享页只减引用，最后一个映射退出时才真正回收。

---

## 3. COW fork

`paging_copy_user_space()` 从“深拷贝”改为：

```c
for each mapped user page:
    flags = *src_pte;
    phys = flags & PAGE_MASK;

    if (flags & PTE_WRITE) {
        *src_pte = (flags & ~PTE_WRITE) | PTE_COW;   /* 父进程也变只读 */
        child_flags = PTE_USER | PTE_COW;
    } else {
        child_flags = PTE_USER | (flags & PTE_COW);
    }

    pmm_ref_inc(phys);
    paging_map_user(child_pml4, virt, phys, child_flags);
```

- 子进程有独立页表，但叶子 PTE 指向同一物理页；
- 父进程的 W 位也被清掉，所以双方首次写都会触发 #PF；
- 末尾刷新 CR3，清掉父进程 TLB 里的可写映射。

软件位 `PTE_COW = 1 << 9` 区分“COW 只读”与真正的只读代码段。

---

## 4. #PF 处理 COW

```c
if (int_no == 14 && cr2 in user range && paging_handle_cow(cr2))
    return;   /* iretq 重试原指令 */
```

`paging_handle_cow()`：

- PTE 必须 present + COW；
- `refs = pmm_ref_count(old_phys)`；
- refs > 1：
  - `pmm_alloc_page()` 新页；
  - memcpy 4 KiB；
  - PTE 指向新页，恢复 W，清 COW；
  - `pmm_free_page(old_phys)` 减引用；
- refs == 1：
  - 只恢复 W、清 COW（对方已退出）；
- `invlpg(vaddr)`。

该路径也覆盖内核 `copy_to_user()` 写 COW 页的情况（在 uaccess fixup 之前处理）。

---

## 5. VMA 与 mmap

```c
struct vm_area {
    u64 start, end;
    u32 flags;
    struct vm_area *next;
};
```

- `task->vmas`：按 start 排序的链表；
- `task->mmap_next`：匿名映射起始地址（USER_BASE + 512 KiB）；
- `vma_add` / `vma_find` / `vma_free_all`；
- fork：`vma_copy()` 复制链表，`mmap_next` 继承；
- exec：释放旧 VMA，重置 `mmap_next`。

新增系统调用：

```c
mmap(size) -> user address
```

- 按页分配并 `paging_ensure_user_page`（零页、可写）；
- 登记 `VM_READ | VM_WRITE | VM_ANON`；
- 当前是 eager 分配，但 VMA 已经是逻辑区域的抽象层，为后续 demand paging/file-backed mmap 铺路。

---

## 6. 实测

### 6.1 cowtest

```
[M4] cowtest task: pid=8
[cow] vaddr=0x00000080000fffe0 phys=... refs=2
[cow] vaddr=0x0000008000000210 phys=... refs=2
[cow] vaddr=0x0000008000000210 phys=... refs=1
cowtest: PASS
```

- 子进程写 COW 页 → refs=2，复制；
- 父进程随后写 → refs=1，直接恢复 W；
- 父进程读到的仍是旧值 0x1111，子进程改动没有污染父进程。

### 6.2 mmaptest

```
[M4] mmaptest task: pid=9
[cow] vaddr=0x0000008000080000 refs=2
cowtest: PASS
[cow] vaddr=0x0000008000080000 refs=1
mmaptest: PASS
```

- mmap 8 KiB，写入/校验；
- fork 后 mmap 页同样 COW；
- 子进程写 0xAA，父进程仍看到 0；
- 父进程写 0x55 成功。

### 6.3 回归

- `make test` → **PASS**（含 cowtest/mmaptest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 已知限制

1. mmap 是 eager 分配，没有 demand paging；
2. VMA 目前只用于匿名 mmap，尚未覆盖 ELF/stack，也尚未用于 #PF 范围校验；
3. 没有 `munmap`、`mprotect`、file-backed mmap、共享映射；
4. COW 只处理用户页；内核页仍直接映射；
5. fork 仍需复制页表页，只是不复制数据页；
6. refcount 为 u16（足够当前场景），PMM 仍只管理前 1 GiB；
7. 没有 SMP/锁，COW 依赖单 CPU。
8. `mmap` 失败时已分配的页暂不回滚（极端 ENOMEM 路径）。

---

## 8. 下一步

- **M5-3b**：双超级块 + fsck + journaling；
- VMA 覆盖 ELF/stack + demand paging + `munmap/mprotect`；
- 文件-backed mmap（mmap 读文件）；
- Session/SIGTTIN/SIGTTOU 终端闭环；
- 继续外置 mkdir/rm/touch。

---

## 9. 复现清单

- [x] PMM 每页 refcount，alloc=1，free 归零才释放
- [x] fork 共享物理页 + 清 W + 置 COW
- [x] 父进程 TLB 刷新
- [x] #PF COW 复制/恢复 W/refcount--
- [x] 内核 copy_to_user COW 路径
- [x] VMA 链表、fork/exec 复制与清理
- [x] `mmap(size)` 匿名映射
- [x] cowtest：父子写隔离
- [x] mmaptest：mmap + fork COW
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告