# M4-40 — AP 用户态 smoke test：sys_exit(42)

> 状态：**AP 成功执行用户任务并退出**；隔离直启，不是完整 per-CPU 调度  
> 日期：2026-10-29  
> 关联：[M4-swapgs-asm.md](./M4-swapgs-asm.md) · [M4-ap-timer-iret.md](./M4-ap-timer-iret.md)

---

## 1. 结果

QEMU `-smp 4`：

```
[smp] APs online=3, total CPUs=4, spin counter=300000, percpu ok=3,
      irq=3, timer ticks=0
[smp] AP user smoke task pid=2 launched
[smp] AP user: cpu=3 pid=2 cr3=0x0000000000283000
      tss0=0x000000000029d000 entry=0x0000008000000000
      stack=0x00000080000ffff8
[proc] exit: pid=2 name=user-apuser cpu=3 status=42 -> ZOMBIE
[reap] orphan: pid=2 name=user-apuser exit=42
```

- 3 个 AP online；
- 一个 AP 原子认领 `user-apuser`；
- 该 AP 设置：
  - 自己的 per-CPU current；
  - 自己的 TSS.rsp0；
  - 任务 CR3；
- `user_enter` 进入 Ring3；
- 用户程序只执行 `sys_exit(42)`；
- 内核在 AP 上下文正确记录 `cpu=3 status=42`；
- BSP 的 orphan reaper 回收任务。

这是兰特斯特第一次在 AP 上真正执行 Ring3 用户代码并完成系统调用。

---

## 2. 隔离策略

为了先验证 TSS/CR3/swapgs/syscall 链路，没有直接上 per-CPU 调度器：

1. BSP 创建 `user-apuser`，`state = TASK_STOPPED`；
2. 通过 `smp_launch_ap_user()` 发布任务指针；
3. AP 在 `ap_main` 中：
   - 停止自己的 LVT timer（`lapic_timer_stop`，masked）；
   - 忙等任务发布；
   - 用 `__atomic_exchange_n` **只有一个 AP 认领**；
   - 设置 per-CPU current、`gdt_set_tss_rsp0_cpu`、CR3；
   - `user_enter(entry, stack)`；
4. 其他 AP `hlt`；
5. 用户 `sys_exit(42)`；
6. AP 的 `task_exit_current` 标记 ZOMBIE 后 `hlt`；
7. BSP reaper 回收。

关键修复：之前 3 个 AP 同时抢到同一个 `smp_ap_user`，同一个 pid/CR3/TSS 并发进入，直接崩溃。现在用原子 exchange，只有一个 AP 执行。

---

## 3. 用户程序

```c
int main(...)
{
    __asm__ volatile ("movq $2, %rax; movq $42, %rdi; int $0x80");
    for (;;)
        ;
}
```

没有 `sys_write`、没有 VFS/console，避免在基础链路验证阶段引入额外并发。

---

## 4. 回归

- `make test` → **PASS**（task A=26, B=26；25s 窗口）；
- `make test-persist` → **PASS**（每段 15s 窗口）；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 5. 当前限制（重要）

1. **不是 per-CPU ready queue 调度**：只是隔离直启一个 smoke task；
2. AP timer 在进入用户态前被停掉，没有 AP 抢占；
3. 只有一个 AP 执行用户任务；
4. 没有 CR3/TLB shootdown；
5. 没有多任务/队列/亲和性；
6. AP 用户任务状态 STOPPED，由 BSP 创建并手工发布；
7. 其他 AP 空闲 hlt；
8. `make test`/`test-persist` 的 QEMU 窗口因 AP smoke 增大到 25s/15s。

---

## 6. 下一步（M4-41）

1. 每 CPU ready queue + `rq_lock`；
2. AP timer 恢复，本地 `schedule()`；
3. 多 AP 用户任务亲和；
4. CR3 切换与 TLB shootdown；
5. 进程销毁 active_cpus 确认；
6. 压测：fork 100 + kill + 多 AP 用户任务。

---

## 7. 复现清单

- [x] AP 原子认领唯一用户任务
- [x] AP per-CPU current
- [x] AP TSS.rsp0
- [x] AP 任务 CR3
- [x] user_enter Ring3
- [x] AP 上 int 0x80 sys_exit
- [x] `cpu=3 status=42` + reaper
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] per-CPU ready queue（M4-41）
- [ ] AP 抢占/多任务/TLB（M4-41）