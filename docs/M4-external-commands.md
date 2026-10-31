# M4-9 — 外部命令：/bin/ls、/bin/cat、/bin/echo

> 状态：**已完成并通过实测**（QEMU 串口 + LantestFS 持久化）  
> 日期：2026-10-28  
> 关联文档：[M4-pipes.md](./M4-pipes.md) · [M4-external-shell-keyboard.md](./M4-external-shell-keyboard.md)

---

## 1. 本步目标

把 Shell 内建的 `ls/cat/echo` 踢出 Shell，变成 LantestFS 上的独立 ELF：

1. 新增 `user/crt.h`：naked `_start` 从初始栈取 `argc/argv/envp`，调用 `main`；
2. 新增 `user/ls.c`、`user/cat.c`、`user/echo.c`，编译为 ELF；
3. 内核启动时安装到 `/bin/ls`、`/bin/cat`、`/bin/echo`（大小检查，已存在不重写）；
4. Shell 对这三个命令改为 `fork + execve`；
5. 管道 stage 直接 `execve` 外部 ELF，而不是调用 Shell 内函数；
6. 支持外部命令的 `>` 重定向。

---

## 2. 外部程序

### 2.1 共用启动头 `user/crt.h`

```c
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile (
        "movq (%rsp), %rdi\n"          /* argc */
        "leaq 8(%rsp), %rsi\n"         /* argv */
        "leaq 8(%rsi,%rdi,8), %rdx\n"  /* envp */
        "andq $-16, %rsp\n"
        "call main\n"
        "movl %eax, %edi\n"
        "movl $2, %eax\n"
        "int $0x80\n"
        "1: jmp 1b\n"
    );
}
```

每个外部程序只写 `int main(int argc, char **argv, char **envp)`。

### 2.2 程序

| ELF | 功能 |
| --- | --- |
| `/bin/ls` | open 目录 + read dirent，目录名补 `/` |
| `/bin/cat` | 有参数 open 文件；无参数读 stdin |
| `/bin/echo` | 打印 argv[1..]，空格分隔 + 换行 |

三者都只依赖 `userlib.h` 中的 syscall 包装，无 libc。

### 2.3 内核安装

```c
vfs_mkdir("/bin", 0755);
install_binary("/bin/hello", hello_elf_start, hello_elf_end);
install_binary("/bin/sh",    shell_elf_start, shell_elf_end);
install_binary("/bin/ls",    ls_elf_start,    ls_elf_end);
install_binary("/bin/cat",   cat_elf_start,   cat_elf_end);
install_binary("/bin/echo",  echo_elf_start,  echo_elf_end);
```

- 首次启动写入 LantestFS；
- 大小一致则不重写；
- 与 `/bin/sh`、`/bin/hello` 一样持久化。

串口实测：

```
[M4] installed /bin/sh (54272 bytes)
[M4] installed /bin/ls (9736 bytes)
[M4] installed /bin/cat (9368 bytes)
[M4] installed /bin/echo (7768 bytes)
```

---

## 3. Shell 改造

### 3.1 命令路径映射

```c
static const char *command_path(const char *cmd)
{
    if (!scmp(cmd, "ls"))   return "/bin/ls";
    if (!scmp(cmd, "cat"))  return "/bin/cat";
    if (!scmp(cmd, "echo")) return "/bin/echo";
    return 0;
}
```

### 3.2 顶层外部命令

`run_command` 识别外部命令后：

```
fork
  child: 可选 dup2(redirect_fd, 1)
         execve(/bin/xxx, argv, 0)
parent: close redirect fd; wait
```

`>` 重定向在 Shell 中解析：

```
lantest> echo saved > /x.txt
lantest> cat /x.txt
saved
```

### 3.3 管道 stage 直接 exec

`cmd_pipeline` 的子进程不再调用 Shell 内建函数，而是：

```c
sys_dup2(fds[1], 1);
exec_stage(left);     /* 解析 argv -> execve("/bin/...") */
```

`exec_stage` 对外部命令直接 `execve`；如果是 `mkdir/rm/...` 等内建命令，则回退到 `run_command`，因此在管道中也能用内建命令。

### 3.4 实测

```
lantest> echo hello | cat
hello
lantest> ls | cat
hello.txt
big.bin
...
lantest> cat /hello.txt | cat
Hello from ramfs!
lantest> echo saved > /x.txt
lantest> cat /x.txt
saved
```

串口日志同时显示：

```
[exec] pid=10 -> /bin/echo argc=2 env=0
[exec] pid=11 -> /bin/cat  argc=1 env=0
pipe-ok
```

即启动自检 `echo pipe-ok | cat` 已经实际执行了两个独立 ELF。

---

## 4. 踩坑记录

| 问题 | 处理 |
| --- | --- |
| shell.c 中 `copy_str` 在 `exec_stage` 之后定义 | 在文件顶部加 forward declaration |
| `>` 重定向原来由内建 echo 处理 | 新增通用 `run_external_cmd`，打开文件并 `dup2` 到 stdout |
| 外部命令 ELF 首次启动不存在 | 内核统一 `install_binary()`，大小检查 + 持久化 |
| 管道右端 `cat` 需要 stdin 模式 | cat 无参数读 `STDIN_FD` |
| 内建函数被替换后 unused warning | 保留为 `__attribute__((unused))` 参考实现，后续可删除 |

---

## 5. 自动化

- `make test` → **PASS**（自动管道实际执行 `/bin/echo` 和 `/bin/cat`）；
- `make test-persist` → **PASS**（第二次启动 `/bin/*` 已存在）；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 已知限制

1. 目前只把 `ls/cat/echo` 外部化；`mkdir/touch/rm/help/run/exit` 仍是 Shell 内建；
2. `>` 重定向只对外部命令生效，内建命令（如 `run`）暂不支持；
3. 管道仍是两段；多段依赖右递归，未做完整作业控制；
4. 没有 SIGPIPE、非阻塞管道、`head` 等会提前关闭读端的场景；
5. Shell 内仍保留旧内建 ls/cat/echo 的未使用代码，后续可删除以瘦身；
6. LantestFS 有一级间接块，但无 journaling/双超级块。

---

## 7. 下一步

- **M4-10**：多级管道 `cmd1 | cmd2 | cmd3`、后台任务、SIGPIPE；
- 把 `mkdir/rm/touch` 也做成 `/bin/*`；
- **M5-3b**：双超级块 + fsck + journaling；
- 删除 Shell 内不再使用的内建实现。

---

## 8. 复现清单

- [x] `user/crt.h` naked 启动 + `main(argc, argv, envp)`
- [x] `/bin/ls`、`/bin/cat`、`/bin/echo` 独立 ELF
- [x] 内核安装到 LantestFS，重启不重写
- [x] Shell 外部命令 fork/execve/wait
- [x] 外部命令 `>` 重定向
- [x] 管道 stage 直接 exec 外部 ELF
- [x] `echo hello | cat`、`ls | cat`、`cat file | cat` 实测
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
