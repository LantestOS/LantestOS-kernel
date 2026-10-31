# M4-44 — IPI 0xFE 队列唤醒

> 状态：**已完成并通过实测**（QEMU `-smp 4`；三 AP 不再等下一次 timer）  
> 日期：2026-10-30  
> 关联文档：[M4-multi-ap-users.md](./M4-multi-ap-users.md) · [M4-active-cpus.md](./M4-active-cpus.md)

---

## 1. 结果

三 AP 任务不再等下一次 100 Hz timer tick：

```
[smp] AP user task pid=2 queued on CPU1
[M3b] entering Ring 3: user-apuser pid=2 entry=0x0000008000000000 ...
[proc] exit: pid=2 name=user-apuser cpu=1 status=42 -> ZOMBIE
[smp] AP user task pid=3 queued on CPU2
[M3b] entering Ring 3: user-apuser2 pid=3 ...
[proc] exit: pid=3 name=user-apuser2 cpu=2 status=42 -> ZOMBIE
[smp] AP user task pid=4 queued on CPU3
[M3b] entering Ring 3: user-apuser3 pid=4 ...
[proc] exit: pid=4 name=user-apuser3 cpu=3 status=42 -> ZOMBIE
[reap] orphan: pid=2 ... exit=42
[reap] orphan: pid=3 ... exit=42
[reap] orphan: pid=4 ... exit=42
```

## 2. 实现

- `isr_asm.S` 增加 `isr254`；
- `idt.c`：
  - 增加 `SMP_WAKE_VEC = 0xFE`；
  - `idt_init()` 设置 0xFE gate → `isr254`；
  - `isr_handler` 收到 `int_no == 0xFE` 直接 `schedule()`；
- `percpu.c`：
  - `percpu_enqueue_task()` 记录入队前队列是否为空；
  - 解锁后，如果原本为空且目标 CPU APIC ID 有效，发送
    `lapic_send_ipi(apic_id, 0xFE)`；
- `percpu_apic_id_of(cpu)` 暴露每 CPU APIC ID；
- 增加 `#include <lapic.h>`。

AP 在 idle hlt 中被 IPI 唤醒，立即 `schedule()` 从本地队列取任务，不再等 timer。

## 3. 回归

- `make test` → **PASS**（task A=48, B=47）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

## 4. 当前限制

1. **TLB shootdown 仍未做**；
2. 没有 IPI 0xFD / ack 等待；
3. page_cache/pipe/VFS 未全锁；
4. `active_cpus` 等待是忙等；
5. AP 仍只跑短命任务；
6. 没有 fork 100 + kill 压测。

## 5. 下一步（M4-45）

1. TLB shootdown IPI 0xFD + ack；
2. page_cache/pipe/VFS 锁；
3. 长任务并发；
4. fork 100 + kill 压测。

## 6. 复现清单

- [x] isr254
- [x] IDT 0xFE gate
- [x] IPI handler schedule
- [x] enqueue 空队列发 IPI
- [x] 三 AP status=42
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] TLB shootdown（M4-45）
- [ ] VFS/pipe 锁（M4-45）
