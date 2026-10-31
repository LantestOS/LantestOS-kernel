# Changelog / 变更记录

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

- **AP bring-up timing race (M4-47c)**: in roughly 2–3 of 10 fresh-disk
  boots the start handshake loses its window and boot stalls. Root cause
  localized to fixed `ap_delay` waits in `start_one_ap`; the fix is an
  AP parameter-ack / stage protocol with bounded waits. Reproduction is
  deterministic under `qemu -d int` (8/8).
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

- **AP 拉起时序竞态（M4-47c）**：约 2–3/10 的全新磁盘启动会卡在手势
  窗口；根因已定位到 `start_one_ap` 的固定 `ap_delay` 等待，修法为
  AP 参数 ack/stage 握手 + 有界等待；`qemu -d int` 下 8/8 确定性复现。
- SMP 仅在 QEMU 验证，未在物理硬件测试。
- AP idle 目前轮询就绪队列而非 `hlt`（功耗问题，不影响正确性）。
- `docs/`（设计文档与里程碑报告）目前为中文。
