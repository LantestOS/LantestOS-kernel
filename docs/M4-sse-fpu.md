# M4-27 — SSE/FPU 启用（Lua 的硬件前提）

> 状态：**已完成并通过实测**（QEMU + floattest）  
> 日期：2026-10-29  
> 关联文档：[M4-lua-prereq.md](./M4-lua-prereq.md) · [design.md](./design.md)

---

## 1. 为什么必须做

Lua 默认使用 `double` 数值，编译器会生成 SSE 指令。此前：

- 用户程序统一 `-mgeneral-regs-only`，禁用 SSE；
- 启动代码只设置 CR4.PAE，没有 OSFXSR/OSXMMEXCPT；
- 直接编译 Lua 会报 SSE/浮点相关错误，或运行 #UD。

---

## 2. 启动代码

CR4：

```asm
or $((1 << 5) | (1 << 9) | (1 << 10)), %eax
```

- bit 5：PAE
- bit 9：OSFXSR（FXSAVE/FXRSTOR + SSE）
- bit 10：OSXMMEXCPT（SSE 异常）

CR0：

```asm
and $0xFFFFFFF3, %eax   /* clear EM/TS */
or  $0x80000023, %eax   /* PG|PE|NE|MP */
mov %eax, %cr0
fninit
```

- 清 EM/TS，防止 #NM；
- 置 MP/NE；
- `fninit` 初始化 x87。

---

## 3. 用户编译标志

普通程序继续 `-mgeneral-regs-only`；需要浮点的程序使用：

```make
USER_FLOAT_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS))
```

`floattest` 用该标志编译，Lua 之后同样。

---

## 4. floattest

```c
volatile double a = 1.5;
volatile double b = 2.25;
double c = a * b;
int ok = (c == 3.375);
```

实测：

```
[M4] floattest task: pid=18
floattest: PASS
[proc] exit: pid=18 name=user-floattest status=0 -> ZOMBIE
```

- SSE 指令可执行；
- double 算术正确；
- 没有 #UD/#NM/SIGSEGV。

---

## 5. 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. **没有 FPU 上下文切换**：context switch 不保存/恢复 xmm/x87；当前只有一个浮点任务，整数任务不使用 SSE，所以安全；
2. 两个浮点任务并发会互相破坏 FPU 状态；
3. setjmp/longjmp 不保存 xmm（System V 下 xmm 是 caller-saved，通常没事）；
4. Lua 仍需要移植头 + multi-TU libc + 编译，尚未完成；
5. 没有 FPU 异常处理；
6. 用户 float printf（`%f/%g`）尚未实现，Lua 数字输出需要补。

---

## 7. 下一步

- 真正编译 Lua：
  - `user/lua_port/include` 标准头；
  - multi-TU libc（malloc/stdio/string/strtod/ctype/math/time）；
  - Lua core + base/table/string/utf8；
  - `print("Hello from Lantest OS!")`；
- 若多浮点任务，再做 FXSAVE/FXRSTOR 上下文；
- 实现 `%f/%g`（Lua `tostring` 需要）。

---

## 8. 复现清单

- [x] CR4.OSFXSR/OSXMMEXCPT
- [x] CR0.MP/NE，清 EM/TS
- [x] fninit
- [x] USER_FLOAT_CFLAGS 去掉 mgeneral-regs-only
- [x] floattest double 运算 PASS
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告