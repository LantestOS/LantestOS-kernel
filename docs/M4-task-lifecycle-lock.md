# M4-35 — 任务生命周期锁（AP 用户调度仍关闭）

> 状态：**部分完成**：任务链表并发锁已加；AP 仍未进入用户任务调度  
> 日期：2026-10-29  
> 关联文档：[M4-percpu-locks.md](./M4-percpu-locks.md) · [design.md](./design.md)

---

## 1. 这轮做了什么

在 `sched/scheduler.c` 中，任务链表的插入/移除/计数现在使用 `run_queue_lock`：

- `task_alloc()` 把新任务挂到全局链表时 `spin_lock_irqsave`；
- `task_reap()` 递减 `total_tasks` 时加锁；
- `task_count()` 读取时加锁。

这样在 SMP 下，BSP/AP 同时创建/回收任务不会再把 `task_head`/`total_tasks` 撕裂。

验证：

- `make test` → **PASS**（task A=27, B=27）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 2. 为什么没有硬上 AP 用户调度

要让 AP 真正跑用户任务，至少还缺：

1. **每 CPU ready queue**：现在是全局单队列，AP 取任务会与 BSP 抢同一个 `task`；
2. **AP 的 IDT/GDT**：AP 当前没加载 IDT，收不到 IRQ/IPI；
3. **APIC timer**：AP 没有本地定时器中断，无法抢占；
4. **用户 CR3 切换**：AP 仍用 boot CR3；
5. **TLB shootdown**：BSP 改页表后 AP TLB 仍可能命中旧项；
6. **进程销毁等待**：AP 还在跑该进程时，BSP 不能销毁页表。

在这些条件同时满足前直接让 AP `schedule()`，最可能的结果是：

- 两个 CPU 同时运行同一个用户任务；
- AP 用错 CR3；
- TLB 脏页；
- 任务链表/僵尸回收竞争；
- Triple Fault / 随机 #PF。

所以这轮只做“不会把系统打崩”的生命周期锁，不伪装 AP 已经参与调度。

---

## 3. 当前状态

- AP online=3；
- AP per-CPU current 正确（`percpu ok=3`）；
- AP 跑完 100000 次跨核 spinlock 后进入 per-CPU idle/hlt；
- BSP 仍是唯一调度用户任务的 CPU；
- 任务创建/回收计数已加锁。

---

## 4. 下一步（M4-36）

1. 每 CPU ready queue + idle task；
2. AP 加载 IDT、启用 x2APIC LVT timer；
3. AP 本地 `schedule()`，只取本地队列；
4. 用户 CR3 切换；
5. TLB shootdown IPI（0xFD）与进程退出确认；
6. 把用户任务亲和到 AP 做真实多核执行。

---

## 5. 复现清单

- [x] task_alloc 插入链表加锁
- [x] task_reap 计数加锁
- [x] task_count 加锁
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] per-CPU ready queue
- [ ] AP IDT/APIC timer
- [ ] AP 用户任务调度
- [ ] TLB shootdown