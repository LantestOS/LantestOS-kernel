# M4-30 — FPU 上下文、Lua io/os/math 与 REPL

> 状态：**已完成并通过实测**（QEMU：REPL + `cat /hello.txt` + 全量回归）  
> 日期：2026-10-29  
> 关联文档：[M4-lua-stdlibs.md](./M4-lua-stdlibs.md) · [M4-sse-fpu.md](./M4-sse-fpu.md)

---

## 1. FPU 上下文切换

`struct task` 在 `rsp` 之后新增：

```c
u8 fpu_state[512] __attribute__((aligned(16)));
```

- 位于 offset 16，天然 16 字节对齐；
- `switch_to`：

```asm
fxsave 16(%rdi)
fxrstor 16(%rsi)
```

- 每个 task 独立保存 x87/SSE 状态；
- task_alloc 清零 fpu_state；
- CR4.OSFXSR 已在 M4-27 启用。

Lua + floattest 交替运行不再互相污染。

---

## 2. Lua 标准库扩展

新增：

- `lmathlib.c`：`math.floor/sqrt/...`
- `liolib.c`：`io.open/read/write/close/...`
- `loslib.c`：`os.time/clock/exit/remove/...`

`lua_main` 打开 base/table/string/utf8/math/io/os。

演示脚本：

```lua
print('Hello from Lantest OS!')
local t = {'hello','lua','from','lantest'}
table.insert(t, '!')
print(string.upper(table.concat(t, ' ')))
print(utf8.char(0x4E2D))
print(math.floor(3.9), math.sqrt(9))

local f = io.open('/hello.txt', 'w')
if f then
  f:write('Written by Lua in Lantest OS!\n')
  f:close()
  print('File written.')
end
local g = io.open('/hello.txt', 'r')
if g then
  print(g:read('*a'))
  g:close()
end
```

实测：

```
Hello from Lantest OS!
HELLO LUA FROM LANTEST !
中
File written.
Written by Lua in Lantest OS!
lua: OK
```

Shell：

```
lantest> cat /hello.txt
Written by Lua in Lantest OS!
```

---

## 3. REPL

`lua_main` 三种模式：

| argc | 行为 |
| --- | --- |
| 0 | 内置演示脚本（启动任务） |
| 1 | REPL（`run /bin/lua`） |
| >1 | `lua script.lua` |

REPL：

```
lua> print("REPL OK")
REPL OK
lua> os.exit(0)
```

实测 `REPL_TEST: PASS`。

---

## 4. Port 补丁

| 问题 | 处理 |
| --- | --- |
| `L_tmpnam/tmpnam` | stdio.h/libc.c 增加 |
| `EXIT_SUCCESS/FAILURE` | stdlib.h 增加 |
| `mktime` | time.h/libc.c 增加桩 |
| `strcmp` 在 lua_main 未声明 | 加 `<string.h>` |
| math/io/os 依赖 | port 头与 libc 补齐 |

---

## 5. 回归

- `make test` → **PASS**（含 Lua io 文件写/读 grep）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告；
- REPL 手工测试 PASS；
- `cat /hello.txt` PASS。

---

## 6. 当前限制

1. `os.date/localtime/strftime` 仍是桩，`os.date` 不可用；
2. `system/getenv` 返回空；
3. `math.sin/cos/tan/exp/log/pow` 多数是简化/桩，只有 floor/ceil/sqrt/fabs/fmod 基本可用；
4. `%f/%g` 仍是简化实现；
5. REPL 逐行执行，不支持多行 `function/if` 块；
6. Lua `io` 的 `popen/tmpfile` 不可用；
7. `rename` 不支持；
8. FPU 状态已保存，但 `setjmp/longjmp` 不保存 xmm（caller-saved，通常安全）；
9. 用户栈 64 KiB；
10. malloc 不合并。

---

## 7. 下一步

- `os.date`/`strftime` 真正实现；
- 完整 `math`（sin/cos/exp/log/pow）；
- REPL 多行/历史；
- `io` 的 `rename/popen`；
- 多块 Journal / SMP。

---

## 8. 复现清单

- [x] task->fpu_state[512] aligned(16)
- [x] switch_to fxsave/fxrstor
- [x] Lua math/io/os 编译并打开
- [x] Lua 写 `/hello.txt` 并读回
- [x] Shell `cat /hello.txt`
- [x] REPL `print("REPL OK")` + `os.exit(0)`
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告