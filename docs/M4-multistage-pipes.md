# M4-11 — 多级管道（最多 8 段）

> 状态：**已完成并通过实测**（QEMU 串口）  
> 日期：2026-10-28  
> 关联文档：[M4-pipes.md](./M4-pipes.md) · [M4-signals-sigpipe.md](./M4-signals-sigpipe.md)

---

## 1. 本步目标

把两段硬编码管道改成通用多段：

```
cmd1 | cmd2 | cmd3 | ...        (最多 8 段)
```

要求：

1. N 个 stage 建 N−1 个 pipe；
2. 每个子进程只保留自己需要的读写端，其余 pipe fd 全部关闭；
3. 父进程 fork 完所有子进程后关闭所有 pipe fd；
4. 父进程 wait 全部子进程；
5. 死锁/EOF 必须正确：不能因为某个 fd 没关导致读端永远等不到 EOF。

---

## 2. 算法

### 2.1 解析

`run_command()` 检测到 `|` 后调用 `cmd_pipeline(line)`；`cmd_pipeline` 原地按 `|` 切分：

```c
stages[0] = "cmd1";
stages[1] = "cmd2";
stages[2] = "cmd3";
```

最多 8 段，空 stage 报错。

### 2.2 建 pipe

```c
for (i = 0; i < npipes; ++i)
    sys_pipe(pipes[i]);
```

### 2.3 fork 每个 stage

```c
for (i = 0; i < nstages; ++i) {
    pids[i] = fork();

    if (pids[i] == 0) {
        if (i > 0)        dup2(pipes[i-1][0], 0);
        if (i < npipes)   dup2(pipes[i][1], 1);

        for (j = 0; j < npipes; ++j) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }

        exec_stage(stages[i]);
        exit(0);
    }
}
```

关键点：

- **第 0 段**：只写 `pipes[0]`；
- **中间段 i**：读 `pipes[i-1]`，写 `pipes[i]`；
- **最后段**：只读 `pipes[npipes-1]`；
- 每个子进程在 dup2 之后关闭**所有** pipe fd，包括自己刚 dup 的原 fd；
- 父进程在所有 fork 完成后也关闭所有 pipe fd。

### 2.4 等待

```c
for (i = 0; i < forked; ++i)
    wait();
```

任何 stage 提前退出（如 head）都会让上游写端在关闭后收到 SIGPIPE，不会永久卡住。

---

## 3. 实测

串口输入：

```
lantest> echo one | cat | cat
one
lantest> echo two | cat | cat | cat
two
lantest> cat /hello.txt | cat | cat
Hello from ramfs!
lantest> ls | cat | cat
hello.txt
big.bin
...
lantest>
```

- 2 段、3 段、4 段均正常；
- 父进程 wait 全部子进程后 prompt 返回；
- 没有出现因 fd 未关导致的 EOF 死锁。

`MULTIPIPE_TEST: PASS`。

---

## 4. 自动化回归

- `make test` → **PASS**（自动 `echo pipe-ok | cat`）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 5. 已知限制

1. 最多 8 段；
2. 仍是阻塞管道，无 `O_NONBLOCK`/`poll/select`；
3. 没有 `&` 后台任务、`jobs/fg/bg`、进程组/作业控制；
4. 没有 SIGINT（Ctrl-C 杀前台进程组）；
5. `>` 重定向只支持顶层外部命令，管道 stage 内暂不支持；
6. Shell 只外部化了 ls/cat/echo/head，mkdir/rm/touch 仍是内建。

---

## 6. 下一步

- **M4-12**：`&` 后台任务 + 孤儿回收 + 简单 `jobs`；
- SIGINT/Ctrl-C 与进程组；
- `mkdir/rm/touch` 外部化；
- 管道 stage 内的重定向；
- **M5-3b**：双超级块 + fsck + journaling。

---

## 7. 复现清单

- [x] N 段管道建 N−1 个 pipe
- [x] 每段正确 dup2 stdin/stdout
- [x] 每个子进程关闭全部 pipe fd
- [x] 父进程关闭全部 pipe fd 后 wait 所有子进程
- [x] `echo one | cat | cat`
- [x] `echo two | cat | cat | cat`
- [x] `cat /hello.txt | cat | cat`
- [x] `ls | cat | cat`
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
