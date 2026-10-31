# M4-10 — 最小信号机制与 SIGPIPE

> 状态：**已完成并通过实测**（QEMU 串口 + `cat /big.bin | head`）  
> 日期：2026-10-28  
> 关联文档：[M4-pipes.md](./M4-pipes.md) · [M4-external-commands.md](./M4-external-commands.md)

---

## 1. 本步目标

1. 增加最小信号状态：每任务一个 pending 位图；
2. 实现 **SIGPIPE**（13）的默认动作——终止进程（退出码 `128 + 13 = 141`）；
3. 管道写端发现所有读端关闭时向当前任务发送 SIGPIPE；
4. 在系统调用返回用户态之前投递信号，默认动作直接终止，不返回到用户代码；
5. 新增 `/bin/head`，用 `cat /big.bin | head` 验证写端被杀。

---

## 2. 内核信号实现

### 2.1 任务状态

```c
struct task {
    ...
    u32 signal_pending;   /* bitmask: 1 << (sig-1) */
};
```

- `task_alloc()` / boot task 初始化为 0；
- fork 子进程从零开始，不继承 pending。

### 2.2 发送与投递

```c
void signal_send(struct task *t, int sig)
{
    t->signal_pending |= 1u << (sig - 1);
}

void signal_deliver_pending(registers_t *regs)
{
    if (task_current()->signal_pending & (1u << (SIGPIPE - 1))) {
        task_exit_current(128 + SIGPIPE);
        /* never returns */
    }
}
```

- `syscall_handler()` 在 switch 结束后、返回 iretq 前调用 `signal_deliver_pending()`；
- `task_exit_current()` 统一处理 exit/信号终止：
  - 状态设为 `TASK_ZOMBIE`；
  - 记录 `exit_code`；
  - 唤醒等待父进程；
  - 永久 `schedule()` 让出 CPU；
- 因此用户程序不会看到 `write()` 的 `-EPIPE` 返回值——默认动作直接终止。

### 2.3 管道发送 SIGPIPE

在 `pipe_write()` 中：

```c
if (p->readers == 0) {
    signal_send(task_current(), SIGPIPE);
    return -EPIPE;
}
```

- 读端全部关闭时，写端被标记 SIGPIPE；
- 该写操作返回 -EPIPE 后，syscall 出口投递信号并终止进程。

---

## 3. /bin/head

新增外部程序 `/bin/head`：

```c
while (sys_read(STDIN_FD, &c, 1) == 1) {
    sys_write(STDOUT_FD, &c, 1);
    if (c == '\n')
        break;
}
```

- 读完第一行就退出，关闭读端；
- 不解析参数（示例中的 `-n 1` 会被忽略，但仍只读一行）。

内核安装：

```
[M4] installed /bin/head (7184 bytes)
```

---

## 4. 实测

```
lantest> cat /big.bin | head
[signal] pid=12 name=fork-child SIGPIPE -> terminate (status=141)
[proc] exit: pid=12 name=fork-child status=141 -> ZOMBIE
[proc] wait: parent pid=8 reaped child pid=12 status=141
lantest>
```

- `cat` 写 5000 字节，`head` 只读第一行就退出；
- 管道读端归零；
- `cat` 的写请求返回 -EPIPE 前被标记 SIGPIPE；
- syscall 出口投递信号，`cat` 以 141 终止；
- 父 Shell wait 两个 child，prompt 正常返回。

对照：

```
lantest> echo hello | head
hello
lantest>
```

数据量小，写端在 head 退出前完成，不产生 SIGPIPE。

`SIGPIPE_TEST: PASS`。

---

## 5. 自动化回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 已知限制

1. 目前只有 **SIGPIPE + 默认终止**，没有 `sigaction`/`signal` 用户处理函数；
2. 没有忽略/阻塞信号、没有信号栈、没有 `sigreturn`；
3. 没有 SIGKILL/SIGTERM/SIGINT 等其它信号；
4. 信号只在 syscall 返回前投递，IRQ 返回路径暂不检查；
5. 没有进程组/作业控制，Shell 不能向整条管道发送信号；
6. 管道仍是两段，没有多级递归执行；
7. 管道仍为阻塞模式，无 `O_NONBLOCK`/`poll/select`。

---

## 7. 下一步

- **M4-11**：多级管道 `cmd1 | cmd2 | cmd3`、后台任务、作业控制；
- `mkdir/rm/touch` 外部化为 `/bin/*`；
- 信号处理函数 / `sigaction` / SIGINT（Ctrl-C）；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 8. 复现清单

- [x] 每任务 `signal_pending` 位图
- [x] `signal_send` + syscall 出口 `signal_deliver_pending`
- [x] SIGPIPE 默认动作：终止，退出码 141
- [x] 管道读端归零时写端发送 SIGPIPE
- [x] `/bin/head` 外部 ELF
- [x] `cat /big.bin | head` 触发 SIGPIPE，父进程 wait 到 141
- [x] `echo hello | head` 不触发 SIGPIPE
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
