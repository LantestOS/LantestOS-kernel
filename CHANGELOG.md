# Changelog / 变更记录

## v0.0.38b — 2026-10-31 · First public release

The first public snapshot of Lantest OS: an independent x86_64 SMP kernel with a
working userland, written from scratch (no Linux source, no Linux ABI).

### Highlights

- **Boot**: Multiboot2/GRUB2 → 32-bit → long mode, ACPI MADT parsing, x2APIC,
  SIPI trampoline, 4-CPU SMP (`-cpu max -smp 4`).
- **Memory**: physical page allocator, slab/kmalloc with locking, per-task PML4,
  demand paging, copy-on-write, user/kernel copy with SMAP checks.
- **VFS / filesystems**: VFS core (mount, dentry, inode, file), ramfs root,
  LantestFS (LFS) on virtio-blk with journaling and persistent boot counter.
- **Processes**: fork/exec/wait/waitpid, signals (SIGSEGV/SIGINT/…), process
  groups, foreground process group, pipes, shared memory (mmap SHARED + msync),
  kill/detach, TLB shootdown on address-space teardown.
- **Scheduler**: per-CPU run queues, per-task `active_cpus` guard, deferred reap
  for AP-owned tasks, wakeup unification.
- **System calls**: custom table (write/read/open/close/mkdir/unlink/fork/exec/
  wait/waitpid/pipe/dup2/setpgid/kill/mmap*/munmap/mprotect/msync/lseek/time…).
- **Userland**: libc (printf, malloc, string, file I/O), shell with pipelines,
  test programs, and a Lua 5.4 port running in Ring 3.
- **Concurrency hardening** (M4-45…M4-47b):
  - lock ordering fixes, spinlocks around heap, virtio-blk and VFS;
  - **pcache↔vfs ABBA deadlock removed** (page-cache eviction closes files
    outside `pcache_lock`);
  - **x2APIC ICR delivery-status drain** — hardware IPIs now actually deliver
    (probe: `irq 3 -> 4`, TLB `ACK TIMEOUT` count 26→0);
  - **wait/wakeup lost-wakeup window closed** (`SYS_WAIT` scans and sleeps in the
    same scheduler critical section as exit-and-wake);
  - wake IPI handler no longer calls `schedule()` from interrupt context.

### Verification

- `make test` (fresh 16 MiB disk, serial-marker driven): **7–8 / 10 runs green**;
  every green run has 3/3 stress workers passing.
- `make test-persist`: PASS (LFS `boot_count` 1 → 2 across two boots).
- `make test-exception`: PASS (deliberate kernel page fault panic path).
- Build: zero warnings (`-std=gnu11 -Wall -Wextra`).

### Known issues

- **AP bring-up timing race (M4-47c)**: in roughly 2–3 of 10 fresh-disk boots the
  AP start handshake loses the SIPI window and the boot self-test stalls after
  boot. Root cause is a fixed-delay handshake in `start_one_ap`; the fix is an
  AP-side parameter-acknowledge/stage protocol with bounded waits.
- Physical SMP validation is QEMU-only; no real hardware testing yet.
- `docs/` (design + milestone reports) is currently written in Chinese.

---

## 中文摘要

首个公开版本：从零实现的 x86_64 SMP 宏内核（不含 Linux 源码、不实现 Linux ABI），
含 VFS/LFS、进程与信号、管道与共享内存、自研系统调用、用户态 libc/shell、Lua 5.4
移植；并发侧修复了 page-cache↔VFS ABBA 死锁、x2APIC IPI 投递、wait 唤醒窗口。
自检：fresh-disk `make test` 7–8/10 通过，persist 与 exception 测试通过。已知问题：
AP 拉起存在时序竞态（M4-47c），仅 QEMU 验证。
