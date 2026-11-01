# Changelog / 变更记录

## v0.1.0 — 2026-11-01 · First gate-certified release / 首个通过门槛测量的发布

### English

The first release issued through the measured gate (`docs/release-gate.md`).
M4-47c + M4-47d closed the entire known-defect backlog:

- **Zombie/deferred-reap list corruption** (double free root cause): the
  deferred reap list no longer shares `rq_next` with the per-CPU ready
  queue; tasks are unlinked from their ready queue before destruction;
  AP schedulers never pick ZOMBIE/DEAD (`task_reap` + `percpu_remove_task`).
- **TLB shootdown livelock**: `tlb_shootdown_all` is fire-and-forget. GDB
  caught all four CPUs spinning in the ack wait (ack=0, all locks idle):
  a syscall-context sender (IF=0) can never service peers' ACK IPIs, and
  the shared ack counter got re-zeroed by every reaper. Safety rests on
  the `active_cpus==0` invariant plus hardware CR3-write flush.
- **Serial byte-loss**: `uart_putc` used to give up after 1e6 spins and
  silently dropped bytes; now unbounded THRE wait under `uart_lock`, with
  `uart_puts` emitting a whole buffer atomically; console writes go
  through it. (This artefact had made passing boots look like freezes.)
- **Heap forensics stay in-tree**: scan guard + 64-slot caller ring,
  raw-serial dump on trip. Zero trips over the whole gate sample.

Gate results (fresh-disk, `cc5b69b` + this commit):

| Tier | Result |
| --- | --- |
| `make test` × 50 | **50/50** — every run stress 3/3, ACK TIMEOUT 0, no PANIC |
| `-d int` × 5 | 5/5 |
| `-icount shift=7` × 3 | 3/3 (~290 virtual seconds each) |
| `test-persist` × 10 | 10/10 |
| `test-exception` × 5 | 5/5 |
| build | zero warnings |

Remaining known issues (non-blocking, recorded per H1): AP idle polls
instead of `hlt`; QEMU-only validation; docs mostly Chinese (wiki pending).

### 中文

首个完全按测量门槛（`docs/release-gate.md`）发布的版本。M4-47c + M4-47d
清空了全部已知缺陷：僵尸/延迟回收链表复用导致的 double free（`defer_next`
独立链 + reap 前先从就绪队列摘除 + AP 调度跳过 ZOMBIE）；TLB shootdown
活锁（GDB 四核同框取证，改为 fire-and-forget，依据 `active_cpus==0` 不变式
与 CR3 写入硬件刷新）；串口静默丢字（无界 THRE 等待 + `uart_lock` +
整缓冲原子写；该假象曾把通过的启动误报为冻结）；堆取证装备常驻（扫描哨
+ 调用环形日志，整个门槛样本零触发）。

门槛实测：fresh-disk `make test` **50/50**（每轮 stress 3/3、ACK TIMEOUT 0、
零 PANIC）；`-d int` 5/5；`-icount shift=7` 3/3（约 290 虚拟秒）；
persist 10/10；exception 5/5；零警告。

非阻塞残项（按 H1 记录）：AP idle 轮询未改 hlt；仅 QEMU 验证；docs 以中文
为主（wiki 待初始化）。

---

## v0.0.38b — 2026-10-31 · First public release / 首个公开版本

### English

The first public snapshot of Lantest OS: an independent x86_64 SMP kernel
written from scratch — no Linux source code, no Linux ABI, custom syscall
table, object layouts and build system.

**Kernel**

- **Boot**: Multiboot2/GRUB2 → 32-bit → long mode; ACPI MADT parsing;
  x2APIC enabled via MSRs; SIPI trampoline at physical `0x6000`;
  4-CPU SMP verified under QEMU (`-cpu max -smp 4 -m 512M`).
- **Memory**: physical page allocator; slab-style `kmalloc`/`kfree` with
  heap lock; per-task PML4 page tables; demand paging; copy-on-write fork;
  `mmap` / `mmap_file` / `munmap` / `mprotect` / shared `mmap` + `msync`;
  SMAP-guarded user/kernel copies.
- **Processes & IPC**: `fork` / `execve` / `wait` / `waitpid`; SIGSEGV on
  bad user faults; process groups, foreground group, `kill`, `detach`;
  pipes with blocking readers/writers; `dup2`; fd table over console,
  ramfs and LFS files.
- **VFS & filesystems**: VFS core (mount, dentry, inode, file_operations);
  ramfs root; **LantestFS (LFS)** on legacy virtio-blk with persistent
  boot counter across reboots; page cache with writeback and eviction.
- **Scheduler**: per-CPU run queues with per-CPU locks; AP LVT timers;
  user tasks pinned to CPU1–3; FPU (FXSAVE/FXRSTOR) context switching;
  deferred reap for tasks still active on another CPU; TLB shootdown on
  address-space teardown.
- **Userland**: custom libc (stdio/malloc/string/file I/O), shell with
  pipelines and job control, ~20 test programs, **Lua 5.4.7 port** running
  in Ring 3.

**Concurrency hardening (M4-45 … M4-47b)**

- Lock ordering established: `run_queue_lock`/`rq_lock` (level 1) outside
  `vfs_lock`/`pcache_lock`/`heap_lock`/`blk_io_lock` (level 2) outside
  `pipe->lock` (level 3).
- **pcache↔vfs ABBA deadlock removed** — page-cache eviction now detaches
  an entry under `pcache_lock` and closes the file only after the lock is
  dropped (M4-47a).
- **x2APIC IPI delivery fixed** — writing the ICR while a previous send
  is still pending is silently dropped by the hardware; the driver now
  drains delivery-status (bit 12) before/after each write and reports ESR
  errors. Boot probe proves it: `irq 3 -> 4`; TLB `ACK TIMEOUT` went from
  26 per run to 0 (M4-47b).
- **Lost-wakeup window closed** — `SYS_WAIT`/`SYS_WAITPID` scan-and-sleep
  and exit-and-wake-parent now share one scheduler critical section;
  slow work (`copy_to_user`, `task_reap`) stays outside the lock (M4-47b).
- Wake-IPI handler no longer calls `schedule()` from interrupt context.

**Verification (fresh 16 MiB disk, serial-marker based)**

- `make test`: **7–8 of 10 runs green**; every green run has 3/3 stress
  workers, apuser tasks on CPU1–3 (`status=42`), shell pipelines
  (`pipe-ok`), LFS persistence, Lua, and all user tests passing.
- `make test-persist`: PASS (LFS `boot_count` 1 → 2 across two boots).
- `make test-exception`: PASS (deliberate kernel #PF → panic path).
- Build: zero warnings (`-Wall -Wextra`).

**Known issues**

- **AP bring-up timing race — RESOLVED and re-attributed (M4-47b/d)**:
  the stall was the x2APIC ICR pending-drop (INIT/SIPI also travel through
  ICR), fixed by the delivery-status drain in M4-47b; confirmed with
  `re-INIT=0` across 10 runs, `-d int` 16/16 APs online, `ACK TIMEOUT=0`.
  The remaining late freezes were a TLB-shootdown **livelock** (all CPUs
  spinning with IF=0 can never service each other's ACK IPIs), fixed in
  M4-47d by fire-and-forget shootdown; and a **serial byte-loss**
  artefact that made passing boots look failed, fixed by unbounded
  THRE wait under a uart lock plus atomic whole-buffer writes.
- SMP is validated on QEMU only; no physical hardware testing yet.
- AP idle currently polls the run queue instead of `hlt` (power, not
  correctness).
- `docs/` (design overview + milestone reports) is currently in Chinese.

### 中文

兰特斯特操作系统首个公开快照：从零实现的独立 x86_64 SMP 宏内核——不含 Linux
源码、不实现 Linux ABI，系统调用表、对象布局与构建系统全部自研。

**内核能力**

- **启动**：Multiboot2/GRUB2 → 32 位 → 长模式；ACPI MADT 解析；x2APIC MSR
  使能；物理 `0x6000` 处的 SIPI 跳板；QEMU `-cpu max -smp 4 -m 512M` 下
  4 核 SMP 验证。
- **内存**：物理页分配器；带堆锁的 slab 式 `kmalloc`/`kfree`；每任务 PML4
  页表；按需缺页；COW fork；`mmap`/`mmap_file`/`munmap`/`mprotect`/
  共享 `mmap` + `msync`；SMAP 守护的用户态读写拷贝。
- **进程与 IPC**：`fork`/`execve`/`wait`/`waitpid`；非法用户访问投递
  SIGSEGV；进程组、前台组、`kill`、`detach`；阻塞式管道读写；`dup2`；
  控制台/ramfs/LFS 文件统一的 fd 表。
- **VFS 与文件系统**：VFS 核心（挂载点/dentry/inode/file_operations）；
  ramfs 根；**LantestFS (LFS)** 跑在 legacy virtio-blk 上，重启后
  boot_count 持久；带写回与驱逐的页缓存。
- **调度器**：每 CPU 就绪队列与独立锁；AP LVT 定时器；用户任务绑定
  CPU1–3；FPU（FXSAVE/FXRSTOR）上下文切换；他核仍活跃任务的延迟回收
  （deferred reap）；页表销毁前的 TLB shootdown。
- **用户态**：自研 libc（stdio/malloc/string/文件 I/O）、支持管道与作业的
  shell、约 20 个测试程序、**Lua 5.4.7 移植**运行于 Ring 3。

**并发硬化（M4-45 … M4-47b）**

- 确立锁序：`run_queue_lock`/`rq_lock`（1 级）在外，
  `vfs_lock`/`pcache_lock`/`heap_lock`/`blk_io_lock`（2 级）居中，
  `pipe->lock`（3 级）在最内。
- **拆除 pcache↔vfs ABBA 死锁**——页缓存驱逐改为持锁摘链、**放锁后**
  再 `file_close`（M4-47a）。
- **修复 x2APIC IPI 投递**——上一次发送仍 pending 时写 ICR 会被硬件静默
  丢弃；驱动现在每次写前后都排空 delivery-status（bit 12）并上报 ESR。
  启动探针实证：`irq 3 -> 4`；TLB `ACK TIMEOUT` 从每轮 26 次归零
  （M4-47b）。
- **关闭丢失唤醒窗口**——`SYS_WAIT`/`SYS_WAITPID` 的"扫描子进程+置
  WAITING"与退出路径的"置 ZOMBIE+唤醒父进程"共用同一调度器临界区；
  慢操作（`copy_to_user`、`task_reap`）一律移到锁外（M4-47b）。
- 唤醒 IPI 处理器不再在中断上下文里调用 `schedule()`。

**验证（全新 16 MiB 磁盘、串口 marker 判定）**

- `make test`：**10 次中 7–8 次全绿**；每个绿灯轮次 stress worker 3/3、
  CPU1–3 的 apuser 全部 `status=42`、shell 管道（`pipe-ok`）、LFS 持久化、
  Lua 与全部用户测试通过。
- `make test-persist`：通过（两次启动 boot_count 1 → 2）。
- `make test-exception`：通过（故意触发内核 #PF → panic 路径）。
- 构建零警告（`-Wall -Wextra`）。

**已知问题**

- **AP 拉起时序竞态——已结案并重新归因（M4-47b/d）**：真实根因是
  x2APIC ICR pending 丢弃（INIT/SIPI 同样经 ICR），已由 M4-47b 的
  delivery-status 排空修复；三重证据：`re-INIT=0`×10 轮、`-d int`
  16/16 全部 AP 上线、`ACK TIMEOUT=0`。残余的晚期"冻结"实为 TLB
  shootdown **活锁**（各核关中断自旋永远收不到彼此的 ACK IPI），
  M4-47d 改为 fire-and-forget 修复；以及**串口丢字**造成的观测假象
  （通过的启动被误报失败），已由无界 THRE 等待 + uart 锁 + 整缓冲
  原子写修复。
- SMP 仅在 QEMU 验证，未在物理硬件测试。
- AP idle 目前轮询就绪队列而非 `hlt`（功耗问题，不影响正确性）。
- `docs/`（设计文档与里程碑报告）目前为中文。
