# M4-16 — SIGSEGV：用户态故障不再杀死内核

> 状态：**已完成并通过实测**（QEMU + segvtest）  
> 日期：2026-10-28  
> 关联文档：[M4-job-control.md](./M4-job-control.md) · [M4-demand-paging.md](./M4-demand-paging.md)

---

## 1. 目标

用户态野指针或权限违规不能触发内核 panic。处理顺序：

1. COW 页 → 复制解决；
2. VMA 内 not-present → demand 分配；
3. 其余用户态 #PF → 向当前任务发 **SIGSEGV（11）**，默认终止，退出码 139；
4. 打印 `[segv]` 现场，内核和其他进程继续运行；
5. 新增 `segvtest` 故意访问 `0x200000000` 验证。

---

## 2. SIGSEGV 路径

`isr_handler()` 在 COW / demand 均失败后：

```c
if (regs->int_no == 14 && (regs->cs & 3u) == 3u) {
    u64 cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    kprintf("[segv] pid=%lu name=%s cr2=%p err=%lx\n",
            task_current()->pid, task_current()->name,
            (void *)cr2, regs->err_code);

    signal_send(task_current(), SIGSEGV);
    signal_deliver_pending(regs);
    return;
}
```

- 只对 **CPL=3** 的 #PF 做 SIGSEGV；
- 内核态 `copy_to_user` 的 fault 仍走原 `.ex_table` fixup / panic 逻辑；
- `signal_deliver_pending()` 默认动作终止当前任务，状态 139。

---

## 3. segvtest

```c
sys_write_str("segvtest: about to fault\n");
*(volatile u64 *)0x200000000ULL = 0xDEADBEEFULL;
sys_write_str("segvtest: FAIL survived\n");
```

- 地址在用户地址空间之外；
- 触发 #PF，err=6（present=0, write=1, user=1）；
- SIGSEGV 杀死该任务；
- 内核继续调度 Shell/其他任务。

---

## 4. 实测

```
[M4] segvtest task: pid=10
segvtest: about to fault
[segv] pid=10 name=user-segvtest cr2=0x0000000200000000 err=6
[signal] pid=10 name=user-segvtest signal=11 -> terminate (status=139)
[proc] exit: pid=10 name=user-segvtest status=139 -> ZOMBIE
Lantest shell (M4-13) - type 'help'
[reap] orphan: pid=10 name=user-segvtest exit=139
```

- 没有 CPU exception / panic；
- Shell 正常启动；
- 退出码 139；
- `make test` 中的 `segvtest` 检查通过。

---

## 5. 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. 没有用户态 SIGSEGV handler / `sigaction`；
2. SIGSEGV 只有默认终止；
3. 没有 SIGBUS/SIGILL/SIGFPE；
4. 权限违规与野指针都走同一 SIGSEGV，没有更细的 si_code；
5. VMA 权限尚未真正驱动 PTE 读/写/执行保护；
6. 内核态页错误仍 panic/fixup，不做内核故障恢复；
7. 没有 core dump。

---

## 7. 下一步

- **file-backed mmap**：VMA 绑定 fd/offset，#PF 从 LantestFS 读页；
- **ELF 按需加载**：PT_LOAD 只建 VMA，#PF 读文件；
- `munmap/mprotect`；
- 用户态信号处理函数；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 8. 复现清单

- [x] SIGSEGV（11）默认终止，退出码 139
- [x] 用户态 #PF 不再 panic
- [x] `[segv]` 打印 pid/name/cr2/err
- [x] 内核 copy_to_user fault 仍走 fixup
- [x] segvtest 故意野指针，仅自身死亡
- [x] Shell/其他任务继续运行
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告