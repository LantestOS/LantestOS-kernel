# M4-24 — lseek、FILE 读缓冲与追加模式

> 状态：**已完成并通过实测**（QEMU + fiotest）  
> 日期：2026-10-29  
> 关联文档：[M4-stdio-file.md](./M4-stdio-file.md) · [M4-libc.md](./M4-libc.md)

---

## 1. 目标

1. 内核新增 `lseek(fd, offset, whence)`；
2. FILE 增加 512 字节读缓冲；
3. 实现 `fseek/ftell/rewind`；
4. 支持 `fopen` 的 `a` / `a+` 追加模式；
5. fiotest 验证追加、ftell、SEEK_END、SEEK_SET。

---

## 2. 内核 lseek

```c
SYS_LSEEK = 25
lseek(fd, offset, whence)
```

- `SEEK_SET (0)`：`pos = offset`；
- `SEEK_CUR (1)`：`pos = f_pos + offset`；
- `SEEK_END (2)`：`pos = inode->i_size + offset`；
- 越界/负值/非 VFS 文件 → `-EINVAL`；
- 成功写回 `file->f_pos` 并返回新位置。

因为 VFS 的 read/write 都以 `f_pos` 为当前偏移，lseek 后立即生效。

---

## 3. FILE 读缓冲

```c
struct FILE {
    int fd;
    ...
    unsigned char rbuf[512];
    u64 rpos;
    u64 rlen;
};
```

- `file_fill()`：缓冲区空时一次 `sys_read(fd, rbuf, 512)`；
- `fgetc`：优先从 rbuf 取；
- `fread`：
  - 先消费 rbuf；
  - 大块直接 sys_read 到目标；
  - 小块通过 rbuf 读；
- `fseek`：清空 rbuf；
- `ftell`：`sys_lseek(fd,0,SEEK_CUR) - (rlen-rpos)`，补偿预读但未消费的字节。

## 4. 追加模式

`fopen` 新支持：

- `a`：`O_WRONLY|O_CREAT`，打开后 `lseek(fd,0,SEEK_END)`；
- `a+`：`O_RDWR|O_CREAT`，同样 seek 到末尾；
- `w/w+` 仍 O_TRUNC；
- `r/r+` 不变。

当前是单进程单 fd 的“seek 到末尾”语义，没有 O_APPEND 原子保证（单 CPU 下足够）。

---

## 5. fiotest

在原有 fopen/fprintf/fwrite/fread/fgetc/fgets 基础上新增：

```c
f = fopen("/fio.txt", "a");
fputs("XYZ", f);
fclose(f);

f = fopen("/fio.txt", "r");
fseek(f, 0, SEEK_END);
ftell(f) == 20;
fseek(f, -3, SEEK_END);
fgetc(f) == 'X';
fseek(f, 6, SEEK_SET);
fgetc(f) == '4';
```

实测：

```
[M4] fiotest task: pid=16
fiotest: PASS
[proc] exit: pid=16 name=user-fiotest status=0 -> ZOMBIE
```

- 追加内容成功；
- ftell 返回 20（14 + abc + XYZ）；
- SEEK_END 偏移读取 'X'；
- SEEK_SET 读取 '4'；
- 读缓冲 + seek 组合没有错位。

---

## 6. 回归

- `make test` → **PASS**；
- `make test-persist` → **PASS**；
- `make test-exception` → **PASS**；
- 构建零警告。

---

## 7. 当前限制

1. 没有写缓冲，fflush 仍是 no-op；
2. 没有 O_APPEND 原子保证；
3. `fseek` 对管道/console 无效；
4. 没有 fscanf；
5. 读缓冲 512 字节，固定；
6. `ftell` 对非 VFS 文件返回 -1；
7. 没有 fgetpos/fsetpos；
8. 没有 mmap 与 FILE 的缓存一致性协调。

---

## 8. 下一步

- FILE 写缓冲；
- fscanf；
- 移植 Lua；
- 多块 Journal/group commit（M5-4）；
- SMP + spinlock。

---

## 9. 复现清单

- [x] SYS_LSEEK 25，SEEK_SET/CUR/END
- [x] FILE rbuf/rpos/rlen
- [x] fgetc/fread 读缓冲
- [x] fseek/ftell/rewind
- [x] fopen a/a+
- [x] fiotest 追加 + ftell + SEEK_END/SET
- [x] 管道/持久化回归 PASS
- [x] 构建零警告