# M4-31 — 完整 math、Lua 数字格式与 REPL 多行

> 状态：**已完成并通过实测**（QEMU，Lua math + REPL 多行）  
> 日期：2026-10-29  
> 关联文档：[M4-fpu-io-repl.md](./M4-fpu-io-repl.md) · [M4-lua-stdlibs.md](./M4-lua-stdlibs.md)

---

## 1. math 从桩变真

实现位置：`user/lua_port/libc.c`

| 函数 | 实现 |
| --- | --- |
| `sqrt` | `sqrtsd` SSE 指令 |
| `sin/cos/tan` | x87 `fsin/fcos/fptan` |
| `atan2/atan/asin/acos` | `fpatan` + sqrt |
| `exp` | 范围归约 + 24 项 Taylor + ldexp |
| `log` | frexp + atanh 级数，30 项 |
| `log10` | log / ln10 |
| `pow` | 整数幂精确连乘；否则 exp(y*log(x)) |
| `fabs/floor/ceil/fmod/ldexp/frexp/isinf/isnan` | C 实现 |

实测：

```lua
print(math.floor(3.9), math.sqrt(9))
print(math.sin(0), math.cos(0), math.exp(0), math.log(1))
print(2 ^ 10, tostring(3.14), string.format('%.2f', 3.14159))
```

输出：

```
3	3.0
0.0	1.0	1.0	0.0
1024.0	3.14	3.14
```

- `log(1)=0`，`log(2)=0.693147...`，`log(10)=2.302585...`；
- `exp(1)=2.71828182845904`；
- `2^10` 走整数精确路径，输出 `1024.0`，不再是 1024.0000000000018。

`%g` 默认精度 14，`%.2f` 固定两位；`tostring(3.14)` 得到 `3.14`。

---

## 2. REPL 多行

`run_repl` 现在有 512 字节代码缓冲：

- `luaL_loadstring(buffer)`；
- 错误信息包含 `<eof>` → 视为未完成，打印 `>> ` 继续累积；
- 完成后 `lua_pcall` 执行；
- 其他语法错误立即报告并清空；
- `quit/exit` 仅在第一行生效。

实测：

```
lua> function add(a,b)
>> return a+b
>> end
lua> print(add(2,3))
5
lua> os.exit(0)
```

`MULTILINE_REPL_TEST: PASS`。

---

## 3. 回归

- `make test` → **PASS**（task A=34, B=34）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 4. 当前限制

1. `%e` 科学计数法仍未实现，`%f/%g` 舍入/边界仍是简化版；
2. `math` 的 exp/log 是级数近似，极端范围精度有限；
3. REPL 只按 `<eof>` 判断续行，不支持跨行字符串等所有情况；
4. REPL 没有历史/补全；
5. `os.date/strftime` 仍是桩；
6. FPU 上下文已保存，但 setjmp/longjmp 不保存 xmm；
7. 用户栈 64 KiB；
8. malloc 不合并。

---

## 5. 下一步

- 完整 `%e`、舍入、`%a`；
- `os.date/strftime`；
- SMP 多核唤醒；
- 多块 Journal / Group Commit。

---

## 6. 复现清单

- [x] sqrt/sin/cos/tan/atan2
- [x] exp/log/log10/pow
- [x] log(1)=0，log(2/10) 正确
- [x] 2^10 = 1024.0
- [x] tostring(3.14)、%.2f
- [x] REPL 多行 function/end
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告