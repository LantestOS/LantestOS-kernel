# M4-38 — 同环 iret、AP 定时器心跳

> 状态：**AP 异常返回与 APIC timer 完成**；AP 仍未进入用户调度  
> 日期：2026-10-29  
> 关联文档：[M4-percpu-tss-ap-idt.md](./M4-percpu-tss-ap-idt.md) · [design.md](./design.md)

---

## 1. 结果

```
[smp] AP vector 0x30 handled, apic id=1
[smp] AP vector 0x30 handled, apic id=2
[smp] AP vector 0x30 handled, apic id=3
[smp] APs online=3, total CPUs=4, spin counter=300000,
      percpu ok=3, irq=3, timer ticks=10562
```

- AP 的 `int $0x30` 现在可以正常返回，不再 #GP；
- AP LVT timer 周期触发 0x31，10k+ tick；
- kprintf 仍无交错；
- AP 仍在 idle/hlt 循环中被 timer 唤醒，**不取用户任务**。

---

## 2. 真正的 #GP 根因：CS 还是 0x18

之前判断是“同环 iret 栈”问题，但根因更基础：

- AP trampoline 的临时 GDT 中，`0x18` 是 64 位内核代码；
- 加载内核 GDT 后，内核的 `0x18` 是**用户代码段（DPL 3）**；
- AP 的 CS 没有重新加载，仍然是 `0x18`；
- `int $0x30` 时 CPU 压入的 CS 是“DPL3 选择子”，`iretq` 返回时触发 #GP。

修复：`gdt_load_cpu()` 最后用 far return 重载 CS：

```c
pushq $0x08
leaq 1f(%rip), %rax
pushq %rax
lretq
1:
```

之后：

- AP CS = 0x08；
- `int $0x30` 进入/返回正常；
- 同环 iret 不再是问题。

---

## 3. APIC Timer

`lapic_timer_start(vector, count)`：

- LVT Timer MSR `0x832`：
  - vector = 0x31；
  - bit17 = 1（periodic）；
- Divide Configuration `0x83E` = 0x0B（divide by 1）；
- Initial Count `0x838` = 1000000。

AP 定时器中断处理：

```c
if (regs->int_no == 0x31) {
    smp_timer_ticks++;
    lapic_eoi();   /* x2APIC MSR 0x80B = 0 */
    return;
}
```

没有 EOI 的话只跳一次，这是最容易漏的坑。

---

## 4. 当前限制（AP 仍未调度用户）

1. **AP 本地 ready queue 未实现**；
2. AP 不调用 `schedule()`，只 hlt + timer 唤醒；
3. AP 仍用 boot CR3；
4. 没有 CR3 切换；
5. 没有 TLB shootdown；
6. 每 CPU TSS 的 `rsp0` 没有在 AP 调度时更新；
7. AP timer 只计数，不触发调度；
8. 没有 IPI 唤醒 AP 队列。

---

## 5. 回归

- `make test` → **PASS**（task A=20, B=20）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 下一步（M4-39）

1. 每 CPU ready queue + `rq_lock`；
2. AP 亲和性（写死 target_cpu）；
3. AP 本地 `schedule()`；
4. `tss.rsp0 = next->kernel_stack_top` per CPU；
5. CR3 切换；
6. TLB shootdown IPI 0xFD 与进程退出确认；
7. 用户任务在 AP 上真实执行。

---

## 7. 复现清单

- [x] AP CS far-return 到 0x08
- [x] 0x30 异常返回不再 #GP
- [x] AP LVT timer 0x31 periodic
- [x] AP timer EOI 0x80B
- [x] timer ticks > 0
- [x] kprintf 无交错
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] AP ready queue（M4-39）
- [ ] AP 用户调度/CR3/TLB（M4-39）