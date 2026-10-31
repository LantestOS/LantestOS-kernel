# M4-39 — GS/swapgs 修正（AP 用户调度基础）

> 状态：**swapgs 正确性完成**；AP 用户任务调度仍回滚、未启用  
> 日期：2026-10-29  
> 关联文档：[M4-ap-timer-iret.md](./M4-ap-timer-iret.md) · [design.md](./design.md)

---

## 1. 背景

M4-39 尝试让 AP 跑 `user-apuser`：

- AP 任务已进入 Ring3（`cpu=1`）；
- 但第一次用户态中断/返回路径出现 `CPU EXCEPTION 13`，`ss=0`；
- 随后 `kmalloc: corrupt block header`；
- 已回滚到 M4-38 稳定基线，没有保留半成品。

复查后确认一个必须先修的正确性问题：**per-CPU GS 与用户态切换没有 swapgs**。

---

## 2. 问题

内核用 GS 做 per-CPU：

```c
task_current() -> mov %gs:0
```

但 `user_enter` 直接 `iretq` 进入用户态，没有交换 GS base；从用户态进入内核时，ISR 也没有 `swapgs`。

结果：

- 用户态运行时 GS base 可能仍是内核 per_cpu；
- 中断/系统调用进来后，`%gs:0` 语义错乱；
- `task_current()` 可能拿到野指针；
- 后续 #GP / 栈踩坏。

这正是 AP 用户任务进 Ring3 后崩溃的高概率根因之一。

---

## 3. 修复

### 3.1 初始化

`percpu_init_cpu()`：

```c
wrmsr(IA32_GS_BASE, per_cpu_ptr);
wrmsr(IA32_KERNEL_GS_BASE, 0);
```

- 内核态 GS base = per_cpu；
- KERNEL_GS_BASE = 0（用户 GS）。

### 3.2 进入用户态

`user_enter` / `user_enter_regs` 在 `iretq` 前：

```asm
swapgs      /* kernel per_cpu -> user GS=0 */
movw $0x23, %ax
...
iretq
```

### 3.3 中断/系统调用入口

`isr_handler` 包装：

```c
int from_user = (regs->cs & 3u) == 3u;
if (from_user)
    swapgs();
isr_dispatch(regs);
if (from_user)
    swapgs();
```

- Ring3 → Ring0：切到内核 GS base；
- 返回 Ring3 前：切回用户 GS；
- Ring0 内部中断（AP timer/idle）不 swapgs。

---

## 4. 验证

- `make test` → **PASS**（task A=17, B=17）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

swapgs 没有破坏现有 BSP 用户任务、fork、Lua、shell。

---

## 5. 当前限制

1. **AP 本地 ready queue 未提交**：M4-39 的队列/亲和性代码在崩溃尝试中已回滚；
2. AP 仍不进入用户任务调度；
3. 没有跨核 TLB shootdown；
4. 每 CPU TSS 已存在，但 `rsp0` 只在 BSP 调度时更新；
5. AP timer 只计数，不驱动用户调度；
6. `user-apuser` 未提交。

---

## 6. 下一步（M4-40）

1. 以协作式方式重开 AP 用户任务：先关 AP timer 抢占；
2. 每 CPU ready queue + 亲和性；
3. AP `schedule()` + 每 CPU `tss.rsp0`；
4. CR3 切换；
5. 打开 AP timer 抢占；
6. TLB shootdown；
7. 最后做 fork 100 + kill 压测。

---

## 7. 复现清单

- [x] KERNEL_GS_BASE=0
- [x] user_enter/user_enter_regs swapgs
- [x] ISR 用户态入口/出口 swapgs
- [x] BSP 用户态回归 PASS
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] AP 本地队列（M4-40）
- [ ] AP 用户任务调度（M4-40）