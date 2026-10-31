# M4-8 — 管道（pipe）、dup2 与 Shell 管道语法

> 状态：**已完成并通过实测**（QEMU 串口 + LantestFS 间接块）  
> 日期：2026-10-28  
> 关联文档：[M4-shell-line-editor.md](./M4-shell-line-editor.md) · [M5-lantestfs.md](./M5-lantestfs.md)

---

## 1. 本步目标

1. 内核实现 `pipe()`：4 KiB 环形缓冲 + 读/写等待队列；
2. 读写阻塞/唤醒，写端全关时 read 返回 0（EOF），读端全关时 write 返回 `-EPIPE`；
3. 实现 `dup2()`，用于把子进程 stdout/stdin 重定向到管道；
4. Shell 支持 `cmd1 | cmd2`：
   - fork 两个子进程；
   - 左子进程 `dup2(write_fd, 1)`；
   - 右子进程 `dup2(read_fd, 0)`；
   - 父进程关闭两个端口并 wait 两次；
5. 顺带补 **LantestFS 一级间接块**：Shell ELF 已超过 48 KiB，旧文件系统装不下。

---

## 2. 管道内核实现

```c
struct pipe {
    u8 buf[4096];
    u32 rpos, wpos, used;
    u32 readers, writers;
    struct task *read_wait;
    struct task *write_wait;
};
```

- `pipe_read`：有数据就按环形缓冲拷贝；空且写端仍开 → `TASK_WAITING` 入读队列 + `schedule()`；写端全关 → 0。
- `pipe_write`：循环写；空间满 → 入写队列阻塞；读端全关 → `-EPIPE`。
- `pipe_release`（file refcount 归零时）：
  - 写端关闭：`writers--`，唤醒所有读等待者（EOF）；
  - 读端关闭：`readers--`，唤醒所有写等待者（EPIPE）。
- 两端都为 0 时 `kfree(pipe)`。
- `pipe_alloc()` 初始计数 0，`pipe_make_file()` 每创建一个方向 file 才 `readers++/writers++`，避免半初始化泄漏。

等待队列使用 `struct task.wait_next`：

```c
t->wait_next = *queue;
*queue = t;
t->state = TASK_WAITING;
schedule();
```

`pipe_wake_all()` 把等待任务改回 `TASK_READY`。

---

## 3. sys_pipe / sys_dup2

| 调用 | 号 | 参数 | 返回 |
| --- | --- | --- | --- |
| `pipe` | 12 | `rdi=&fds[2]` | 0 / `-ENOMEM` / `-EMFILE` / `-EFAULT` |
| `dup2` | 13 | `rdi=oldfd, rsi=newfd` | newfd / `-EBADF` |

- `pipe()` 创建读/写两个 file 对象，安装到 fd 表，再 `copy_to_user` 返回两个 fd；
- `dup2()` 关闭目标 fd（若打开），对源 file `file_get()` 后放进目标槽位；
- 方向由 `f_flags` 区分（`O_RDONLY` / `O_WRONLY`）。

---

## 4. Shell 管道语法

`run_command` 先扫描 `|`：

```c
if (bar != NULL) {
    *bar = '\0';
    cmd_pipeline(left, right);
    return;
}
```

`cmd_pipeline`：

```c
sys_pipe(fds);
p1 = fork();
if (p1 == 0) {
    sys_dup2(fds[1], 1);
    close(fds[0]); close(fds[1]);
    run_command(left);
    sys_exit(0);
}
p2 = fork();
if (p2 == 0) {
    sys_dup2(fds[0], 0);
    close(fds[0]); close(fds[1]);
    run_command(right);
    sys_exit(0);
}
close(fds[0]); close(fds[1]);
wait(); wait();
```

`cat` 无参数时读取 `STDIN_FD`，从而可以接管道：

```
lantest> echo hello | cat
hello
lantest> ls | cat
hello.txt
big.bin
...
lantest> cat /hello.txt | cat
Hello from ramfs!
```

启动时还自动执行 `echo pipe-ok | cat` 作为冒烟测试。

---

## 5. LantestFS 一级间接块

Shell 加行编辑器/管道后 ELF 达到 **54,136 字节**，超过 12 个直接块（48 KiB），`/bin/sh` 安装失败。

新增：

```c
#define LFS_INDIRECT_ENTRIES (4096 / 4)   /* 1024 */
#define LFS_MAX_BLOCKS (12 + 1024)
```

- inode 的 `indirect` 指向一个 4 KiB 指针块；
- `lfs_bmap(di, index, create)` 统一处理直接/间接块；
- 文件最大约 **4.2 MiB**；
- `truncate` / `unlink` 递归释放间接块；
- 块分配/释放时丢弃对应 cache entry。

**踩到的致命 bug：**

`lfs_bmap` 先拿到间接块 cache 指针，再调用 `lfs_alloc_block()` 分配数据块；分配位图会进入 cache 并可能**淘汰间接块**，原指针失效。结果间接块第 0 项被写成 `0xFFFFFFFF`，读取文件在 48 KiB 处截断，exec 报 ELF 非法。修复方式：

```c
b = lfs_alloc_block();          /* may evict cache */
ind = lfs_cache_get(di->indirect);  /* re-fetch! */
ind[index] = b;
```

---

## 6. 实测

自动冒烟：

```
pipe-ok
```

交互（串口）：

```
lantest> echo hello | cat
hello
lantest> ls | cat
hello.txt
big.bin
...
lantest> cat /hello.txt | cat
Hello from ramfs!
```

- 父进程 wait 两个子进程，prompt 正常返回；
- 子进程 stdout/stdin 重定向正确；
- 读空管道阻塞、写端关闭后 EOF、读端关闭后 EPIPE 均已验证；
- `/bin/sh` 以 54 KiB ELF 从 LantestFS 安装、读取、exec 成功。

自动化：

- `make test` → **PASS**（自动管道 + 全部旧功能）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 额外修复

### 7.1 cat 无参数

原 `run_command` 的 cat 分支要求必须有参数，管道右端 `cat` 直接报 missing file。改为：

```c
cmd_cat(arg);   /* 空参数 => 读 stdin */
```

### 7.2 Makefile 头文件依赖

给 `task.h` 增加 `wait_next` 后，`kernel.o` 因 Makefile 未跟踪头文件而保持旧结构布局，`tchild->parent = tparent` 写到了错误偏移，导致 parent wait 失败。加入：

```make
HDRS := $(wildcard include/*.h include/x86_64/*.h)
$(OBJS): $(HDRS)
```

并让 user 对象依赖 `user/userlib.h`。这是一个典型的"增量构建 + 结构体布局"陷阱。

---

## 8. 已知限制

1. 管道容量固定 4 KiB，没有 `F_SETPIPE_SZ`；
2. 只支持阻塞 I/O，没有 `O_NONBLOCK` / `poll/select`；
3. Shell 目前实现两段管道；多段依赖右递归，未做完整作业控制；
4. 没有信号，读端关闭时写端只得到 `-EPIPE`，不会收到 SIGPIPE；
5. LantestFS 只有一级间接块（~4.2 MiB），没有双超级块/fsck/journaling；
6. 管道内数据是裸字节，无消息边界。

---

## 9. 下一步

- **M5-3b**：双超级块 + fsck + journaling；
- **M4-9**：多级管道、后台任务、作业控制；
- 把 `ls/cat` 做成 `/bin` 下的独立 ELF，通过 execve 走管道；
- 管道非阻塞/select、SIGPIPE。

---

## 10. 复现清单

- [x] pipe 环形缓冲 + 读写等待队列
- [x] 阻塞读/写、EOF、EPIPE、引用计数释放
- [x] sys_pipe(12) / sys_dup2(13)
- [x] Shell `cmd1 | cmd2`（fork + dup2 + wait）
- [x] `cat` 无参数读 stdin
- [x] `echo hello | cat`、`ls | cat`、`cat file | cat` 实测
- [x] LantestFS 一级间接块，Shell 54 KiB ELF 可安装/执行
- [x] 修复间接块 cache 淘汰导致的 `0xFFFFFFFF` 指针
- [x] Makefile 头文件依赖，避免结构体布局陈旧
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
