# M4-26 — Lua 前置：setjmp/longjmp、errno、time、fflush(NULL)、多页栈

> 状态：**前置完成并通过实测**（QEMU + sjtest + fiotest）  
> 日期：2026-10-29  
> 关联文档：[M4-file-writebuffer.md](./M4-file-writebuffer.md) · [M4-libc.md](./M4-libc.md)

---

## 1. 目标

为移植 Lua 补齐运行时基础：

1. 用户栈从 1 页扩到 **16 页 / 64 KiB**；
2. 手写 `setjmp/longjmp`（Lua `pcall/error` 依赖）；
3. 全局 `errno`；
4. `SYS_TIME` 返回开机毫秒数；
5. `fflush(NULL)` 遍历所有 fopen 的 FILE；
6. sjtest 验证长跳转 + 8 KiB 栈局部变量；
7. fiotest 改为 `fflush(NULL)` 验证全局 FILE 链表。

Lua 5.4.7 源码已拉取到 `/tmp/lua-5.4.7`，实际编译留到下一步。

---

## 2. 多页用户栈

```c
#define USER_STACK_PAGES 16u   /* 64 KiB */
```

`task_create_user_elf()` 与 `do_execve()`：

- 从 `USER_STACK_TOP` 向下映射 16 个 4 KiB 页；
- 每页清零；
- 失败时 `paging_destroy_user_space()` 回滚；
- 栈顶仍为 `USER_STACK_TOP - 8`（ABI 对齐）；
- 同时登记 `[USER_STACK_TOP-64K, USER_STACK_TOP)` 匿名 VMA。

以前单页栈只能跑小测试；Lua 的 parser/VM 需要更深栈。

---

## 3. setjmp / longjmp

`user/libc.h`：

```c
typedef u64 jmp_buf[8];
```

保存：

| offset | 寄存器 |
| --- | --- |
| 0 | rbx |
| 8 | rbp |
| 16 | r12 |
| 24 | r13 |
| 32 | r14 |
| 40 | r15 |
| 48 | rsp (setjmp 返回后的 rsp) |
| 56 | rip (返回地址) |

- 裸函数（naked）保存 callee-saved 寄存器、返回地址；
- `longjmp` 恢复寄存器/rsp，`eax=val`（0 则改为 1），`jmp *rip`；
- 不依赖 glibc。

sjtest：

```c
v = setjmp(jump_env);
if (v == 0) {
    jumped = 1;
    longjmp(jump_env, 42);
}
if (v != 42 || jumped != 1) FAIL;
```

实测 `sjtest: PASS`。

---

## 4. 全局 errno

- `user/libc.c` 定义 `int errno;`；
- `libc.h` 声明 `extern int errno;`；
- 所有 user ELF 链接 `user/libc.o`，因此 errno 是进程级全局，而不是每 TU 一份。

---

## 5. SYS_TIME

```c
SYS_TIME = 26
```

- 返回 `timer_ticks() * 10`，即开机毫秒数；
- 用户态 `sys_time_ms()` 包装；
- 后续 Lua `time/clock` 可直接取值。

---

## 6. fflush(NULL)

FILE 增加 `next` 指针和全局链表：

- `fopen` 成功后挂入；
- `fclose` 前先从链表摘除；
- `fflush(NULL)` 遍历链表，对每个流执行 `file_flush()`。

fiotest 改为：

```c
fflush(NULL);
ftell(f) == 17;
```

验证全局 flush 真的把 wbuf 落盘。

---

## 7. 回归

- `make test` → **PASS**（含 sjtest/fiotest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 8. 当前限制

1. Lua 源码已下载但 **尚未编译/链接**；
2. 还没有为 Lua 提供 `<stdint.h>/<stddef.h>/<stdlib.h>/<math.h>/<time.h>` 等移植头；
3. `errno` 还没有由 syscall 失败路径自动填充；
4. `time/clock` 的 libc 包装未实现；
5. 用户栈是固定 64 KiB，没有 guard/demand 扩展；
6. 没有 `sscanf`；
7. 没有 `setvbuf` 行缓冲；
8. setjmp 只保存 callee-saved 寄存器，未保存 FPU/SIMD 状态（当前用户态未使用）。

---

## 9. 下一步

- 建立 `user/lua_port/include` 标准头；
- 实现 multi-TU libc（malloc/stdio/string/strtod/ctype/math stubs）；
- 编译 Lua core（跳过 io/os/loadlib，保留 base/table/string/utf8）；
- `lua_main` + `luaL_loadstring` + pcall；
- 跑 `print("Hello Lantest")`。

---

## 10. 复现清单

- [x] 16 页用户栈
- [x] setjmp/longjmp 裸汇编
- [x] sjtest 长跳转 + 8 KiB 栈数组
- [x] 全局 errno + user/libc.o
- [x] SYS_TIME(sys_time_ms)
- [x] FILE 全局链表
- [x] fflush(NULL)
- [x] fiotest 验证 fflush(NULL)
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告