# M4-23 — FILE 流与 printf 重构

> 状态：**已完成并通过实测**（QEMU + fiotest + 管道/持久化回归）  
> 日期：2026-10-29  
> 关联文档：[M4-libc.md](./M4-libc.md) · [M4-external-commands.md](./M4-external-commands.md)

---

## 1. 目标

1. 实现最小 FILE 流：`fopen/fclose/fread/fwrite/fgetc/fputc/fgets/fputs/fprintf`；
2. `stdin/stdout/stderr` 作为静态 FILE 包装 fd 0/1/2；
3. 用 `printf` 重构 Shell 的打印路径；
4. `ls/cat/echo/head` 改用 `printf/FILE`；
5. fiotest 验证 fopen/fprintf/fwrite/fread/fgetc/fgets/fclose。

---

## 2. FILE 实现

```c
typedef struct FILE {
    int fd;
    int writable;
    int eof;
    int err;
} FILE;
```

- `stdin/stdout/stderr` 是静态 FILE，不 malloc；
- `fopen` 用 `malloc` 分配 FILE；
- 模式支持：`r`、`w`、`r+`、`w+`；
- **暂不支持 `a/a+`**（没有 lseek，无法可靠追加）；
- `fread/fwrite` 直接循环 sys_read/sys_write；
- `fgetc/fputc` 逐字节；
- `fgets` 读到换行/EOF；
- `fprintf` 用 `vsnprintf` 格式化后 fwrite；
- `fflush` 是 no-op（无写缓冲）；
- `feof/ferror/fileno`。

---

## 3. 重构

### Shell

```c
static void print(const char *s)
{
    printf("%s", s);
}
```

- 所有 Shell 文案现在走 `printf`；
- `print_int` 改为 `printf("%d", v)`；
- 行编辑器仍直接 `sys_write` 控制字符/回显。

### ls

```c
printf("%s%s\n", de.d_name, de.d_type == DT_DIR ? "/" : "");
```

### cat

- `FILE *in = stdin` 或 `fopen(path,"r")`；
- `fread` 128 字节循环，`fwrite` 到 `stdout`；
- 无参数仍然读 stdin，管道行为不变。

### echo

```c
printf("%s", argv[i]);
```

### head

- `fgetc(stdin)` + `fputc(c, stdout)`，读到第一行。

---

## 4. fiotest

```c
f = fopen("/fio.txt", "w");
fprintf(f, "hello %d %s\n", 42, "file");
fwrite("abc", 1, 3, f);
fclose(f);

f = fopen("/fio.txt", "r");
fread(buf, 1, 12, f);   // "hello 42 fil"
fgetc(f);               // 'e'
fclose(f);

f = fopen("/fio.txt", "r");
fgets(buf, sizeof(buf), f);  // "hello 42 file\n"
```

实测：

```
[M4] fiotest task: pid=16
fiotest: PASS
[proc] exit: pid=16 name=user-fiotest status=0 -> ZOMBIE
```

- fopen/fprintf/fwrite/fread/fgetc/fgets/fclose 全通过；
- 无 panic/SIGSEGV；
- Shell/管道/持久化测试继续 PASS。

---

## 5. 回归

- `make test` → **PASS**（含 fiotest、自动管道）；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 6. 当前限制

1. 没有 `a/a+` 追加模式（缺少 lseek）；
2. 没有写缓冲，`fflush` 是 no-op；
3. 没有 `fseek/ftell/rewind`；
4. 没有 `fscanf`；
5. FILE 没有缓冲区，fgetc 每次 syscall；
6. 没有错误码/errno 细节；
7. `stdout` 与 `printf` 是两条路径（printf 直接 fd 1）；
8. 头文件 static 实现，每个程序一份。

---

## 7. 下一步

- 增加 lseek → fseek/ftell/append；
- FILE 读写缓冲；
- 浮点 printf；
- 移植 Lua/轻量 C 软件；
- 多块 Journal/group commit（M5-4）；
- SMP + spinlock。

---

## 8. 复现清单

- [x] FILE 结构与 stdin/stdout/stderr
- [x] fopen/fclose/fread/fwrite
- [x] fgetc/fputc/fgets/fputs
- [x] fprintf
- [x] feof/ferror/fileno/fflush
- [x] Shell print → printf
- [x] ls/cat/echo/head 使用 printf/FILE
- [x] fiotest PASS
- [x] 管道 `echo | cat`、`ls | cat` 仍正常
- [x] `make test` / `test-persist` / `test-exception` PASS
- [x] 构建零警告