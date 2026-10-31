# M4-32 — SMP 地基：MADT、x2APIC、自旋锁

> 状态：**已完成并通过实测**（QEMU `-smp 4`；AP SIPI 尚未实现）  
> 日期：2026-10-29  
> 关联文档：[design.md](./design.md) · [M4-math-repl-multiline.md](./M4-math-repl-multiline.md)

---

## 1. 结果

- QEMU 以 `-smp 4` 启动；
- 从 Multiboot2 ACPI tag / 低内存 RSDP 扫描找到 ACPI RSDP；
- 解析 RSDT/XSDT 找到 MADT；
- 枚举 4 个 Local APIC：

```
[smp] MADT: 4 CPU(s), BSP APIC ID 0, x2APIC=yes
[smp]   CPU0 APIC ID 0 (BSP)
[smp]   CPU1 APIC ID 1
[smp]   CPU2 APIC ID 2
[smp]   CPU3 APIC ID 3
```

- x2APIC 通过 MSR 启用（避免 LAPIC MMIO 0xFEE00000 超出当前 1 GiB 恒等映射）；
- 自旋锁 `spinlock.h` 已加入；
- PMM 分配/释放/refcount 已加 `spin_lock_irqsave`；
- `page_cache_phys` 查询已加锁。

**明确边界：AP 还没有真正启动，BSP 仍是唯一执行 CPU；`smp_cpu_count()` 返回 1。**

---

## 2. ACPI/MADT

`arch/x86_64/acpi.c`：

- `find_rsdp()`：
  - Multiboot2 tag 14/15；否则扫描 `0xE0000..0xFFFFF` 的 `"RSD PTR "`；
  - 校验和；
- `find_madt()`：
  - revision≥2 用 XSDT，否则 RSDT；
  - 按 `"APIC"` 签名找 MADT；
- 解析：
  - type 0 Local APIC；
  - type 9 x2APIC；
  - 去重、记录 APIC ID；
- BSP APIC ID 用 CPUID.1 EBX[31:24]。

## 3. x2APIC

`arch/x86_64/lapic.c`：

- CPUID.1:ECX[21] 检查；
- IA32_APIC_BASE MSR bit10 启用 x2APIC；
- MSR：
  - ID = 0x802
  - TPR = 0x808
  - EOI = 0x80B
  - SVR = 0x80F（0x1FF）
  - ICR = 0x830
- `lapic_send_ipi(dest, low)` 已实现，供 M4-33 SIPI 使用。

选择 x2APIC 而不是 MMIO，是因为当前启动页表只恒等映射前 1 GiB，而 LAPIC MMIO 在 0xFEE00000。

## 4. 自旋锁

`include/spinlock.h`：

```c
typedef struct { volatile u32 locked; } spinlock_t;
```

- `spin_lock`：`__atomic_compare_exchange_n` 生成 `lock cmpxchg`；
- `spin_unlock`：release store；
- `spin_lock_irqsave`：
  - `pushfq/popq` 保存 RFLAGS；
  - `cli`；
  - 再获取锁；
- `spin_unlock_irqrestore`：释放后按 IF 恢复；
- `cpu_relax`：`pause`。

PMM 使用：

```c
static spinlock_t pmm_lock = SPINLOCK_INIT;
spin_lock_irqsave(&pmm_lock, &flags);
...
spin_unlock_irqrestore(&pmm_lock, flags);
```

覆盖 `pmm_alloc_page/pmm_alloc_pages/pmm_free_page/pmm_free_pages/pmm_ref_inc`。

`page_cache_phys` 也加了同一把页缓存锁（目前只保护该查询）。

---

## 5. 验证

- QEMU `-smp 4`：
  - `[smp] MADT: 4 CPU(s)`；
  - BSP APIC ID 0，x2APIC=yes；
- `pmm] self-test: PASS`；
- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制（重要）

1. **AP 尚未启动**：没有 trampoline、没有 INIT/SIPI、没有 AP `ap_main`；
2. 调度器仍只有 BSP 一个 CPU；
3. 页缓存只锁了 `page_cache_phys`，`get/mark_dirty/flush` 尚未全锁；
4. VFS、pipe、调度器运行队列尚未加锁；
5. 没有 TLB shootdown；
6. 没有 APIC timer / IOAPIC，仍用 PIC/PIT；
7. `smp_cpu_count()` 返回 1，不代表 MADT 发现的 4 个 CPU 已上线；
8. QEMU 的 `-smp 4` 只是让固件提供 4 个 CPU 描述，AP 仍在等待 SIPI。

---

## 7. 下一步（M4-33）

1. 在固定低地址（如 0x6000）准备 16→32→64 位 AP trampoline；
2. BSP 写入 CR3/AP 栈地址；
3. x2APIC 发送 INIT + 两次 SIPI；
4. AP 进入 `ap_main`，递增 `smp_online`；
5. 加 AP 调度/每个 CPU 的 idle；
6. 再逐项给 VFS/pipe/page cache 加锁并做 TLB shootdown。

---

## 8. 复现清单

- [x] QEMU `-smp 4`
- [x] Multiboot2 ACPI RSDP / 低内存扫描
- [x] RSDT/XSDT → MADT
- [x] 枚举 4 个 APIC ID
- [x] x2APIC 启用
- [x] spinlock/irqsave
- [x] PMM 加锁
- [x] page_cache_phys 加锁
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] AP SIPI（M4-33）