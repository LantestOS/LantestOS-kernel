# M4-45 — TLB shootdown、并发锁与回滚策略

> 状态：**TLB/锁基础设施完成**；死亡压测未跑  
> 日期：2026-10-30  
> 关联：[M4-ipi-wake.md](./M4-ipi-wake.md) · [M4-active-cpus.md](./M4-active-cpus.md) · [M4-multi-ap-users.md](./M4-multi-ap-users.md)

---

## 1. TLB shootdown

新增 `arch/x86_64/tlb.c`：

- 全局 `tlb_target_cr3`、`tlb_ack_count`；
- `tlb_shootdown_all(cr3)`：
  - 设置目标 CR3，清零 ACK；
  - 向所有 online AP 广播 IPI；
  - 等待 ACK；带 1e6 次 `pause` 安全超时，避免 reap 卡死。

AP 端：在向量 0x30 handler 中合并 TLB ack（该向量已被验证能投递）：

```c
if (paging_read_cr3() == tlb_shootdown_target())
    paging_write_cr3(paging_read_cr3());
__atomic_add_fetch(&tlb_ack_count, 1, ...);
lapic_eoi();
```

**注意：0xFD/0x32 在当前 QEMU x2APIC 下没有收到 AP handler，实际使用 0x30；已保留 0xFD 代码路径但广播改用 0x30。**

`task_reap()` 在 `active_cpus==0` 后、释放页表前调用 `tlb_shootdown_all(t->cr3)`。

---

## 2. 并发锁

- **page_cache**：`page_cache_get/mark_dirty/flush` 全部包在 `pcache_lock` + `spin_lock_irqsave` 中；
- **pipe**：`struct pipe` 增加 `spinlock_t lock`，保护 `rpos/wpos/used/readers/writers` 和 wait queue；`pipe_wait` 先入队再解锁再 `schedule()`；
- **VFS**：新增全局 `vfs_lock`；`vfs_lookup` 加锁，`vfs_mkdir/vfs_unlink` 使用锁内 lookup，避免目录/dentry 链表撕裂。

---

## 3. 回归

- `make test` → **PASS**（task A=37, B=37）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 4. 当前限制

1. **死亡压测未跑**：还没有 `fork 100 + kill + 并发写文件`；
2. TLB ack 有超时兜底；不是所有 AP 都确认；
3. VFS 数据路径 `f_pos/i_size` 的细粒度锁未做；
4. deferred reap 未实现；`active_cpus` 仍是忙等；
5. AP 仍只跑短命任务；
6. 0xFD 广播在当前环境无效，改用 0x30。

---

## 5. 下一步（M4-46）

1. 写 `user/stress.c`（fork 100 + malloc + write）并跑；
2. deferred reap list + idle/timer 扫描；
3. VFS 数据路径锁；
4. 长任务多 AP 并发。

---

## 6. 复现清单

- [x] tlb_shootdown_all
- [x] IPI 广播 + ack 超时
- [x] AP TLB reload
- [x] task_reap 调用 TLB
- [x] page_cache 全锁
- [x] pipe lock
- [x] VFS 目录锁
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] fork 100 压测（M4-46）
