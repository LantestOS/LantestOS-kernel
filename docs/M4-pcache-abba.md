# M4-47a — pcache 延迟 close：拆除 ABBA 死锁

> 状态：**已提交**；fresh-disk `make test` **8/10**（M4-46 基线约 1/3）
> 残留：晚期冻结 2/10（tick≈500-600，无 panic，见 §5）
> 日期：2026-10-31
> 关联：[M4-concurrency-hardening.md](./M4-concurrency-hardening.md)

---

## 1. 背景：上一里程碑的绿灯有水分

M4-46 的验证复用了脏 `disk.img`。改用 **fresh disk**（每次 `truncate` 新建，
触发 LFS 格式化 + 安装 6 个二进制）后，基线 3 连跑挂 2，挂死在 tick≈140-170
（安装期高压窗口）。本轮又证实 deferred 实验树在同一形态下回归——于是执行
**单变量二分装修**，一次只装一发补丁、每发 3×fresh 验证。

## 2. 真凶：pcache_lock ↔ vfs_lock 反向嵌套

M4-45/46 给页缓存加了 `pcache_lock`，给 VFS 加了 `vfs_lock`，但存在环：

```
路径 A（驱逐）：  page_cache_get(持 pcache_lock)
                   → pcache_evict_one → pcache_free_entry
                   → file_close → lfs_release → 需要 vfs_lock     [pcache → vfs]

路径 B（写）：    syscall write(持 vfs_lock, 经 file_close/release)
                   → lfs_write → page_cache_mark_dirty → 需要 pcache_lock  [vfs → pcache]
```

两个 CPU 各持一半 → ABBA 自旋死锁。fresh disk 的安装期（6 个二进制连续写、
缓存满驱逐、close 频繁）是必现窗口。

**GDB 证据**：挂死时 BSP 栈
`task_reap → tlb_shootdown_all`（等待 ACK 而非死锁点）与更早期的
`spin_lock` 现场共同指向锁嵌套；修复后该形态消失。

## 3. 修复（单变量）

`mm/page_cache.c`：`pcache_free_entry` 改为 `pcache_detach_entry`——
持锁只做**摘链**并把 `e->file` 返回；`file_close()`（可能进 lfs_release
抢 vfs_lock）移动到调用方 `page_cache_get` **释放 pcache_lock 之后**执行，
再重新拿锁。锁序从此单向：`vfs_lock → pcache_lock`。

本轮只装了这一发补丁（pipe 原子入队 / deferred wakeup / wait re-check 均
未回装——它们在实验树中引入了确定性回归，见 §6）。

## 4. 验证

- `make test` fresh-disk 10 连跑：**8/10**（run1-5,7,8,10 PASS）；
- stress worker 每次 **3/3 PASS**（deferred reap 路径也在工作）；
- `make test-persist` → **PASS**；`make test-exception` → **PASS**；
- 零警告。

## 5. 残留问题（下一步主攻）

**晚期冻结 2/10**：run6 停于 tick=600（uptime 6s）、run9 停于 tick=560。
特征：无 PANIC、无 CPU EXCEPTION、A/B 内核线程在冻结前正常打印、
stress/pipe/lua 全部已 PASS（冻结发生在更晚的自动管道/回收阶段）。
嫌疑集中在实验树曾试图修的两处：
1. wait/wake 的 lost-wakeup 窗口（SYS_WAIT 检查与睡眠之间）；
2. pipe 等待者入队非原子。

## 6. 实验树教训（已回滚，不再回装）

deferred wakeup 全套（MPSC 唤醒栈 + pipe 原子入队 + wait re-check +
Level 位 IPI）fresh 0/10：9/9 子进程 ZOMBIE 但 worker 永不打印 PASS。
确定性 bug 未定位前，不与本修复混装。

## 7. 复现清单

- [x] ABBA 环分析与修复（detach + 锁外 close）
- [x] fresh-disk 10 连跑 8/10
- [x] test-persist / test-exception PASS
- [x] 构建零警告
- [ ] 晚期冻结 2/10（M4-47b 目标）
- [ ] fresh 10/10 验收线
