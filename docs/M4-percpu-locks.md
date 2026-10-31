# M4-34 — Per-CPU current、kprintf 锁与 AP idle

> 状态：**并发底座完成并通过实测**（QEMU `-smp 4`；AP 仍不参与用户调度）  
> 日期：2026-10-29  
> 关联文档：[M4-ap-wakeup.md](./M4-ap-wakeup.md) · [design.md](./design.md)

---

## 1. 结果

```
[smp] APs online=3, total CPUs=4, spin counter=300000, percpu ok=3
```

- 3 个 AP 各自设置 GS 基址到自己的 `per_cpu`；
- `task_current()` 从 `%gs:0` 读到的都是自己的 idle task；
- `percpu ok=3` 证明没有串核；
- BSP 继续调度用户任务。

**明确边界：AP 仍只进入 per-CPU idle 并 `hlt`，没有从运行队列取用户任务。**

---

## 2. Per-CPU

`arch/x86_64/percpu.c`：

```c
struct per_cpu {
    u64 current_task;  /* offset 0 = gs:0 */
    u64 apic_id;
    u64 online;
};
```

- 每 CPU 一个 `per_cpu` 块；
- `percpu_init_cpu(apic_id, task)`：
  - 填 current/apic_id；
  - `wrmsr(IA32_GS_BASE, pc)`；
- `task_current()`：

```c
struct task *percpu_current(void)
{
    if (rdmsr(MSR_GS_BASE) == 0)
        return NULL;
    asm("movq %gs:0, %0" : "=r"(t));
    return t;
}
```

- BSP：`scheduler_init()` 设置 `boot_task`；
- AP：`ap_main()` 设置自己的 `ap_idle_task`；
- GS_BASE 未初始化时返回 NULL，避免 uaccess 早期拿到垃圾指针。

---

## 3. kprintf 全局锁

`libk/kprintf.c`：

```c
static spinlock_t kprintf_lock = SPINLOCK_INIT;
...
cli
spin_lock(&kprintf_lock);
vkprintf(...);
spin_unlock(&kprintf_lock);
恢复 RFLAGS
```

- 整行日志原子；
- 与 IRQ 关闭配合，防止 BSP/AP 字符交错。

## 4. 调度器运行队列锁

`sched/scheduler.c`：

```c
static spinlock_t run_queue_lock = SPINLOCK_INIT;
```

- `schedule()` 在 `pick_next`、状态迁移、`current` 更新期间持锁；
- 在 `switch_to` 前释放，避免跨任务持锁；
- 这是全局锁，不是每 CPU 队列锁。

## 5. AP idle

`ap_main()`：

1. 100000 次跨核 spinlock 自增；
2. 初始化 `ap_idle_task` 的 pid/state/cr3/user；
3. `percpu_init_cpu(cpuid_apic_id(), &ap_idle_task)`；
4. 验证 `task_current() == &ap_idle_task`，`ap_percpu_ok++`；
5. `ap_online++`；
6. `hlt`。

没有调用 `schedule()`，因此不会与 BSP 抢同一个用户任务。

---

## 6. 回归

- `make test` → **PASS**（task A=27, B=27）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制（AP 尚未成为调度单元）

1. **AP 不参与用户任务调度**，只 idle/hlt；
2. 运行队列仍是全局单队列，没有 per-CPU ready queue；
3. `run_queue_lock` 只保护 `schedule()`，任务创建/唤醒/回收并非全部加锁；
4. AP 仍使用 boot CR3，没有切到用户页表；
5. 没有 TLB shootdown；
6. 没有 APIC timer/IPI 中断处理，AP 不接收中断；
7. `task_exit_current`/`task_reap` 等仍是 BSP 假设；
8. 没有 CPU 亲和性/负载均衡。

---

## 8. 下一步（M4-35）

1. 每 CPU ready queue + idle 任务；
2. AP 调用 `schedule()`，只在本地队列取任务；
3. 用户页表 CR3 切换与 TLB shootdown IPI；
4. APIC timer 每 CPU 抢占；
5. task_create/wakeup/reap 全锁；
6. CPU 亲和性与负载均衡。

---

## 9. 复现清单

- [x] per_cpu 结构与 GS_BASE
- [x] task_current 从 gs:0 读
- [x] BSP/AP 各自 current
- [x] AP percpu ok=3
- [x] kprintf 全局 spinlock
- [x] schedule 运行队列锁（全局）
- [x] AP idle/hlt
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] AP 进入用户任务调度（M4-35）