# M4-42 — 三个 AP 并发执行用户任务

> 状态：**CPU1/2/3 同时从本地队列取任务并完成 sys_exit(42)**  
> 日期：2026-10-30  
> 关联：[M4-ap-scheduler.md](./M4-ap-scheduler.md)

---

## 1. 结果

QEMU `-smp 4`：

```
[smp] AP user task pid=2 queued on CPU1
[M3b] entering Ring 3: user-apuser pid=2
[smp] AP user task pid=3 queued on CPU2
[M3b] entering Ring 3: user-apuser2 pid=3
[smp] AP user task pid=4 queued on CPU3
[M3b] entering Ring 3: user-apuser3 pid=4
[proc] exit: pid=3 name=user-apuser2 cpu=2 status=42
[proc] exit: pid=2 name=user-apuser cpu=1 status=42
[proc] exit: pid=4 name=user-apuser3 cpu=3 status=42
[reap] orphan: pid=2 ... 3 ... 4
```

- BSP 创建 3 个 apuser 任务；
- 分别挂到 CPU1/2/3 的 per-CPU ready queue；
- 每个 AP 的本地 LVT timer 触发自己的 `schedule()`；
- 每个 AP 取本地任务、设置自己的 TSS.rsp0、CR3，进入 Ring3；
- 三个 AP 都完成 `sys_exit(42)`；
- BSP reaper 回收三个僵尸。

这是兰特斯特第一次在 3 个 AP 上并发运行用户代码。

---

## 2. 实现

- `kernel.c`：一个 apuser ELF 复制成 3 个任务，`cpu=1/2/3`，分别 `percpu_enqueue_task`；
- 每个 AP 有独立 `ap_idle_tasks[cpu]`；
- AP timer 0x31 中断里 `lapic_eoi()` 后 `schedule()`；
- `ap_schedule` 从本地队列取任务；
- BSP `pick_next` 跳过 `cpu!=0` 的任务；
- 用户程序只执行 `sys_exit(42)`，避免 VFS/console 并发。

---

## 3. 回归

- `make test` → **PASS**（task A=48, B=48）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 4. 当前限制（仍然关键）

1. **没有 TLB shootdown**：BSP 改页表后 AP TLB 未刷新；
2. **没有 active_cpus 退出确认**：reap 不等待 AP 离开地址空间；
3. 没有 IPI 0xFE 唤醒（AP timer 周期运行，队列任务靠 tick 取走）；
4. 没有 work stealing/负载均衡；
5. 每个 AP 只跑一个短命任务，没有长时间并发；
6. VFS/pipe/page cache 未全锁；
7. 没有 fork 100 + kill 压测。

**当前多 AP 并发是“安全短任务”级别，不是完整的 SMP 用户态运行时。**

---

## 5. 下一步（M4-43）

1. TLB shootdown IPI 0xFD + CR3 重载；
2. `active_cpus` 位图/计数，reap 等待；
3. IPI 0xFE 唤醒 AP 队列；
4. 长任务/多任务并发；
5. fork 100 + kill 压测；
6. VFS/page cache/pipe 全锁。

---

## 6. 复现清单

- [x] CPU1/2/3 各自本地队列
- [x] 三个 AP timer 本地 schedule
- [x] 三个 AP 进入 Ring3
- [x] cpu=1/2/3 status=42
- [x] BSP reaper 回收
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] TLB shootdown（M4-43）
- [ ] active_cpus（M4-43）
- [ ] fork 100 压测（M4-43）