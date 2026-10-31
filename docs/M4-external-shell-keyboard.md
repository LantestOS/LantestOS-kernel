# M4-6 — 外部 Shell（/bin/sh）与 PS/2 键盘 IRQ1

> 状态：**已完成并通过实测**（QEMU 键盘注入 + LantestFS 双启动）  
> 日期：2026-10-28  
> 关联文档：[M4-argv-envp.md](./M4-argv-envp.md) · [M5-lantestfs.md](./M5-lantestfs.md)

---

## 1. 本步目标

1. Shell 不再是内核直接创建的任务，而是磁盘上的 **`/bin/sh`**；
2. 内核只加载一个极小的 **init** 进程，由 init `fork + execve("/bin/sh")`；
3. Shell 退出后 init 自动重启 Shell；
4. 接入 **PS/2 键盘 IRQ1**，用扫描码转 ASCII + 环形缓冲区；
5. `console_read` 同时支持键盘与 COM1（键盘优先，串口兜底）。

---

## 2. 外部 Shell

### 2.1 持久化 `/bin/sh`

内核仍把 `shell.elf` 作为安装载荷嵌入镜像，但**执行时从 LantestFS 读取**：

```c
u64 shell_size = embedded_shell_end - embedded_shell_start;
struct dentry *d = vfs_lookup("/bin/sh");

if (d && d->d_inode && d->d_inode->i_size == shell_size)
    kprintf("[M4] /bin/sh already on disk\n");
else
    vfs_write_blob("/bin/sh", embedded_shell, shell_size);
```

第一次启动安装，后续启动直接使用磁盘文件。

### 2.2 init 进程

`user/init.c` 是小 ELF：

```c
for (;;) {
    pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { "/bin/sh", 0 };
        sys_execve("/bin/sh", argv, 0);
        sys_exit(127);
    }
    sys_wait(&status);
    print("init: shell exited, restarting\n");
}
```

内核只创建 init：

```
[M4] installed /bin/sh (31272 bytes)
[M4] init task pid=3 (execs /bin/sh from LantestFS)
[exec] pid=8 -> /bin/sh argc=1 env=0 entry=0x8000000180
Lantest shell (M4-6) - type 'help'
```

`/bin/sh` 的 ELF 从磁盘读取、加载；内核不再直接构造 Shell 任务。

### 2.3 Shell 崩溃/退出重启

键盘输入 `exit`：

```
init: shell exited, restarting
[fork] pid=3 -> child pid=10
[exec] pid=10 -> /bin/sh argc=1 env=0
Lantest shell (M4-6) - type 'help'
```

`INIT_RESTART_TEST: PASS`。

---

## 3. PS/2 键盘 IRQ1

### 3.1 驱动

- IDT 中 IRQ1 已由异常/IRQ 框架接管；
- `irq_handler()` 在 IRQ1 时调用 `keyboard_irq()`；
- 读端口 `0x60` 获取扫描码（set 1）；
- make code（最高位 0）查表转 ASCII，shift 状态切换大小写/符号；
- 忽略 `0xE0` 扩展码、释放码、caps lock（当前）；
- 256 字节环形缓冲区，满时丢弃；
- `keyboard_init()` 清空缓冲区并 `pic_unmask(1)`。

### 3.2 console_read

```c
if (!keyboard_getc(&c) && !uart_poll_getc(&c)) {
    sti; hlt;
    continue;
}
```

- **键盘优先**，COM1 兜底；
- 没有输入时仍然 `sti; hlt`，PIT/IRQ1 会唤醒；
- 键盘中断到达即唤醒，不必等 10 ms tick。

---

## 4. 键盘实测（QEMU monitor sendkey）

用 QEMU monitor 的 `sendkey` 注入真实 PS/2 扫描码：

```
sendkey h; sendkey e; sendkey l; sendkey p; sendkey ret
```

串口输出：

```
help
commands:
  help                 this help
  ...
```

再发送：

```
sendkey r u n spc slash b i n slash h e l l o spc k b d ret
```

输出：

```
lantest> run /bin/hello kbd
[exec] pid=10 -> /bin/hello argc=2 env=0
hello from an exec'd ELF!
argc=2
argv[0]=/bin/hello
argv[1]=kbd
```

`KEYBOARD_IRQ_TEST: PASS`。

---

## 5. 回归

- `make test` → **PASS**（包含 `/bin/sh`、键盘初始化、LantestFS、fork/exec、argv）；
- `make test-persist` → **PASS**（`/bin/hello`、`/bin/sh` 第二次启动 `already on disk`，LFS boot_count 1→2）；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 踩坑记录

| 问题 | 处理 |
| --- | --- |
| IRQ1 被 PIC 屏蔽 | `keyboard_init()` 里 `pic_unmask(1)` |
| IRQ1 走错分支 | `irq_handler` 增加 `irq == 1 -> keyboard_irq()`，EOI 由框架统一处理 |
| 键盘缓冲区为空时 `hlt` 睡死 | IRQ1 会唤醒；同时保留 PIT 100 Hz 兜底 |
| QEMU 默认扫描码集 | 按 set 1 实现；`0xE0` 扩展码暂时忽略 |
| Shell 每次启动重建 | `/bin/sh` 持久化 + 大小检查，第二次启动直接用磁盘 ELF |
| init 重启后 fd 泄漏 | shell 退出为 ZOMBIE，init wait 回收；fd 引用计数递减 |
| Shell 崩溃（page fault） | 目前仍会触发内核 panic；init 只能重启正常退出的 shell |

---

## 7. 已知限制

1. 键盘仅支持 set 1 基本按键 + shift；caps lock/方向键/功能键/小键盘未处理；
2. 没有命令历史（上下键）与 Tab 补全；
3. 没有管道、重定向组合、作业控制；
4. Shell 是内置命令集合，只能执行 `/bin/hello` 这类无参数程序（现在支持 argv）；
5. Shell 发生异常（非法访问）会内核 panic，而不是被 init 重启；
6. 串口与键盘同时输入时只有键盘优先，没有多路复用/会话管理。

---

## 8. 下一步

- **M4-7**：命令历史（↑/↓）、Tab 补全、管道；
- **M5-3**：间接块、双超级块/fsck、journaling；
- 把 `ls/cat` 也做成独立 `/bin` 程序，让 Shell 完全由外部 ELF 组成。

---

## 9. 复现清单

- [x] `/bin/sh` 持久化到 LantestFS
- [x] init ELF：fork + execve("/bin/sh") + wait
- [x] Shell 退出后 init 自动重启
- [x] PS/2 键盘 IRQ1 + 扫描码 set 1 + shift
- [x] 256B 环形缓冲区 + `console_read` 键盘优先
- [x] QEMU `sendkey` 注入 `help` 与 `run /bin/hello kbd`，argv 正确
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
