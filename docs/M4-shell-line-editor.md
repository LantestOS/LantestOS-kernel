# M4-7 — Shell 行编辑器：历史、Tab 补全

> 状态：**已完成并通过实测**（QEMU sendkey 键盘注入 + 串口）  
> 日期：2026-10-28  
> 关联文档：[M4-external-shell-keyboard.md](./M4-external-shell-keyboard.md) · [design.md](./design.md)

---

## 1. 本步目标

1. `console_read` 从"内核按行缓冲"改为**原始字符模式**，行编辑交给用户态 Shell；
2. Shell 支持：
   - 回显、退格；
   - `↑` / `↓` 命令历史（8 条）；
   - `Tab` 命令名补全；
   - `Tab` 路径补全（遍历目录项）；
3. 修正用户 ELF 链接脚本，使 Shell 这类超过一页的大程序仍能保留 RX/RW 段共享页的装载测试；
4. 保持键盘 IRQ1、串口输入、init 重启、LantestFS 持久化全部可用。

---

## 2. 原始字符输入

`console_read()`：

```c
for (;;) {
    if (keyboard_getc(&c) || uart_poll_getc(&c)) {
        if (c == '\r') c = '\n';
        out[0] = c;
        return 1;
    }
    sti; hlt;
}
```

- 不再由内核回显/处理退格/Tab；
- `read(0, buf, 1)` 每次返回一个字节；
- 键盘 IRQ1 与 COM1 都汇聚到这里；
- Shell 用户态负责所有编辑逻辑。

键盘驱动扩展：

- `0xE0 0x48` → `ESC [ A`
- `0xE0 0x50` → `ESC [ B`
- `0xE0 0x4B/0x4D` → `ESC [ D/C`（当前忽略左右）

---

## 3. Shell 行编辑器

### 3.1 基础编辑

- 可打印字符：写入 `line_buf` 并回显；
- `\b` / `0x7F`：删除一个字符并输出 `\b \b`；
- Enter：输出换行，返回整行；
- Ctrl-C：取消当前行。

### 3.2 历史

```c
static char history[HIST_MAX][LINE_MAX];
static int history_count;
```

- 非空、非重复命令加入历史（最多 8 条，FIFO 淘汰）；
- `↑`：回到上一条，并 erase/redraw 当前行；
- `↓`：回到下一条，越过最新则清空；
- 历史只在当前 Shell 进程内有效，不写入磁盘。

### 3.3 Tab 补全

**命令名**（首个 token、无 `/`）：

- 匹配 `help/ls/cat/mkdir/touch/rm/echo/run/exit`；
- 唯一匹配：补全并追加空格；
- 多个匹配：换行列出，然后重绘 `lantest> ` + 当前行。

**路径**（后续 token 或包含 `/`）：

- 拆出目录部分与文件名前缀；
- `sys_open` + `sys_read` 遍历目录项；
- 目录补 `/`，普通文件补空格；
- 唯一匹配直接替换 token；多个匹配列出候选；
- 实测：`mkdir /tmp` 后输入 `ls /tm<Tab>` → `ls /tmp/`。

---

## 4. 用户链接脚本修正

Shell 加入行编辑器后 `.text` 超过 0x900，原脚本固定把 `.data` 放在 `0x8000000900`，导致：

```
section .bss VMA ... overlaps section .text VMA ...
```

新脚本动态放置 RW 段：

```ld
. = ALIGN(16);
. = . + 16;
.data : { ... } :data
.bss  : { ... } :data
```

- 不再与 RX 段重叠；
- 仍保持 RW 段非页对齐，并尽量与 RX 尾页共享，继续测试 ELF 加载器的页共享逻辑；
- Shell ELF 现在 RX `filesz≈0x1caa`，RW `vaddr=0x8000001cc0, memsz≈0xca0`，加载/运行正常。

---

## 5. 实测

### 5.1 历史（QEMU sendkey）

输入 `help` + Enter，再按 `↑` + Enter：

```
commands:
...
commands:
...
commands:
...
```

三次 `commands:` 输出：一次原始 help、一次 ↑ 历史、一次 Tab 补全。

### 5.2 Tab 补全

命令：

- 输入 `he` + `Tab` + Enter → 执行 `help`；
- 输入 `h` + `Tab` + Enter → 同理。

路径（先 `mkdir /tmp`）：

```
ls /tm<erase/redraw>ls /tmp/
```

### 5.3 串口与 init

- 串口发送 `help\n` → 正常执行；
- 发送 `exit\n` → init 打印 `init: shell exited, restarting` 并重新 exec `/bin/sh`；
- 串口原始字节模式工作正常。

### 5.4 自动化

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 踩坑记录

| 问题 | 处理 |
| --- | --- |
| Shell 变大后 `.text` 与固定 `.data=0x900` 重叠 | 链接脚本改为动态、非页对齐放置 RW 段 |
| 内核行缓冲无法实现历史 | console 改为原始字节，行编辑移入用户态 |
| 键盘方向键被丢弃 | 扩展码 `0xE0 0x48/0x50` 转成 `ESC [ A/B` |
| 路径补全误判 | 测试前 `/tmp` 不存在；先 mkdir 后补全正常 |
| 多候选重绘 | 列出候选后重新打印 prompt + 当前行 |
| 串口输入失效疑云 | 加临时 rx 日志确认字节到达；实际是测试时序问题，日志已移除 |

---

## 7. 已知限制

1. 只支持行尾编辑，没有左右移动光标/插入编辑；
2. 历史不持久化，重启即清空；
3. Tab 不做循环选择（多候选只列出）；
4. 键盘只有 set 1 基本布局 + shift；caps/功能键/小键盘未处理；
5. 没有管道、后台任务、作业控制；
6. 行长度 127 字节，候选最多 16 个。

---

## 8. 下一步

- **M4-8**：管道 `|`、`cat file | ...`、简单作业控制；
- **M5-3**：LantestFS 间接块、双超级块/fsck、journaling；
- 光标左右移动与行内编辑；
- 把更多命令做成 `/bin/*` 独立 ELF。

---

## 9. 复现清单

- [x] console 原始字符读取（键盘/串口统一）
- [x] Shell 回显、退格、Ctrl-C
- [x] 8 条命令历史 + ↑/↓
- [x] Tab 命令名补全
- [x] Tab 路径补全（目录遍历）
- [x] 用户链接脚本动态 RW 段，Shell 大 ELF 正常加载
- [x] QEMU sendkey 历史/Tab 实测
- [x] 串口输入 + init 重启实测
- [x] `make test` / `make test-persist` / `make test-exception` PASS
- [x] 构建零警告
