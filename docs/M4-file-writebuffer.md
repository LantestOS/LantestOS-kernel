# M4-25 — FILE 写缓冲与真正的 fflush

> 状态：**已完成并通过实测**（QEMU + fiotest）  
> 日期：2026-10-29  
> 关联文档：[M4-lseek-buffered-stdio.md](./M4-lseek-buffered-stdio.md) · [M4-stdio-file.md](./M4-stdio-file.md)

---

## 1. 目标

1. FILE 增加 512 字节写缓冲；
2. `fputc/fwrite` 小块先进缓冲，满 512 或 flush/close 时一次 sys_write；
3. `fflush` 变成真正有用的函数；
4. `fclose/fseek/ftell` 前自动 flush，避免位置错乱；
5. stdin/stdout/stderr 保持无缓冲，保证终端输出即时可见。

---

## 2. FILE 扩展

```c
struct FILE {
    int fd;
    int writable;
    int eof;
    int err;
    int unbuffered;
    unsigned char rbuf[512];
    u64 rpos, rlen;
    unsigned char wbuf[512];
    u64 wpos;
};
```

- 普通文件：`unbuffered=0`；
- `stdin/stdout/stderr`：`unbuffered=1`（stdin 读仍用 rbuf，写路径无缓冲）；
- `file_flush()`：把 `wbuf[0..wpos)` 循环 sys_write，成功后 `wpos=0`。

## 3. 写入路径

`fputc`：

- unbuffered → 直接 sys_write；
- 否则缓冲区满则 flush，再写入 wbuf。

`fwrite`：

- unbuffered → 直接循环 sys_write；
- 否则逐字节进 wbuf，满则 flush；
- 返回写入的 nmemb。

`fprintf/fputs` 都走 `fwrite`，自动享受写缓冲。

## 4. flush 时机

- `fflush(f)` → `file_flush`；
- `fclose(f)` → flush 后 close；
- `fseek/fseek` → 先 flush 再 lseek；
- `ftell` → 先 flush 再 lseek 当前偏移，保证返回值正确；
- `fread/fgetc` → 若 wbuf 非空先 flush（w+ 读写切换）。

## 5. fiotest

```c
fprintf(f, "hello %d %s\n", 42, "file");  // 14
fwrite("abc", 1, 3, f);                   // +3
fflush(f);
ftell(f) == 17;                           // 写缓冲已落盘
fclose(f);

fopen("/fio.txt","a") → fputs("XYZ")     // +3
fseek/ftell/SEEK_END/SEEK_SET 验证
```

实测：

```
[M4] fiotest task: pid=16
fiotest: PASS
[proc] exit: pid=16 name=user-fiotest status=0 -> ZOMBIE
```

- 写缓冲 flush 后 ftell=17；
- 追加/seek 仍正确；
- 没有 panic/SIGSEGV。

## 6. 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制

1. stdout/stderr 无缓冲，没有行缓冲策略；
2. 没有全局 FILE 链表，`fflush(NULL)` 不能 flush 所有流；
3. 没有 O_APPEND 原子保证；
4. 写缓冲固定 512 字节；
5. 没有后台 writeback/异步 I/O；
6. 没有 fscanf；
7. 写错误只置 ferror，没有 errno 细节；
8. 读/写缓冲与 mmap 共享映射没有一致性协调。

---

## 8. 下一步

- 行缓冲/全缓冲策略；
- `fflush(NULL)`；
- fscanf；
- 移植 Lua；
- 多块 Journal/group commit（M5-4）；
- SMP + spinlock。

---

## 9. 复现清单

- [x] FILE wbuf[512] + wpos
- [x] fputc/fwrite 写缓冲
- [x] fflush 真正落盘
- [x] fclose/fseek/ftell 自动 flush
- [x] stdin/stdout/stderr unbuffered
- [x] fiotest fflush+ftell=17 + 追加/seek
- [x] make test/test-persist/test-exception PASS
- [x] 构建零警告