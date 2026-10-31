# M4-29 — Lua 标准库：table / string / utf8

> 状态：**已完成并通过实测**（QEMU，Lua 启动任务 + 全量回归）  
> 日期：2026-10-29  
> 关联文档：[M4-lua-port.md](./M4-lua-port.md) · [M4-sse-fpu.md](./M4-sse-fpu.md)

---

## 1. 结果

在 M4-28 core+base 基础上加入：

- `ltablib.c`：`table.insert/concat/...`
- `lstrlib.c`：`string.upper/format/...`
- `lutf8lib.c`：`utf8.char/...`

`lua_main` 现在打开 base + table + string + utf8。

脚本：

```lua
print('Hello from Lantest OS!')
local t = {'hello','lua','from','lantest'}
table.insert(t, '!')
print(string.upper(table.concat(t, ' ')))
print(utf8.char(0x4E2D))
```

实测：

```
[M4] updated /bin/lua (217656 bytes)
[M4] lua task: pid=19
Hello from Lantest OS!
HELLO LUA FROM LANTEST !
中
lua: OK
[proc] exit: pid=19 name=user-lua status=0 -> ZOMBIE
```

UTF-8 字节实测为 `e4 b8 ad`（中）。

---

## 2. 编译/链接

`LUA_SRCS` 增加：

```
ltablib.c lstrlib.c lutf8lib.c
```

`lua_main`：

```c
luaL_requiref(L, "_G", luaopen_base, 1);       lua_pop(L,1);
luaL_requiref(L, LUA_TABLIBNAME, luaopen_table,1); lua_pop(L,1);
luaL_requiref(L, LUA_STRLIBNAME, luaopen_string,1);lua_pop(L,1);
luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8,1); lua_pop(L,1);
```

支持可选脚本路径：

```c
if (argc > 1)
    rc = luaL_loadfile(L, argv[1]) || pcall;
else
    fixed hello_script;
```

---

## 3. Port 补丁

| 问题 | 处理 |
| --- | --- |
| `lstrlib` 需要 `memchr` | port string.h/libc.c 增加 |
| `DBL_MAX_10_EXP` 未定义 | float.h 增加 `DBL_MIN_10_EXP/DBL_MAX_10_EXP` |
| `getc/freopen` 头未声明 | port stdio.h 增加 `getc` 宏、`freopen/ungetc` |
| 增量构建 `lauxlib.o` 仍是旧头 | Lua 对象依赖全部 port 头；清 obj 重编 |
| Makefile 仍检查小写 Hello，脚本已改大写 | 脚本恢复 `print('Hello from Lantest OS!')` |

---

## 4. 验证

- Lua 启动任务输出 Hello / uppercase / 中；
- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告；
- `/bin/lua` 217,656 B（<256 KiB exec cap）；
- Shell `run /bin/lua` 仍可执行。

---

## 5. 当前限制

1. 仍没有 io/os/math/coroutine/debug 库；
2. `lua_main` 没有 REPL，只有固定脚本或 `lua script.lua` 路径；
3. port libc 的 `%f/%g`、math、strtod 仍是简化实现；
4. `io/os` 不可用，脚本不能读写文件/取时间；
5. malloc 不合并，64 KiB chunk；
6. 没有 FPU 上下文切换；
7. 多 TU Lua 依赖手工头文件；
8. 用户栈 64 KiB。

---

## 6. 下一步

- 加 `lcorolib/ldblib/lmathlib`；
- REPL + `lua file.lua` + argv；
- `io/os` 库；
- 补 `%f/%g`、math；
- FXSAVE/FXRSTOR；
- M5-4 多块 Journal/SMP。

---

## 7. 复现清单

- [x] ltablib/lstrlib/lutf8lib 编译链接
- [x] lua_main 打开 table/string/utf8
- [x] table.insert/concat
- [x] string.upper
- [x] utf8.char(0x4E2D) 输出 UTF-8
- [x] 可选 argv[1] 脚本路径
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告