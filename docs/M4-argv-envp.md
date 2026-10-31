# M4-5 — argv/envp 与持久化 /bin/hello

> 状态：**已完成并通过实测**（QEMU 串口交互 + LantestFS 两次启动）  
> 日期：2026-10-28  
> 关联文档：[M5-lantestfs.md](./M5-lantestfs.md) · [design.md](./design.md)

---

## 1. 本步目标

1. `execve(path, argv, envp)` 真正把参数传到新程序；
2. 用户态 `_start` 从初始栈读取 `argc/argv/envp`；
3. Shell 支持 `run /bin/hello one two`；
4. `/bin/hello` 不再每次启动重写：LantestFS 上只在缺失或大小不符时安装，第二次启动直接执行磁盘上的 ELF。

---

## 2. execve 的用户栈布局

x86_64 进程初始栈（从低地址到高地址）：

```
rsp ->  argc
        argv[0]
        ...
        argv[argc-1]
        NULL
        envp[0]
        ...
        envp[envc-1]
        NULL
        argv/envp 字符串本体
```

内核 `do_execve`：

1. 用 `copy_from_user` 逐字节收集用户 `argv/envp` 指针数组和字符串；
2. 上限：16 个 argv、8 个 envp、每个字符串 128 字节；
3. 没传 argv 时自动构造 `argv[0] = path`；
4. 在新用户栈页上：
   - 从栈顶向下拷贝 envp/argv 字符串；
   - 8 字节对齐；
   - 计算 `argc + argv + NULL + envp + NULL` 的指针数组空间；
   - 调整 `rsp % 16 == 8`，满足 C 函数入口 ABI；
5. 最后 `regs->rsp = 新栈指针`。

失败（参数过多 `-E2BIG`、指针非法 `-EFAULT`、栈放不下）时**新 PML4 尚未切换**，旧进程继续运行，Shell 可以打印错误。

---

## 3. 用户态 _start

`user/hello.c` 使用 naked `_start`：

```c
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile (
        "movq (%rsp), %rdi\n"          /* argc */
        "leaq 8(%rsp), %rsi\n"         /* argv */
        "leaq 8(%rsi,%rdi,8), %rdx\n"  /* envp = argv + (argc+1)*8 */
        "andq $-16, %rsp\n"            /* SysV 对齐 */
        "call main\n"
        "movl %eax, %edi\n"
        "movl $2, %eax\n"
        "int $0x80\n"
        "1: jmp 1b\n"
    );
}

int main(int argc, char **argv, char **envp);
```

- 不依赖 libc；
- `argv[0]`、参数、环境指针全部来自内核初始栈；
- 其他程序仍使用 `_start(void)`（不接受参数），初始栈对齐同样满足 ABI。

---

## 4. Shell 的 run 参数

```c
static void cmd_run(char *argline)
{
    char *argv[8];
    int argc = 0;
    /* 按空格切分 argv */
    ...
    pid = sys_fork();
    if (pid == 0) {
        sys_execve(argv[0], argv, 0);
        sys_exit(127);
    }
    sys_wait(&status);
}
```

启动时的自动冒烟测试也改成可变的 `char boot_prog[] = "/bin/hello";`，避免向只读字符串写入 token。

---

## 5. 持久化 /bin/hello

内核启动逻辑：

```c
vfs_mkdir("/bin", 0755);
u64 hello_size = embedded_hello_end - embedded_hello_start;
struct dentry *d = vfs_lookup("/bin/hello");

if (d && d->d_inode && d->d_inode->i_size == hello_size)
    kprintf("[M4] /bin/hello already on disk (%lu bytes)\n", hello_size);
else
    vfs_write_blob("/bin/hello", embedded_hello_start, hello_size);
```

- 第一次启动：把嵌入内核的 `hello.elf` 写进 LantestFS；
- 第二次启动：文件已在磁盘上，**不再重写**，直接 open/read/exec；
- 如果 ELF 版本变化导致大小不同，会自动更新一次；
- ramfs fallback 下文件本来不持久，仍会每次安装。

---

## 6. 实测证据

### 6.1 自动 exec（无参数）

```
[exec] pid=8 -> /bin/hello argc=1 env=0 entry=0x8000000000
hello from an exec'd ELF!
argc=1
argv[0]=/bin/hello
```

### 6.2 Shell 带参数

```
lantest> run /bin/hello one two
[fork] pid=3 -> child pid=9
[fork] child pid=9 resumes in Ring 3 (rax=0)
[elf] entry=0x8000000000 phnum=2
[exec] pid=9 -> /bin/hello argc=3 env=0 entry=0x8000000000
hello from an exec'd ELF!
argc=3
argv[0]=/bin/hello
argv[1]=one
argv[2]=two
run: child exited
```

### 6.3 重启后直接执行磁盘上的程序

第二次启动：

```
[M4] /bin/hello already on disk (11976 bytes)
...
lantest> run /bin/hello a b c
[exec] pid=9 -> /bin/hello argc=4 env=0 entry=0x8000000000
argc=4
argv[1]=a
argv[2]=b
argv[3]=c
```

`ARGV_PERSIST_TEST: PASS`。

### 6.4 自动化

- `make test` → **PASS**（包含 `argc=1`、LantestFS、fork/exec、VFS）；
- `make test-persist` → **PASS**（LFS/virtio boot_count 1→2，且第二次启动 `/bin/hello already on disk`）；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 踩坑记录

| 问题 | 处理 |
| --- | --- |
| 初始栈不对齐 | 指针数组放置后调整 `rsp` 使 `%16 == 8`，满足 C 入口 ABI |
| 没传 argv | 自动用 path 填 `argv[0]`，保证 argc≥1 |
| 参数指针非法 | 逐指针/逐字节 `copy_from_user`，fixup 返回 `-EFAULT` |
| 参数过多 | 16 argv / 8 envp / 128B 字符串上限，超限返回 `-E2BIG` |
| 自动 run 写入只读字符串 | 使用可变 `char boot_prog[]` 再切分 |
| 已存在旧 hello | 比较 `i_size` 与嵌入 ELF 大小；不一致则更新 |
| 内核静态参数缓冲 | execve 单线程且 syscall 关中断，静态缓冲安全 |

---

## 8. 已知限制

1. envp 仍未由 Shell 提供（当前传 0，envc=0）；
2. 没有 auxv、没有 `/proc`、没有 shebang；
3. argv/envp 总大小必须塞进单页用户栈（4 KiB）；
4. `_start(void)` 程序忽略参数，只有 hello 使用 naked 启动；
5. `/bin/hello` 只按大小判断是否需要更新，同大小内容变化不会刷新；
6. 键盘 IRQ、管道、后台任务仍未实现。

---

## 9. 下一步

- **M5-3**：间接块（支持 >48 KiB 文件）、双超级块/fsck、journaling；
- **M4-6**：键盘 IRQ + 环形缓冲、管道、命令历史；
- 把 Shell 本身也做成磁盘上的 `/bin/sh`，为 fork/exec 多程序做准备。

---

## 10. 复现清单

- [x] execve 复制 argv/envp 字符串与指针数组
- [x] 初始栈布局 + C ABI 对齐
- [x] naked `_start` 提取 argc/argv/envp
- [x] Shell `run <file> [args...]`
- [x] `run /bin/hello one two` → argc=3 / argv 正确
- [x] `/bin/hello` 首次安装、第二次启动不再重写
- [x] 重启后执行磁盘上的 `/bin/hello` 并带参数
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
