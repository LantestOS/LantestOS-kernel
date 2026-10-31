# M4-33 — AP 唤醒成功：SIPI trampoline + 跨核自旋锁

> 状态：**已完成并通过实测**（QEMU `-smp 4`；AP online=3 + 跨核计数）  
> 日期：2026-10-29  
> 关联文档：[M4-smp-foundation.md](./M4-smp-foundation.md) · [design.md](./design.md)

---

## 1. 结果

QEMU `-smp 4`：

```
[smp] MADT: 4 CPU(s), BSP APIC ID 0, x2APIC=yes
[smp]   CPU0 APIC ID 0 (BSP)
[smp]   CPU1 APIC ID 1
[smp]   CPU2 APIC ID 2
[smp]   CPU3 APIC ID 3
[smp] AP APIC ID 1 online
[smp] AP APIC ID 2 online
[smp] AP APIC ID 3 online
[smp] APs online=3, total CPUs=4, spin counter=300000
```

- 3 个 AP 从实模式一路进入 64 位长模式；
- 每个 AP 在 `ap_main` 中执行 100000 次 `spin_lock`/`spin_unlock` 自增；
- 总计数精确为 300000，证明跨核原子锁没有丢更新；
- AP 完成后 `hlt` 停住。

**这是第一次真正有多核同时执行内核代码。**  
但 AP 还没有进入调度器，BSP 仍是唯一调度任务的 CPU。

---

## 2. 低端 trampoline（0x6000）

`arch/x86_64/ap_trampoline.S`，链接到 `. = 0x6000`：

- SIPI 向量 0x06 → CS:IP = 0x6000:0；
- 16 位实模式：
  - `cli`、清段寄存器；
  - 加载临时 GDT；
  - 置 CR0.PE；
  - 远跳 32 位保护模式。
- 32 位：
  - 加载数据段；
  - CR4.PAE=1；
  - 从 0x7000 读 BSP CR3 写 CR3；
  - EFER.LME=1；
  - CR0.PG=1；
  - 远跳 64 位。
- 64 位：
  - 从 0x7008 读 AP 栈顶；
  - 从 0x7010 读 entry；
  - `call *entry`，进入 `ap_main`。

GDT 描述符：

| selector | 描述 |
| --- | --- |
| 0x08 | 32 位代码，base 0，limit 4G |
| 0x10 | 数据 |
| 0x18 | 64 位代码（L=1） |

---

## 3. BSP/AP 参数

`0x7000`：

```c
struct ap_boot_params {
    u64 cr3;
    u64 stack_top;
    u64 entry;
};
```

- BSP 在 `start_one_ap()` 写入；
- `__sync_synchronize()` 后发 IPI；
- AP 在 64 位最后读取；
- 每个 AP 有独立 8 KiB 栈（`ap_stacks[AP_MAX][8192]`）。

---

## 4. INIT/SIPI 时序

`arch/x86_64/smp.c`：

1. INIT assert：`0x00004500`；
2. 忙等 ~10ms；
3. INIT de-assert：`0x00008500`；
4. 忙等 ~200us；
5. SIPI：`0x00000606`（vector 0x06）；
6. 忙等；
7. 第二次 SIPI；
8. 轮询 `ap_online`。

全部通过 x2APIC MSR ICR（0x830）发送。

---

## 5. AP 入口与自旋锁

```c
void ap_main(void)
{
    for (u32 i = 0; i < 100000u; ++i) {
        spin_lock(&ap_test_lock);
        ap_test_counter++;
        spin_unlock(&ap_test_lock);
    }
    __atomic_add_fetch(&ap_online, 1, __ATOMIC_SEQ_CST);
    for (;;)
        __asm__ volatile ("hlt");
}
```

- 3 个 AP 并发抢同一把锁；
- 计数 300000 完全正确；
- 说明 `lock cmpxchg` 自旋锁在真实多核下工作。

---

## 6. 回归

- `make test` → **PASS**（task A=26, B=26；多核下计数正确）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制（AP 还未成调度单元）

1. AP 进入 `ap_main` 后只做锁测试然后 `hlt`，**没有进入 scheduler**；
2. `task_current` 仍是全局变量，不是 per-CPU；
3. 没有每 CPU idle 任务/运行队列；
4. AP 仍用 BSP 的 boot CR3（identity），没有切到用户页表；
5. `kprintf` 没有全局锁（AP 当前不打印，所以没暴露）；
6. VFS/pipe/page cache 尚未全锁；
7. 没有 TLB shootdown；
8. 没有 APIC timer/IOAPIC，AP 不接收中断；
9. 用户进程仍只在 BSP 上调度。

---

## 8. 下一步（M4-34）

1. per-CPU `current`（GS 基址或数组 + `apic_id` 索引）；
2. 每 CPU idle 任务；
3. 调度器运行队列锁，让 AP 参与 `schedule()`；
4. `kprintf` 全局 spinlock；
5. 页缓存/VFS/pipe 全锁；
6. TLB shootdown IPI；
7. APIC timer/IPI 中断处理。

---

## 9. 复现清单

- [x] 0x6000 低端 AP trampoline
- [x] 16→32→64 位切换
- [x] GDT/CR3/EFER/CR0
- [x] 0x7000 参数页
- [x] INIT + deassert + 双 SIPI
- [x] AP online=3
- [x] 跨核 spinlock 计数 300000
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] AP 进入 scheduler（M4-34）