# M4-37 — Per-CPU TSS、AP 的 IDT 与 AP 异常测试

> 状态：**Per-CPU TSS/AP IDT 完成并通过实测**；APIC timer 与同环 iret 留 M4-38  
> 日期：2026-10-29  
> 关联文档：[M4-percpu-locks.md](./M4-percpu-locks.md) · [design.md](./design.md)

---

## 1. 结果

QEMU `-smp 4`：

```
[smp] AP vector 0x30 handled, apic id=1
[smp] AP vector 0x30 handled, apic id=2
[smp] AP vector 0x30 handled, apic id=3
[smp] APs online=3, total CPUs=4, spin counter=300000, percpu ok=3, irq=3
```

- 3 个 AP 各自加载了内核 GDT（含自己的 TSS）和共享 IDT；
- 各自执行 `ltr` 本核 TSS 选择子；
- 在 AP 上执行 `int $0x30`，进入自己的异常上下文；
- `percpu_apic_id()` 正确返回 1/2/3；
- kprintf 全局锁保证三行输出没有交错。

---

## 2. Per-CPU TSS

GDT 布局扩展：

```
0x00 null
0x08 kernel code
0x10 kernel data
0x18 user code
0x20 user data
0x28 + cpu*0x10 : TSS for CPU cpu
```

- `GDT_MAX_CPUS = 8`；
- `static struct tss64 tss[8]`；
- `static u8 tss_stacks[8][8192]`；
- GDT 共 21 项；
- `gdt_init()` 填所有 TSS 描述符，BSP `ltr 0x28`；
- `gdt_init_cpu(cpu)` 重新 `lgdt` 并 `ltr 0x28+cpu*0x10`。

### 踩坑：GS base 被段重载清掉

`gdt_load_cpu()` 最初会 `mov %ax, %gs`，AP 的 per-CPU GS base 被清成 0，导致：

- `percpu_apic_id()` 返回 0；
- `int $0x30` 里打印 `apic id=0`。

修复：**不再重载 `%gs`**，保留 `IA32_GS_BASE`。

---

## 3. AP 加载 IDT

- `idt_reload()` 暴露 `lidt idtr`；
- AP `ap_main` 在 `gdt_init_cpu` 后调用 `idt_reload()`；
- IDT 是全系统共享的；
- 3 个 AP 都能进入同一个异常分发器。

---

## 4. 向量 0x30 AP 自测

- `isr_asm.S` 增加 `isr48`；
- `isr_stub_table` 必须加 `.quad isr48`（否则 IDT 第 48 项读到表外垃圾，`int $0x30` 直接崩）；
- `idt_init()` 填 49 个 gate；
- `isr_handler` 对 `int_no==0x30`：
  - `smp_test_irq_count++`；
  - `kprintf("[smp] AP vector 0x30 handled, apic id=%u")`；
  - 目前直接 `hlt`（见限制）。

---

## 5. 当前限制（重要）

1. **同环 iret 未修**：`int $0x30` 从 Ring0 触发，`isr_common` 的 `iretq` 假定有 RSP/SS，返回会 #GP；所以 0x30 处理器目前打印后 `hlt`，不返回；
2. **APIC timer 未启用**：AP 没有本地定时器心跳，不能抢占；
3. AP 还未进入用户任务调度；
4. 每 CPU TSS 已建立，但 `rsp0` 仍只有 BSP 调度时更新；
5. 没有 AP 的 `schedule()`/就绪队列；
6. 没有 TLB shootdown。

---

## 6. 回归

- `make test` → **PASS**（task A=22, B=22）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 下一步（M4-38）

1. 修 `isr_common` 同环 iret（Ring0 中断/异常返回）；
2. AP LVT timer（0x31）+ 周期 tick，处理器返回；
3. AP 本地 ready queue + idle；
4. AP `schedule()`；
5. CR3 切换 + TLB shootdown；
6. 用户任务亲和性。

---

## 8. 复现清单

- [x] GDT 21 项，8 个 TSS
- [x] BSP `ltr 0x28`
- [x] AP `gdt_init_cpu` + `ltr` 自己的 TSS
- [x] 保留 GS base
- [x] AP `idt_reload`
- [x] isr48 + stub table
- [x] AP `int $0x30`，APIC ID 1/2/3
- [x] kprintf 无交错
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] APIC timer（M4-38）
- [ ] AP 用户调度（M4-38）