# M4-17 — File-backed VMA 与 ELF 按需加载

> 状态：**已完成并通过实测**（QEMU + maptest + 惰性 ELF 外部命令）  
> 日期：2026-10-28  
> 关联文档：[M4-demand-paging.md](./M4-demand-paging.md) · [M4-sigsegv.md](./M4-sigsegv.md)

---

## 1. 目标

1. VMA 支持 backing file（file、file_offset、file_data_end）；
2. `mmap_file(fd, offset, length)`：只建 VMA，不分配物理页；
3. `#PF` demand 时从文件读 4 KiB 页；短读/EOF 部分清零；
4. execve 改为 **PT_LOAD → VMA**：不映射物理页，只登记文件区间；
5. 代码段只读，数据/BSS 可写；BSS 页零填，不读文件；
6. 用户 ELF 的 data 段按页对齐，避免 text/data 共享页导致代码段被迫可写。

---

## 2. VMA 扩展

```c
struct vm_area {
    u64 start, end;
    u32 flags;          /* VM_READ/WRITE/EXEC/ANON/FILE */
    struct file *file;  /* holds a file reference */
    u64 file_offset;    /* file offset corresponding to start */
    u64 file_data_end;  /* absolute file offset where valid data ends */
    struct vm_area *next;
};
```

- `vma_add_file()`：`file_get()` 持有文件；
- `vma_free_all()`：`file_close()`、释放 VMA；
- fork：`vma_copy()` 对 file-backed VMA 再 `file_get()`；
- exec：先释放旧 VMA，再建立新 VMA。

### 2.1 重叠合并

- ELF 的 PT_LOAD 可能共享页；
- 相邻/重叠且 **同一 file** 的 VMA 合并：end/flags/file_offset/file_data_end 取并集；
- file_offset 只由**真正携带文件数据**的段参与合并（BSS-only 段不贡献 offset），避免把 text 页读到错误偏移。

---

## 3. file-backed mmap

新增 `sys_mmap_file(fd, offset, length)`：

- 从 fd 取 file，只登记 VMA（`VM_READ|VM_WRITE|VM_FILE`）；
- 不分配页；
- 首次访问触发 demand。

`maptest`：

```
mmap_file(/maptest.txt)
→ 首次读触发 [demand] flags=b
→ 比较文件内容与 EOF 后的零填充
maptest: PASS
```

读取路径：

```c
vfs_read_at(v->file, foff, phys, n)
```

`vfs_read_at()` 保存/恢复 `f_pos`，在 `#PF` 里安全地读取指定偏移。

---

## 4. ELF 按需加载

### 4.1 execve

- 读取/校验 ELF 头；
- `elf_load_vmas()`：
  - 每个 PT_LOAD 建一个 file-backed VMA；
  - `file_offset` 对应段起始页；
  - `file_data_end = p_offset + p_filesz`；
  - 权限来自 `p_flags`：R / W / X；
- 不调用 `paging_ensure_user_page` 映射任何代码/数据页；
- 用户栈仍分配一页并登记匿名 VMA。

### 4.2 #PF demand

```c
if (v->file) {
    foff = v->file_offset + (page - v->start);
    if (foff < v->file_data_end)
        vfs_read_at(v->file, foff, page, min(4096, end - foff));
    /* 剩余字节保持 0（短读/BSS/EOF） */
}
```

- 代码页：VMA 无 VM_WRITE，PTE 清 W；
- 数据/BSS：W；
- BSS 页：foff >= file_data_end，直接零页；
- 首次取指也会触发 demand，从 `/bin/xxx` 读代码页。

### 4.3 用户链接脚本

data 段改为页对齐：

```ld
. = ALIGN(4096);
.data : { ... } :data
```

- text 页与 data 页不重叠；
- 代码段保持只读；
- 数据/BSS 可写；
- 代价是 ELF 文件略大。

---

## 5. 实测

惰性 ELF 加载日志：

```
[elf-lazy] vaddr=0x8000000000 off=4096 filesz=12234 memsz=12234
[elf-lazy] vaddr=0x8000003000 off=4096 filesz=0 memsz=4132
[exec] pid=12 -> /bin/sh argc=1 env=0 entry=0x80000022b0
Lantest shell (M4-13) - type 'help'
```

- Shell 自身也是惰性加载；
- 外部命令：

```
[elf-lazy] /bin/echo ...
[elf-lazy] /bin/cat ...
pipe-ok
```

- `echo hello | cat`、`ls | cat`、`cat /hello.txt | cat`、`echo saved > /x.txt`、`cat /x.txt` 全部正常；
- `maptest` file-backed mmap PASS；
- 代码段 demand flags = 0x19（R+X+FILE），数据/BSS = 0x1b（R+W+FILE）；
- `make test` / `make test-persist` / `make test-exception` PASS；
- 构建零警告。

---

## 6. 踩坑记录

| 问题 | 处理 |
| --- | --- |
| exec 切换 CR3 后又 `vma_free_all(t)`，把新 VMA 删光 | 删除切换后的 VMA 清理，只在建新 VMA 前清旧 |
| BSS-only 段把 file_offset 拉到 0，text 页读到 ELF 头 | BSS 段 `file_data_end = file_offset`；合并时只让有文件数据的段贡献 offset |
| text/data 共享页导致代码段被迫可写 | user.ld data 段 4096 对齐；VMA 只合并真正重叠的段 |
| 持久化只比较 size，布局变但大小相同时不更新 | install_binary 增加内容逐字节比较 |
| maptest 在栈上放 4 KiB buffer，超过单页用户栈 | 直接读映射地址，避免大栈 |

---

## 7. 已知限制

1. execve 仍先把整个 ELF 读入临时缓冲做校验（页映射已惰性，I/O 尚未完全惰性）；
2. 没有 Page Cache：每次 demand 都走文件系统；
3. file-backed 页是私有副本，没有写回/共享映射；
4. 没有 `munmap/mprotect`；
5. 用户栈仍只有一页；
6. VMA 权限只区分 W，未实现 NX；
7. 没有按页拆分/合并 VMA，只有整段合并；
8. 单 CPU，#PF 与文件系统无锁；
9. 文件系统仍是 LantestFS v1（无 journaling）。

---

## 8. 下一步

- **Page Cache**：按 (inode, offset) 缓存 4 KiB 页，多个进程共享；
- `munmap/mprotect`；
- ELF 头/程序头也按需读，完全惰性 execve；
- 用户栈动态扩展；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 9. 复现清单

- [x] VMA backing file / offset / data_end
- [x] file-backed `mmap_file`
- [x] `vfs_read_at` + demand 文件页读取
- [x] 短读/EOF 零填充
- [x] ELF PT_LOAD → VMA，不 eager 映射
- [x] BSS 页零填
- [x] 代码段只读、数据段可写
- [x] 惰性加载 Shell 与 /bin/ls/cat/echo/head
- [x] 管道/重定向仍正常
- [x] maptest PASS
- [x] 二进制内容校验
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告