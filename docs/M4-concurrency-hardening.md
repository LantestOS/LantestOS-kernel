# M4-46 — Deferred reap、pipe 唤醒、堆/VFS/virtio 锁与多 AP 压力测试

> 状态：**已完成并通过实测**（QEMU `-smp 4`；3 个 stress worker 并发 + 持久化 + 异常）  
> 日期：2026-10-30  
> 关联：[M4-final-concurrency.md](./M4-final-concurrency.md) · [M4-ap-scheduler.md](./M4-ap-scheduler.md)

---

## 1. 结果

QEMU `-smp 4`，三个 stress worker 并发跑在 CPU1/2/3，每个 fork 3 个子进程（写内存 + 创建写文件），父进程 wait 全部回收：

```
[smp] stress worker pid=5 queued on CPU1
[smp] stress worker pid=9 queued on CPU2
[smp] stress worker pid=13 queued on CPU3
stress: worker PASS children=3
stress: worker PASS children=3
stress: worker PASS children=3
[reap] pid=5 deferred (active_cpus=1)
[reap] pid=8 deferred (active_cpus=1)
[reap] pid=12 deferred (active_cpus=1)
```

- deferred reap 真正触发：BSP 回收 AP 任务时发现 `active_cpus=1`，放入延迟队列，稍后完成回收；
- 零 PANIC / 零 CPU EXCEPTION / 零 virtio I/O error；
- 全新磁盘两次启动：boot_count 1→2，LFS 挂载成功，stress 在两次启动都 PASS。

## 2. 本轮修复的四个雷

### 雷 1：kmalloc/kfree 无锁（根因）

`mm/kmalloc.c` 注释就写着“single-CPU and not reentrant”。三个 AP 并发 malloc/free 会撕碎 free-list，导致 `vma_split_at` 遍历 VMA 链表时踩到被覆盖的指针（`cr2=0x100000000` 内核 #PF panic）。

修复：新增 `heap_lock`，`kmalloc`/`kfree` 全程 `spin_lock_irqsave`。

### 雷 2：virtio-blk 单请求槽位无锁

`drivers/virtio_blk.c` 用全局 `blk_req/blk_status/vq_desc[3]`，且 avail ring 的写-通知-used 读整个流程没有锁。多 AP 并发写盘（filetest/stress）会互相踩踏，出现 `[virtio] I/O error: id=0 status=ff` 与 boot_count 不推进。

修复：新增 `blk_io_lock`，`virtio_blk_rw` 全程持锁（关中断）。这是粗粒度但正确的做法。

### 雷 3：vfs_open O_CREAT 与 release 未锁

并发 `open(O_CREAT)` 与最后一个 close（release 释放 inode/数据块）撕裂 LFS 元数据，第二次启动 `LantestFS mount failed`。

修复：
- `vfs_open` 整体持 `vfs_lock`（lookup + create + open_inode 原子）；
- `file_close` 的 `release` 在 inode-backed 文件上持 `vfs_io_lock`（现与 `vfs_lock` 合并为同一把）；
- `vfs_read_file/vfs_write_blob` 的 f_op 读写加锁。

### 雷 4：AB-BA 死锁（页缓存回写）

一度把 `vfs_io_lock` 加进 `vfs_read_at/write_at`，但 `page_cache_get` 持 `pcache_lock` 时会经 `pcache_writeback → vfs_write_at → f_op->write`。两把锁嵌套 → 死锁。

修复：`vfs_read_at/write_at` **不持 VFS 锁**（它们只被 pcache（持 pcache_lock）和内核安装路径调用；用户 read/write 走 syscall 层，已有 vfs 锁）。同时把 `vfs_io_lock` 定义为 `vfs_lock` 同一把，消除 metadata/data 之间的顺序问题。

### 附：pipe 丢失唤醒

`pipe_wake_all` 从裸 `state = TASK_READY` 改为 `task_wakeup(t)`：唤醒时把 AP 任务重新挂回其本地队列（否则 READY 但不在队列 = 永不运行）。

## 3. task_wakeup 统一唤醒

`sched/scheduler.c` 新增：

```c
void task_wakeup(struct task *t)
{
    if (t->state != WAITING && != STOPPED) return;
    t->state = READY;
    t->stop_signal = 0;
    if (t->cpu != 0 && !t->on_rq)
        percpu_enqueue_task(t->cpu, t);
}
```

exit / stop / signal_pgrp / pipe 唤醒全部走它。fork 子任务继承父 `cpu` 并挂到对应本地队列。

## 4. AP idle 轮询（临时）

AP 在 `ap_main` 结尾由 `hlt` 改为：

```c
for (;;) { schedule(); pause; }
```

原因：当前 AP 对 0xFE/0x32/0xFD 的 IPI 投递不稳定（0x30 自测向量可靠）。轮询保证 worker 能被取走。这是**牺牲能耗换正确性**的临时措施，已列入限制。

副作用：BSP 的 `[task] A/B` tick 计数从 ~48 降到 ~8（AP 抢占用 QEMU host CPU），仍满足 `>=2`。

## 5. 回归

- `make test` → **PASS**（A=8, B=8，含 stress×3 PASS + deferred）；
- `make test-persist`（fresh disk，35s×2）→ **PASS**（boot_count 1→2，LFS 挂载正常）；
- `make test-exception` → **PASS**；
- 构建零警告。

## 6. 当前限制

1. AP idle 忙等轮询，浪费 CPU，未用 IPI 唤醒 AP（0xFE 对 idle 不生效待修）；
2. TLB shootdown 依赖 0x30 自测向量，ack 有超时兜底；
3. VFS 是单一大锁，粗粒度；
4. 用户 `O_TRUNC` 在持 vfs_lock 时调用 truncate（对 LFS 是磁盘 I/O，会长时间持锁）；
5. 压测规模是 3 worker × 3 子进程，不是 100 fork；
6. 没有 work stealing；
7. kmalloc 全局大锁在高频分配下会成瓶颈。

## 7. 下一步

1. 修 AP IPI/timer 唤醒，恢复 `sti; hlt`；
2. 缩小 vfs_lock 持有范围（O_TRUNC 移出锁，或给 truncate 单独锁）；
3. 更大规模 fork 压测（100 子进程）；
4. deferred reap 改等待队列而非忙轮询；
5. 细粒度 inode 锁 / slab。

## 8. 复现清单

- [x] kmalloc/kfree 堆锁
- [x] virtio_blk 请求锁
- [x] vfs_open O_CREAT 锁
- [x] file release 锁
- [x] AB-BA 死锁修复（read_at/write_at 不持 VFS 锁）
- [x] task_wakeup + pipe 唤醒入队
- [x] fork 子任务亲和 + 入队
- [x] deferred reap list + AP 轮询完成
- [x] AP idle 轮询 schedule
- [x] stress 3 worker × 3 子进程并发 PASS
- [x] 持久化 fresh disk 双启动 PASS
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
