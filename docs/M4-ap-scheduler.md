# M4-41 — AP 本地调度：per-CPU 队列 + AP timer

> 状态：**AP 自动从本地队列取用户任务并调度完成**；TLB shootdown 未做  
> 日期：2026-10-29  
> 关联：[M4-ap-user-smoke.md](./M4-ap-user-smoke.md) · [M4-swapgs-asm.md](./M4-swapgs-asm.md)

---

## 1. 结果

QEMU `-smp 4`：

```
[smp] APs online=3, total CPUs=4, spin counter=300000, percpu ok=3,
      irq=3, timer ticks=4697
[smp] AP user task pid=2 queued on CPU1
[M3b] entering Ring 3: user-apuser pid=2 entry=0x8000000000
[proc] exit: pid=2 name=user-apuser cpu=1 status=42 -> ZOMBIE
[reap] orphan: pid=2 name=user-apuser exit=42
```

这次不是 BSP 手工 `user_enter`：

- BSP 把任务挂到 **CPU1 的 per-CPU ready queue**；
- AP1 的本地 LVT timer 中断触发 `schedule()`；
- AP 本地 `ap_schedule()` 从 CPU1 队列取任务；
- 设置 per-CPU current、TSS.rsp0、CR3；
- `switch_to` 进入用户任务；
- 用户 `sys_exit(42)`；
- AP 切回 per-CPU idle，BSP reaper 回收。

---

## 2. 实现

### 2.1 per-CPU 队列

`struct per_cpu`：

```c
struct task *ready_head;
struct task *idle_task;
spinlock_t rq_lock;
```

- `percpu_enqueue_task(cpu,t)` / `percpu_dequeue_task(cpu)`；
- 每个 AP 有独立 idle task（`ap_idle_tasks[cpu]`）。

`struct task`：

```c
struct task *rq_next;
u32 cpu;
u32 on_rq;
```

### 2.2 调度器

- BSP `pick_next` 只选 `cpu==0` 的任务；
- AP `schedule()` 分派到 `ap_schedule()`；
- `ap_schedule`：
  - 从本地队列取任务；
  - 队列空且当前任务 RUNNING → 保持当前任务；
  - 切走时把 RUNNING 任务重新入队；
  - 设置 `gdt_set_tss_rsp0_cpu(cpu, ...)`；
  - CR3 不同则 `paging_write_cr3`；
  - `switch_to`。

### 2.3 AP timer

- LVT timer 0x31 periodic；
- 中断里 `lapic_eoi()` 后调用 `schedule()`；
- AP1 在 idle 时被 timer 唤醒，取到 queued 用户任务。

### 2.4 任务创建

BSP：

```c
apuser->cpu = 1;
apuser->state = TASK_READY;
percpu_enqueue_task(1, apuser);
```

AP 自动调度，不再手工 launch。

---

## 3. 回归

- `make test` → **PASS**（task A=50, B=50）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 4. 当前限制（重要）

1. 只调度了 **一个 AP 用户任务**（CPU1）；
2. 没有把多个用户任务分到 CPU1/2/3；
3. 没有 work stealing；
4. **没有 TLB shootdown**：AP 切 CR3 后，BSP 改页表没有 IPI 刷新；
5. 没有 `active_cpus` 退出确认；
6. AP timer 与 BSP PIT 独立，没有统一 tick；
7. AP 用户任务退出后 AP 回 idle，未参与更多任务；
8. 还没有 fork 100 + kill 压测。

---

## 5. 下一步（M4-42）

1. 多任务亲和到 AP1/2/3 的本地队列；
2. IPI 0xFE 唤醒 AP 队列；
3. TLB shootdown IPI 0xFD；
4. `active_cpus` 位图与 reap 等待；
5. 多 AP 用户任务并发；
6. fork 100 + kill 压测；
7. 页缓存/VFS/pipe 全锁。

---

## 6. 复现清单

- [x] per-CPU ready queue + rq_lock
- [x] task cpu/rq_next/on_rq
- [x] BSP pick 跳过 AP 亲和任务
- [x] AP 本地 ap_schedule
- [x] AP timer 触发 schedule
- [x] TSS.rsp0 per CPU
- [x] CR3 切换
- [x] AP 自动取 user-apuser
- [x] cpu=1 status=42 + reaper
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] TLB shootdown（M4-42）
- [ ] 多 AP 并发（M4-42）