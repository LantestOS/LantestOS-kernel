# M4-22 — 用户态 stdio/libc 雏形

> 状态：**已完成并通过实测**（QEMU + libctest）  
> 日期：2026-10-29  
> 关联文档：[M4-malloc.md](./M4-malloc.md) · [M4-external-commands.md](./M4-external-commands.md)

---

## 1. 目标

不再只用 `sys_write_str`：

1. `user/libc.h`：printf/snprintf/vsnprintf；
2. 格式化：`%s %c %d %u %x %p %ld %lu %lx`，宽度/零填充；
3. 字符串/内存：`strlen/strcmp/strncmp/strcpy/strncpy/memcpy/memset/memcmp`；
4. `atoi`、`puts`、`putchar`；
5. 不依赖宿主 libc，`-nostdinc` 下纯 freestanding 实现；
6. libctest 验证格式化、atoi、字符串与内存函数。

---

## 2. printf 实现

```c
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_arg(v,t)   __builtin_va_arg(v,t)
```

`vsnprintf` 自己解析格式串：

- 支持 `%0` 标志和数字宽度；
- `%d` 用 `int`，`%ld` 用 `long`；
- `%u/%x` 用 `unsigned int`，`%lu/%lx` 用 `unsigned long`；
- `%p` 输出 `0x...`；
- `%s` 空指针输出 `(null)`；
- 写入 `libc_out` 结构，最终补 `\0`。

`printf`：

```c
char buf[512];
vsnprintf(buf, sizeof(buf), fmt, ap);
sys_write(STDOUT_FD, buf, min(n, 511));
```

- 无 libc、无宿主 `stdarg.h`；
- 单次输出最多 511 字节。

`snprintf` 是 `vsnprintf` 的可变参数包装。

---

## 3. 字符串/内存函数

```c
u64  strlen(const char *);
int  strcmp/strncmp(...);
char *strcpy/strncpy(...);
void *memcpy/memset(...);
int  memcmp(...);
int  atoi(const char *);
int  puts(const char *);
int  putchar(int);
```

全部放在 `user/libc.h`，以 `static` 内联形式提供；用 `__attribute__((unused))` 抑制未被某程序使用的警告。

---

## 4. libctest

```c
strlen("lantest") == 7
strcmp("lantest","lantest") == 0
atoi("-123") == -123
memset/memcpy/strcmp/memcmp
snprintf(buf, ..., "%s %d %08x %c", "ok", -42, 0x1a2b, 'Z')
  => "ok -42 00001a2b Z"
printf("libctest: %s %d %08x %c\n", "format", 7, 0xbeef, '!');
```

实测：

```
[M4] libctest task: pid=15
libctest: format 7 0000beef !
libctest: PASS
[proc] exit: pid=15 name=user-libctest status=0 -> ZOMBIE
```

- 格式化正确；
- atoi/字符串/内存函数正确；
- 无 panic/SIGSEGV；
- `make test` 中 libctest PASS。

---

## 5. 回归

- `make test` → **PASS**（含 libctest）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. printf 单次最多 511 字节，超出被截断；
2. 不支持 `%f/%e/%g` 浮点；
3. 不支持 `*` 宽度/精度、`%n`；
4. 没有 FILE/stdio 流、fopen/fread/fwrite；
5. 没有 locale/UTF-8 宽度处理；
6. `libc.h` 是头文件内 static 实现，每个程序各带一份；
7. 没有 errno/strerror；
8. 没有与 malloc 集成的 `strdup`/`asprintf`。

---

## 7. 下一步

- FILE 流 + fread/fwrite；
- 浮点格式化；
- 多块 Journal transaction/group commit（M5-4）；
- SMP + spinlock；
- 把 printf 用于 Shell 与更多外部程序。

---

## 8. 复现清单

- [x] printf/vsnprintf/snprintf
- [x] `%s %c %d %u %x %p %ld %lu %lx` + 宽度/零填充
- [x] strlen/strcmp/strncmp/strcpy/strncpy
- [x] memcpy/memset/memcmp
- [x] atoi/puts/putchar
- [x] libctest PASS
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告