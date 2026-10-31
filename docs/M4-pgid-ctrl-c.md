# M4-12 — 进程组（PGID）、Ctrl-C/SIGINT 与后台 `&`

> 状态：**已完成并通过实测**（QEMU 键盘 Ctrl-C + 后台任务）  
> 日期：2026-10-28  
> 关联文档：[M4-signals-sigpipe.md](./M4-signals-sigpipe.md) · [M4-multistage-pipes.md](./M4-multistage-pipes.md)

---

## 1. 本步目标

1. 给每个任务增加 **PGID**（进程组 ID）；
2. 增加 `setpgid`、`set_fg`（终端前台组）、`detach`（后台脱离父进程）系统调用；
3. 管道所有 stage 归入同一个 PGID；
4. Shell 在运行前台任务时把终端前台 PGID 设为该组，退出后复位；
5. PS/2 键盘识别 **Ctrl-C**，向终端前台 PGID 发送 **SIGINT**；
6. SIGINT 默认动作：终止，退出码 `128 + 2 = 130`；
7. Shell 支持行尾 `&` 后台任务，父进程不 wait，子进程 detach 后由孤儿回收器处理。

---

## 2. PGID 结构

### 2.1 task

```c
struct task {
    ...
    u32 signal_pending;
    u64 pgid;          /* process group id */
    ...
};
```

- `task_alloc()`：`pgid = pid`；
- `task_fork()`：子进程继承父进程 `pgid`；
- boot task `pgid = 0`。

### 2.2 系统调用

| 调用 | 号 | 参数 | 作用 |
| --- | --- | --- | --- |
| `setpgid` | 14 | `pid, pgid` | pid=0 表示当前；pgid=0 表示用 pid |
| `set_fg` | 15 | `pgid` | 设置终端前台进程组 |
| `detach` | 16 | `pid` | 把子进程 parent 设为 NULL，交给孤儿回收 |

### 2.3 终端前台组

```c
static u64 tty_fg_pgid;

void tty_set_foreground_pgid(u64 pgid);
int  tty_send_sigint(void);   /* 向 fg pgrp 发 SIGINT；无前台组返回 0 */
```

- Shell 运行前台外部命令/管道时设置；
- 命令结束或 Shell 回到 prompt 时重置为 0；
- 没有前台组时，Ctrl-C 被转为 `0x03` 字符，由 Shell 行编辑器取消当前行。

---

## 3. 键盘 Ctrl-C

键盘驱动新增 `kbd_ctrl` 状态：

- `0x1D` make/release：Ctrl 按下/释放；
- `0x2E`（C）make 且 Ctrl 按住：
  - 如果存在前台 PGID：调用 `tty_send_sigint()`，**不**把 `c` 放入输入缓冲；
  - 否则 push `0x03`，让 Shell 行编辑器处理 `^C`。

---

## 4. SIGINT 投递

### 4.1 进程组广播

```c
void task_signal_pgrp(u64 pgid, int sig)
{
    for each user task with t->pgid == pgid:
        signal_send(t, sig);
        if (t->state == TASK_WAITING)
            t->state = TASK_READY;   /* wake blocked console/pipe waiters */
}
```

### 4.2 投递点

- syscall 返回用户态之前：`signal_deliver_pending(regs)`；
- 硬件 IRQ 从用户态返回之前：`isr_handler()` 中同样投递；
- 阻塞在 `console_read` / `pipe_read` / `pipe_write` 的任务检查 `signal_pending`，返回 `-EINTR`，随后 syscall 出口终止。

### 4.3 默认动作

```c
t->signal_pending &= ~mask;
kprintf("[signal] ... signal=%d -> terminate (status=%d)\n", sig, 128 + sig);
task_exit_current(128 + sig);   /* SIGINT -> 130 */
```

---

## 5. Shell 前台进程组

### 5.1 管道

```c
for each stage i:
    if (i == 0) setpgid(0, 0);          /* leader = first child pid */
    else        setpgid(0, first_pid);

parent: set_fg(first_pid);
wait all children;
set_fg(0);
```

### 5.2 顶层外部命令

```c
child: setpgid(0, 0);
parent: set_fg(child_pid);
wait;
set_fg(0);
```

### 5.3 后台 `&`

Shell 解析行尾 `&`：

```c
child: setpgid(0, 0); run_command(line); exit(0);
parent: detach(child_pid);
        print("[bg] started\n");
```

- 后台子进程 `pgid` 独立；
- `bg_mode=1` 阻止后台命令抢占终端前台组；
- 父进程立即回 prompt；
- 后台子进程退出后 parent=NULL，由 idle 孤儿回收器回收。

---

## 6. 实测

### 6.1 后台任务

```
lantest> echo bg | cat &
[bg] started
lantest>
bg
```

- prompt 立即返回；
- 后台管道输出 `bg`；
- 子进程由孤儿回收器处理。

### 6.2 Ctrl-C 杀整组

```
lantest> cat | cat
[exec] pid=15 -> /bin/cat ...
[exec] pid=16 -> /bin/cat ...
[signal] pid=15 name=fork-child signal=2 -> terminate (status=130)
[proc] exit: pid=15 name=fork-child status=130 -> ZOMBIE
[signal] pid=16 name=fork-child signal=2 -> terminate (status=130)
[proc] exit: pid=16 name=fork-child status=130 -> ZOMBIE
[proc] wait: parent pid=8 reaped child pid=15 status=130
[proc] wait: parent pid=8 reaped child pid=16 status=130
lantest>
lantest> echo alive
alive
```

- 两个 cat 同属 first child 的 PGID；
- Ctrl-C 只杀该组，**Shell 本身存活**；
- 阻塞在 console / pipe 的任务都能被唤醒并退出；
- 退出码 130。

`JOBCTRL_TEST: PASS`。

---

## 7. 自动化回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 8. 已知限制

1. 只有 `&`，还没有 `jobs/fg/bg`、作业表和 stopped 状态；
2. 没有 Session 抽象，所有任务共享同一个终端；
3. 没有 SIGTTIN/SIGTTOU，后台进程仍可读写终端；
4. 没有进程组权限检查，`setpgid/set_fg` 简化处理；
5. 没有 SIGINT 用户处理函数/忽略，只有默认终止；
6. 后台任务没有状态查询/回收确认；
7. 管道仍最多 8 段，阻塞模式。

---

## 9. 下一步

- **M4-13**：`jobs/fg/bg`、作业表、stopped 状态、SIGTSTP/SIGCONT；
- Session/终端控制、SIGTTIN/SIGTTOU；
- `mkdir/rm/touch` 外部化；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 10. 复现清单

- [x] task `pgid`，fork 继承
- [x] `setpgid(14)`、`set_fg(15)`、`detach(16)`
- [x] 终端前台 PGID
- [x] 键盘 Ctrl-C → SIGINT 前台组
- [x] SIGINT 默认终止，退出码 130
- [x] console/pipe 阻塞路径 EINTR
- [x] 管道所有 stage 同 PGID
- [x] 后台 `&` + detach + 孤儿回收
- [x] `echo bg | cat &` 立即回 prompt
- [x] `cat | cat` + Ctrl-C 杀掉两个子进程，Shell 存活
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
