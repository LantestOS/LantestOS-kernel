# M3c Step 3 — copyin/copyout、Fixup 表与 SMAP

> 状态：**M3c-3 已完成并通过实测**（QEMU `-cpu max` + VirtualBox 7.2.16）  
> 日期：2026-10-28  
> 关联文档：[M3c-elf.md](./M3c-elf.md) · [design.md](./design.md)

---

## 1. 本步目标

让"用户指针"成为内核的不可信输入，并在任何情况下都不能由用户程序导致内核 panic：

1. `copy_from_user()` / `copy_to_user()`：范围校验 + 真正的安全拷贝；
2. **异常表（exception table / fixup）**：拷贝期间发生 `#PF` 时跳转到 fixup，返回 `-EFAULT`，而不是 panic；
3. **SMAP**：检测 CPUSMAP 并通过 CR4.SMAP 启用；用户页访问必须被 `stac`/`clac` 显式包住；
4. 内核入口清 AC、拷贝前 `cld`，防止恶意用户用 AC/DF 标志绕过或破坏拷贝；
5. 启动自检 + 用户态恶意指针测试（内核地址、未映射用户页）；
6. 在 QEMU 与 VirtualBox 上验证，包括 SMAP 不可用时的安全降级。

---

## 2. 交付物

| 文件 | 作用 |
| --- | --- |
| `include/uaccess.h` | `copy_from_user/copy_to_user`、范围检查、fixup 接口、SMAP 查询 |
| `mm/uaccess.c` | 范围检查、SMAP 探测/开启、AC/CR4 操作、启动自检、`fixup_exception()` |
| `arch/x86_64/uaccess_asm.S` | `rep movsb` 拷贝 + `.fixup` + `.ex_table` 条目 + SMAP 探测读 |
| `linker.ld` | 新增 `.ex_table` 段与 `__ex_table_start/__ex_table_end`；`.fixup` 归入 `.text` |
| `arch/x86_64/idt.c` | 内核入口清 AC；`#PF` 时先尝试异常表 fixup，不再直接 panic |
| `arch/x86_64/paging.c` / `include/x86_64/paging.h` | 抽出 `paging_destroy_user_mappings()`，供自检清理临时用户映射 |
| `syscall/syscall.c` | `write` 改为 `copy_from_user` 分块拷贝；`wait` 的 status 用 `copy_to_user` |
| `kernel.c` | 启动时调用 `uaccess_init()`，自检失败则 panic |
| `user/parent.c` / `user/userlib.h` | 恶意指针测试：内核地址、未映射用户地址 |
| `Makefile` | 新源文件；QEMU 使用 `-cpu max` 暴露 SMAP；M3c-3 测试判定 |
| `docs/m3c3-vbox.png` | VirtualBox 实测截图 |

---

## 3. 设计

### 3.1 为什么"范围检查"不够

`write(ptr, len)` 的 `ptr` 可以落在 `USER_BASE..USER_STACK_TOP` 内但**没有映射**（例如 `USER_BASE + 0x2000`）。仅检查范围，`memcpy` 会在真正读取时触发 `#PF`，当前异常处理会 panic —— 用户程序即可打崩内核。

解决：**异常表 + fixup**。

### 3.2 异常表与 fixup

汇编拷贝：

```asm
__copy_from_user:              /* rdi=dst, rsi=user src, rdx=len */
    cld                        /* 防御：用户可能设置 DF=1 */
    movq %rdx, %rcx            /* rep movsb 的计数器是 RCX！ */
.Lcfu_start:
    rep movsb
.Lcfu_end:
    xorl %eax, %eax
    ret

.section .fixup, "ax"
.Lcfu_fixup:
    movl $-14, %eax            /* -EFAULT */
    ret

.section .ex_table, "a"
    .quad .Lcfu_start, .Lcfu_end, .Lcfu_fixup
```

链接脚本把表项收进 `.ex_table`，并导出 `__ex_table_start/__ex_table_end`。

`#PF` 处理：

```c
if (regs->int_no == 14 && (regs->cs & 3u) == 0u &&
    fixup_exception(regs))
    return;          /* RIP 已改为 fixup 地址，返回 -EFAULT */
panic("unhandled CPU exception");
```

`fixup_exception()` 在表里查找 `[insn_start, insn_end)` 覆盖当前 RIP 的项，命中则把 `regs->rip` 改成 fixup 地址。`iretq` 回到 fixup 执行 `ret`，就像 `__copy_from_user` 正常返回一样，只是返回值是 `-EFAULT`。

### 3.3 SMAP

- CPUID.(EAX=7):EBX[20] 检测 SMAP；
- 支持则设置 CR4.SMAP（bit 21）；
- `copy_*_user` 内：
  ```c
  stac();
  ret = __copy_from_user(...);
  clac();
  ```
- 内核入口 `isr_handler()` 先 `clac()`：防止用户设置 AC=1 后 `int 0x80`，利用 AC 绕过 SMAP；
- SMAP 不可用时（例如当前 VirtualBox 配置）自动降级为"仅 fixup"，功能不受影响。

### 3.4 启动自检

`uaccess_init()` 在 boot PML4 的 `USER_BASE` 临时映射一页，然后：

1. **SMAP 探测读**：AC=0 直接读用户页，必须触发 `#PF` 并由 fixup 返回 -1；
2. `copy_from_user` 从该页读取 16 字节，校验内容；
3. `copy_to_user` 写 8 字节，通过物理恒等映射校验；
4. `copy_from_user(USER_BASE+0x2000)`（范围内但未映射）：必须返回 `-EFAULT`；
5. `copy_from_user(0x100000)`（内核地址）：范围检查直接返回 `-EFAULT`；
6. 销毁临时用户映射（保留 PML4）。

QEMU 日志：

```
[M3c3] user-access hardening: copyin/copyout + fixup + SMAP
[uaccess] SMAP supported, enabled (CR4.SMAP=1)
[uaccess] self-test: PASS (fixup, copy in/out, SMAP probe blocked)
```

### 3.5 用户态恶意指针测试

`user/parent.c` 在 wait 成功后调用：

```c
sys_write((const void *)0x100000UL, 4);              /* 内核地址 */
sys_write((const void *)(USER_BASE + 0x2000UL), 4);  /* 范围内但未映射 */
```

两者都必须返回负数（-EFAULT），并打印：

```
parent: kernel pointer rejected
parent: unmapped user pointer rejected
```

内核不 panic，父进程继续正常 exit/wait/回收。

---

## 4. 实测记录（2026-10-28）

### 4.1 QEMU `-cpu max`（SMAP 可用）

`make test` → **PASS**：

```
[uaccess] SMAP supported, enabled (CR4.SMAP=1)
[uaccess] self-test: PASS (fixup, copy in/out, SMAP probe blocked)
...
parent: C ELF from Ring 3, waiting for child
child: C ELF from Ring 3, exiting 42
[proc] wait: parent pid=3 reaped child pid=4 status=42
parent: kernel pointer rejected
parent: unmapped user pointer rejected
[reap] orphan: pid=3 name=user-parent exit=0
```

- SMAP 探测读被拦截（`#PF` → fixup → -1）；
- 合法 copyin/copyout 正常；
- 恶意指针均返回 -EFAULT；
- 生命周期、ELF、Ring 3、A/B 调度全部继续工作（A=46/B=45）。

### 4.2 `make test-exception`

→ **PASS**（故意的内核 `#PF` 仍然打印现场并 PANIC；fixup 只覆盖异常表内的拷贝指令）。

### 4.3 VirtualBox 7.2.16（未暴露 SMAP）

```
[uaccess] SMAP not supported by CPU; using fixup only
[uaccess] self-test: PASS (fixup, copy in/out, SMAP off)
parent: kernel pointer rejected
parent: unmapped user pointer rejected
```

- VirtualBox 当前 CPUID 未向 guest 暴露 SMAP → 内核自动降级；
- fixup 仍然完整保护未映射用户页；
- 其它功能（ELF/Ring 3/生命周期）正常。
- 结论：SMAP 是"有则更强"的加固，不是功能依赖。

---

## 5. 踩坑记录

| 问题 | 现象 | 解决 |
| --- | --- | --- |
| `rep movsb` 缺少 `mov %rdx,%rcx` | 自检 pattern 在 byte 1 处不匹配，实际只拷贝了 1 字节 | 在拷贝前显式设置 RCX |
| 用户可能设置 DF=1 | `rep movsb` 反向拷贝，破坏内核内存 | 拷贝前 `cld` |
| 用户可能设置 AC=1 | 试图绕过 SMAP | `isr_handler()` 入口 `clac` |
| 未映射用户页 | 直接 memcpy 会 #PF panic | `.ex_table` + `.fixup` 返回 -EFAULT |
| SMAP 不可用平台 | 执行 `stac/clac` 会 #UD | CPUID 检测，运行时开关；不可用则跳过指令 |
| 自检临时映射 | 需要清理但不能释放 boot PML4 | 新增 `paging_destroy_user_mappings()`（保留 PML4） |
| `wait` 的 status 指针 | 直接写用户内存会受 SMAP 拦截 | 改为 `copy_to_user` |

---

## 6. 已知限制

1. SMAP 依赖 CPU/虚拟化平台暴露；VirtualBox 当前配置不支持（已优雅降级）；
2. 只保护"用户指针访问"，没有按 ELF 段设置只读/NX（用户页仍统一 RWX）；
3. 未启用 **SMEP**（内核执行用户页防护）；
4. fixup 只覆盖 `copy_from_user/copy_to_user`；其它内核直接访问用户指针仍会 panic（这是期望的安全失败）；
5. 没有按需分页；用户页必须预先映射，否则 copy 返回 -EFAULT；
6. `copy_*_user` 一次最多 128 字节（syscall write 分块），没有大块优化；
7. 范围校验是粗粒度的（整个用户窗口），未做 VMA 检查。

---

## 7. 下一步

安全地基完成，可以进入 **M4 VFS**（文件描述符、ramfs、init/shell），或者在进入 M4 前补：

- 按 ELF `p_flags` 设置页权限（只读代码段 + NX）；
- 开启 SMEP（CR4.SMEP）并验证；
- 按需分页 / 更精细的 VMA 校验；
- 大块 copyin/copyout 优化。

---

## 8. 复现清单

- [x] `copy_from_user/copy_to_user` 范围检查 + `rep movsb`
- [x] `.ex_table` / `.fixup`，`#PF` 命中后返回 `-EFAULT` 不 panic
- [x] SMAP 探测、CR4.SMAP 开启、`stac/clac`
- [x] 内核入口 `clac`、拷贝前 `cld`
- [x] 启动自检 PASS（QEMU SMAP on；VirtualBox 降级）
- [x] 用户态恶意指针测试通过
- [x] `make test` / `make test-exception` PASS
- [x] VirtualBox 7.2.16 实测（串口 + 截图）
- [x] 构建零警告
