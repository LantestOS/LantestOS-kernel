# M4-28 — Lua 5.4.7 移植成功

> 状态：**已完成并通过实测**（QEMU：启动任务 + `run /bin/lua`）  
> 日期：2026-10-29  
> 关联文档：[M4-lua-prereq.md](./M4-lua-prereq.md) · [M4-sse-fpu.md](./M4-sse-fpu.md)

---

## 1. 结果

Lua 5.4.7 核心 + base 库已编译进兰特斯特：

- `/bin/lua` 安装到 LantestFS；
- 启动任务执行：

```lua
print('Hello from Lantest OS!')
```

输出：

```
[M4] updated /bin/lua (184888 bytes)
[M4] lua task: pid=19
Hello from Lantest OS!
lua: OK
[proc] exit: pid=19 name=user-lua status=0 -> ZOMBIE
```

Shell 里：

```
lantest> run /bin/lua
Hello from Lantest OS!
lua: OK
lantest>
```

这不再是“类似 Lua”，而是真正的 Lua 5.4.7 源码编译执行。

---

## 2. Port 结构

```
user/lua_port/
├── include/     # 17 个标准头（stdint/stddef/stdlib/stdio/string/math/time/...）
├── src/         # Lua 5.4.7 核心 + lauxlib + lbaselib（49 文件）
├── libc.c       # 外部符号 libc（malloc/stdio/string/strtod/setjmp/...）
├── lua_main.c   # luaL_newstate + base + luaL_dostring
└── obj/         # 构建产物
```

链接：

```make
LUA_CFLAGS := ... -Iuser/lua_port/include -Iuser/lua_port/src -Iuser \
              -DLUA_USE_C89 -include stdio.h
user/lua.elf: $(LUA_OBJS) obj/libc.o obj/lua_main.o user/user.ld
```

- 多 TU 链接，把 `libc.o` 放在 Lua 对象之后；
- `lua_main` 用 `crt.h` 的 naked `_start`；
- 用户栈 64 KiB；
- SSE 已启用。

---

## 3. 编译的 Lua 文件

核心：

```
lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem
lobject lopcodes lparser lstate lstring ltable ltm
lundump lvm lzio lauxlib lbaselib
```

暂未包含：

- `liolib/oslib/mathlib/ldblib/lcorolib/loadlib`；
- `lua.c/luac.c` 独立主程序；
- table/string/utf8 库。

`lua_main` 只 open base 库，因此 `print` 可用；数字/表等核心 VM 仍在。

---

## 4. 关键问题与修复

| 问题 | 处理 |
| --- | --- |
| `lstate.h` 需要 `<signal.h>` | 增加 `sig_atomic_t` 桩头 |
| `sprintf` 隐式声明 | 编译加 `-include stdio.h`，并把 port 头放在 `-Iuser` 之前 |
| `getc/freopen` 缺失 | port stdio 增加宏/实现 |
| 旧 Lua 对象混入 `user/malloc.h` 的 static malloc | 清理 `user/lua_port/obj` 后重编 |
| `/bin/lua` 1 MB debug ELF 超过 exec 上限 | 用户链接脚本丢弃 `.debug*`，`ld -s`，Lua 降到 184,888 B |
| `run /bin/lua` 仍失败 | `cmd_run` 的 argv 数组没有 `argv[argc]=NULL`，偶发读到栈垃圾；补 NULL 终止 |
| exec 镜像上限 512 KiB | 降到 256 KiB（Lua 184 KiB 足够） |
| 用户代码不能浮点 | M4-27 SSE + `USER_FLOAT_CFLAGS` |
| 1 页用户栈 | M4-26 扩到 16 页/64 KiB |

---

## 5. 验证

- Lua 启动任务 PASS；
- Shell `run /bin/lua` PASS；
- `make test` → **PASS**（含 `Hello from Lantest OS!`）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. 只链接 **base 库**，没有 table/string/utf8/io/os/math/debug/coroutine；
2. `lua_main` 执行固定脚本，不是 REPL/文件解释器；
3. `luaL_loadfile` 代码已链接但未使用；
4. port libc 的 `%f/%g` 是简化实现，`math` 函数多为桩；
5. malloc 只有 free list、不合并，64 KiB chunk；
6. 没有 FPU 上下文切换（单浮点任务安全）；
7. `LUA_USE_C89` 配置；
8. 用户栈固定 64 KiB；
9. `/bin/lua` 尚未支持 argv 脚本路径；
10. 没有 `io/os` 库，因此不能读文件/系统时间。

---

## 7. 下一步

- 加 `ltablib/lstrlib/lutf8lib`，支持表/字符串/UTF-8；
- 把 `lua_main` 改成 REPL / `lua file.lua`；
- 补 `%f/%g`、`strtod` 精度、math；
- 实现 FXSAVE/FXRSTOR 上下文；
- `io/os` 库（文件、time）；
- 继续多块 Journal/SMP。

---

## 8. 复现清单

- [x] Lua 5.4.7 源码进入 `user/lua_port/src`
- [x] 17 个标准头
- [x] 外部 multi-TU `libc.c`
- [x] Lua core + lauxlib + lbaselib 编译
- [x] `lua_main` open base + dostring
- [x] `/bin/lua` 安装到 LantestFS
- [x] 启动任务输出 Hello
- [x] `run /bin/lua` 输出 Hello
- [x] debug 段剥离，ELF 184,888 B
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告