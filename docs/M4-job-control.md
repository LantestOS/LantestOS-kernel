# M4-13 — 作业控制：SIGTSTP/SIGCONT、jobs/fg/bg

> 状态：**已完成并通过实测**（QEMU 键盘 Ctrl-Z / Ctrl-C）  
> 日期：2026-10-28  
> 关联文档：[M4-pgid-ctrl-c.md](./M4-pgid-ctrl-c.md) · [M4-multistage-pipes.md](./M4-multistage-pipes.md)

---

## 1. 本步目标

1. 新增 `TASK_STOPPED` 暂停态；
2. 新增 **SIGTSTP（Ctrl-Z）** 与 **SIGCONT**；
3. `waitpid()` 让 Shell 能拿到“子进程被暂停”而不是只等退出；
4. Shell 作业表 + `jobs/fg/bg`；
5. 暂停时必须**原地停在阻塞系统调用里**，SIGCONT 后继续原 read/write，而不是返回 `-EINTR` 导致 cat 退出。

---

## 2. 内核

### 2.1 任务状态

```c
enum task_state {
    ...
    TASK_ZOMBIE,
    TASK_DEAD,
    TASK_STOPPED,
};

struct task {
    ...
    u32 stop_signal;
};
```

- 调度器只选择 `TASK_READY` / `TASK_RUNNING`，STOPPED 不会被调度；
- `task_stop_current(sig)`：
  1. 设 `stop_signal`；
  2. 状态改 `TASK_STOPPED`；
  3. 唤醒等待父进程（让 waitpid 立即返回暂停状态）；
  4. `while (state == TASK_STOPPED) schedule();`
- SIGCONT 由 `task_signal_pgrp()` 直接把 STOPPED 改回 READY，`task_stop_current()` 的 while 退出，系统调用继续。

### 2.2 信号

```c
#define SIGCONT 18
#define SIGTSTP 20
```

- SIGTSTP 默认动作：stop（不退出）；
- SIGCONT：继续；
- SIGINT/SIGPIPE：仍默认终止（130 / 141）。

### 2.3 阻塞循环原地暂停

`console_read`、`pipe_read`、`pipe_write`：

```c
if (pending & (1u << (SIGTSTP - 1))) {
    clear pending;
    task_stop_current(SIGTSTP);
    continue;          /* SIGCONT 后继续原来的 read/write */
}
if (pending != 0)
    return -EINTR;     /* SIGINT/SIGPIPE */
```

这保证暂停/恢复对用户程序透明：cat 不会因为被暂停而退出。

### 2.4 waitpid

新增系统调用 18：

```c
waitpid(pid, &status)
```

- 子进程 ZOMBIE → 返回退出码并回收；
- 子进程 STOPPED → 返回 `0x7f | (sig << 8)`，**不回收**；
- 否则父进程阻塞，直到子进程 exit/stop 唤醒；
- Shell 用低字节 `0x7f` 判断“暂停”。

---

## 3. Shell 作业表

```c
struct job {
    int used;
    int pgid;
    int pids[MAX_STAGES];
    int npids;
    int stopped;
    char cmd[64];
};
```

- 前台管道用 `waitpid` 逐个等待；
- 任一子进程返回 stopped → 建 job；
- `jobs` 列出 `[n] Stopped/Running pgid=... cmd`；
- `fg [n]`：`SIGCONT` 整组 → 设置终端前台组 → waitpid 每个 pid → 若又暂停则保留，否则删除；
- `bg [n]`：`SIGCONT` 整组，标记 Running，立即返回。

---

## 4. 键盘 Ctrl-Z

键盘驱动：

- `0x1D` Ctrl 状态；
- `0x2C`（Z）+ Ctrl → `tty_send_sigtstp()`；
- 前台 PGID 存在时发 SIGTSTP；不存在时忽略。

---

## 5. 实测

```
lantest> cat | cat
[proc] stop: pid=12 name=fork-child signal=20 -> STOPPED
[proc] stop: pid=13 name=fork-child signal=20 -> STOPPED
lantest> jobs
[1] Stopped pgid=12  cat | cat
lantest> fg 1
[signal] pid=12 name=fork-child signal=2 -> terminate (status=130)
[proc] exit: pid=12 name=fork-child status=130 -> ZOMBIE
[signal] pid=13 name=fork-child signal=2 -> terminate (status=130)
[proc] exit: pid=13 name=fork-child status=130 -> ZOMBIE
lantest> jobs
lantest> echo alive
alive
```

- Ctrl-Z 暂停整组；
- `jobs` 显示 Stopped 与命令；
- `fg 1` 恢复后 Ctrl-C 杀掉整组；
- 作业表清空；
- Shell 存活。

`JOB_CONTROL_TEST: PASS`。

---

## 6. 另一个上限修复

Shell 加作业表后 ELF 达到 **75,272 字节**。`/bin/sh` 已在 LantestFS 中，但 exec 失败：

```
[dbg] exec read /bin/sh rc=-22 size=0
```

根因是 `EXEC_MAX_IMAGE` 只有 64 KiB，`vfs_read_file` 直接返回 `-EINVAL`。已提升为 **512 KiB**。

---

## 7. 自动化回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 8. 已知限制

1. `&` 后台任务不进入作业表，`jobs` 只显示被暂停的前台作业；
2. 没有 SIGTTIN/SIGTTOU、Session、终端独占；
3. 没有 SIGSTOP/SIGKILL，也没有用户态信号处理函数；
4. `bg` 后作业退出，Shell 暂不主动回收（可能短暂僵尸，直到 Shell 退出/后续 fg）；
5. `fg` 只处理自己 waitpid 得到的 pids，已退出/已回收的子进程会跳过；
6. 作业表最多 8 个；
7. 管道仍最多 8 段、阻塞模式。

---

## 9. 下一步

- 补齐 `&` + 作业表 + SIGTTIN/SIGTTOU/Session；
- 或转向底层硬核：**COW + 页引用计数 + VMA**；
- **M5-3b**：双超级块 + fsck + journaling；
- 继续外部化 mkdir/rm/touch。

---

## 10. 复现清单

- [x] `TASK_STOPPED` + `stop_signal`
- [x] SIGTSTP/SIGCONT
- [x] Ctrl-Z → 前台 PGID SIGTSTP
- [x] 阻塞 read/write 原地暂停/恢复
- [x] waitpid 返回 stopped 状态
- [x] Shell 作业表 + `jobs`
- [x] `fg [n]` 恢复并重新等待
- [x] `bg [n]` 后台继续
- [x] `cat | cat` → Ctrl-Z → jobs → fg → Ctrl-C 全链路
- [x] EXEC_MAX_IMAGE 提升到 512 KiB
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
