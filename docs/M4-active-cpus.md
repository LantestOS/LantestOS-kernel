# M4-43 — active_cpus 与 reap 等待

> 状态：**UAF 防护完成**；TLB shootdown/IPI 尚未实现  
> 日期：2026-10-30  
> 关联：[M4-multi-ap-users.md](./M4-multi-ap-users.md)

---

## 1. 改动

`struct task` 增加：

```c
volatile u32 active_cpus;
```

- AP `ap_schedule` 切到用户任务前 `atomic_add(active_cpus,1)`；
- AP 切走 RUNNING 用户任务时 `atomic_sub`；
- AP 上 `sys_exit` 时 `atomic_sub`；
- `task_reap()` 在释放 CR3/页表前：

```c
while (atomic_load(&t->active_cpus) != 0)
    pause;
```

这样 BSP 回收任务前，保证没有 AP 还拿着该任务的地址空间在跑。

---

## 2. 验证

- 3 个 AP 并发执行 apuser，全部 `status=42`；
- BSP reaper 回收；
- `make test` → **PASS**（task A=47, B=47）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 3. 当前限制

1. **TLB shootdown 未实现**：BSP 改页表后 AP TLB 未刷新；
2. 没有 IPI 0xFD/0xFE；
3. `active_cpus` 等待是忙等，且依赖 AP 及时退出；
4. AP 仍只跑短命任务，没有长时间并发；
5. VFS/pipe/page cache 未全锁；
6. 没有 fork 100 + kill 压测。

---

## 4. 下一步 M4-44

1. TLB shootdown IPI 0xFD + CR3 重载；
2. IPI 0xFE 队列唤醒；
3. 长任务/多任务并发；
4. VFS/pipe/page cache 全锁；
5. fork 100 + kill 压测。

---

## 5. 复现清单

- [x] task active_cpus
- [x] AP 进入用户任务 inc
- [x] AP 切走/退出 dec
- [x] reap 等待 active_cpus==0
- [x] 3 AP apuser status=42
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告
- [ ] TLB shootdown（M4-44）
- [ ] IPI 唤醒（M4-44）