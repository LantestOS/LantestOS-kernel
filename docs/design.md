# 兰特斯特操作系统（Lantest OS）设计文档

> 版本：v0.58（M4-47b：x2APIC ICR 排空 + wait 唤醒窗口原子化）（M4-46：堆/VFS/virtio 锁、deferred reap、多 AP 压测）（M4-45：TLB/锁）（M4-44：IPI 0xFE 唤醒）（M4-43：active_cpus）（M4-42：三 AP 并发用户任务）（M4-41：AP 本地调度）（M4-40：AP 用户 smoke）（M4-39b：asm swapgs）（M4-39：swapgs）
> 日期：2026-10-27  
> 当前状态：M1 引导 ✅、M2 中断/定时器/物理内存 ✅（均通过 QEMU 实测）  
> 文档： [M1-boot.md](./M1-boot.md) · [M2-core.md](./M2-core.md)

---

## 1. 项目定位

**Lantest OS 是一个从零开始、独立实现的 x86_64 单核宏内核。**

它参考 Linux 的**架构思想**（宏内核、VFS、进程模型、系统调用、设备模型、调度器），但：

- 不复制 Linux 源码；
- 不基于 Linux 开发；
- 不依赖 Linux ABI；
- 不引用 Linux 头文件、内核 API 或内部结构体布局。

目标不是马上做出一个"完整 OS"，而是**分阶段**建成一个边界清晰、可验证、可演进的自研内核。

### 1.1 目标

1. 在 QEMU 与 VirtualBox 等虚拟机（以及物理机）上从零引导到 64 位 C 内核。
2. 逐步建立：中断/异常、物理与虚拟内存、进程与调度、系统调用、VFS、设备模型。
3. 内核 ABI、系统调用号、对象布局、构建系统**全部自行定义**。
4. 每个里程碑都有可复现的构建/运行/调试命令和文档。

### 1.2 非目标（近期明确不做）

- 兼容 Linux 系统调用 / Linux 二进制程序（未来只做**可选**的用户态兼容层）。
- SMP、多核调度、实时性保证。
- 完整 POSIX、图形界面、网络协议栈、复杂驱动生态。
- 生产可用性与安全性（现阶段以学习和验证架构为主）。

---

## 2. 三条边界原则

| 原则 | 含义 | 落地约束 |
| --- | --- | --- |
| **代码独立** | 不复制 Linux 源码，只借鉴公开的设计思想 | 内核代码全部自行编写；不搬运 Linux 代码片段；构建系统不用 Kbuild |
| **ABI 自定义** | 初期不兼容 Linux syscall | 系统调用号从自定义区间开始、自定义寄存器约定、自定义错误码；未来兼容层是可选模块 |
| **先单核单 CPU** | 从单核、简单分页开始 | 暂不引入 SMP、spinlock 体系、复杂缓存一致性；先跑通单 CPU 路径 |

### 2.1 什么算"越界"

- 直接复制、翻译或改写 Linux 源码；
- 使用 Linux 内核头文件或内部 API；
- 照搬 Linux 的 `task_struct`、`inode`、`file` 等结构体字段布局；
- 复用 Linux 的 syscall 编号或 `syscall` ABI；
- 依赖 Linux 的构建系统或配置系统。

### 2.2 可以借鉴的公开知识

- 操作系统原理教材与公开论文（进程、虚拟内存、文件系统分层、调度算法）；
- 公开的体系结构规范（Intel SDM、AMD APM、ACPI 等）；
- 公开的引导协议规范（Multiboot2）与设备数据手册（16550 UART、VGA、8259A、8254、Multiboot2）；
- 概念级借鉴：VFS 分层、task/PCB 模型、RR/优先级/CFS 思想。

> 结论：**概念可以借鉴，代码与 ABI 必须自研。**

---

## 3. 目标平台与开发环境

### 3.1 目标平台

| 项目 | 选择 | 说明 |
| --- | --- | --- |
| 架构 | x86_64（long mode） | 用户选定。M1 已解决 GDT/分页/长模式跳板，M2 已解决 IDT/TSS/PIC/PIT |
| CPU 模式 | 单核、单 CPU | SMP 留待后期 |
| 固件/引导 | BIOS（SeaBIOS）+ GRUB2 + Multiboot2 | 当前为 BIOS 引导；UEFI 直启留待以后 |
| 虚拟机 | VirtualBox（主要）、QEMU（调试/自动化） | **两者均已实测**（VirtualBox 7.2.16 / QEMU 10.2.1）；QEMU + GDB 用于深度调试 |
| 显示 | VGA 文本模式 80×25 @ `0xB8000` | 所有阶段的基础可视化输出 |
| 日志 | COM1 16550 UART @ `0x3F8`，115200 8N1 | 探测到 UART 才输出，避免无串口环境（VirtualBox）卡死 |

### 3.2 开发环境（实测）

| 组件 | 版本 |
| --- | --- |
| 宿主系统 | Windows 11 + WSL2 Ubuntu 26.04.1 LTS（内核 6.18.33.1-microsoft-standard-WSL2） |
| 项目位置 | **WSL2 家目录 `~/lantest`**（M2 起从 `/mnt/c` 迁移，避免跨系统 I/O 拖慢构建） |
| 编译器 | GCC 15.2.0（`-std=gnu11`） |
| 链接器 | GNU ld 2.46（binutils） |
| 构建工具 | GNU Make 4.4.1 |
| 引导镜像工具 | GRUB 2.14（`grub-pc-bin`）+ xorriso 1.5.6 + mtools 4.0.49 |
| 虚拟机 | QEMU 10.2.1（SeaBIOS 1.17.0）、VirtualBox 7.2.16；WSLg 提供图形窗口 |
| 调试器 | GDB 17.1 |

宿主硬件（仅作为开发机，不参与内核实现）：

- CPU：Intel Core Ultra 5 225H（14 核 14 线程，基准 1.7 GHz 级别）
- 内存：32 GB DDR5-5600（双通道）
- 存储：约 1 TB NVMe SSD
- 显卡/加速器：Intel Arc 集成显卡 + Intel AI Boost NPU
- 虚拟化：已在固件中启用

---

## 4. 总体架构

采用**简化宏内核**路线：

```
+--------------------------------------------------------------+
|                        用户程序 (M3+)                         |
+--------------------------------------------------------------+
|              系统调用接口 syscall (自定义 ABI)                 |
+--------------------------------------------------------------+
|  VFS / 进程管理 / 内存管理 / 调度器 / 日志 / panic            |
+--------------------------------------------------------------+
|  设备模型：串口 UART、VGA、PIT 定时器、块设备 ...              |
+--------------------------------------------------------------+
|  体系结构层 arch/x86_64：boot、GDT/TSS、IDT、分页、中断框架    |
+--------------------------------------------------------------+
|  Bootloader：GRUB2 (Multiboot2) / UEFI(后续)                   |
+--------------------------------------------------------------+
|  硬件 / 虚拟机：QEMU、VirtualBox、VMware、物理机              |
+--------------------------------------------------------------+
```

内核组件规划（M1/M2 已完成加粗部分）：

| 模块 | 职责 | 状态 |
| --- | --- | --- |
| **boot / arch** | **引导跳板、长模式、GDT/TSS、IDT、异常/IRQ 框架** | **M1 + M2 ✅** |
| **libk** | **自带类型、`kprintf`、`panic`**，后续 string/mem/atomic | **M2 ✅** |
| **drivers** | **UART（轮询 + 存在性探测）**、**VGA 文本控制台**、**PIC/PIT** | **M1 + M2 ✅** |
| **mm** | **COW、VMA、demand、file-backed/ELF lazy、Page Cache（dirty/LRU/MAP_SHARED/msync）** | **M4-21 ✅** |
| **loader** | **ELF64 解析、PT_LOAD 按页映射、共享页处理、`.bss` 清零、`e_entry` 启动** | **M3c-2 ✅** |
| **sched** | **task、switch_to、环形 RR、PIT 抢占、CR3/TSS.RSP0、Ring 3、进程状态机（WAITING/ZOMBIE）与回收** | **M3c-1 ✅** |
| **syscall** | **int 0x80：write/exit/getpid/wait；所有用户指针经 copyin/copyout** | **M3c-3 ✅** |
| **uaccess** | **copy_from_user/copy_to_user、`.ex_table` fixup、SMAP、AC/DF 防护** | **M3c-3 ✅** |
| **fs** | **fd/VFS、ramfs、LantestFS v2（间接块、Journaling、备份超级块）** | **M5-3b ✅** |
| **exec** | **execve 用户栈 argv/envp、naked `_start`、外部 `/bin/sh` `/bin/ls` `/bin/cat` `/bin/echo`** | **M4-9 ✅** |
| **input** | **PS/2 键盘 IRQ1、set 1 扫描码、shift、256B 环形缓冲、console 键盘优先/串口兜底** | **M4-6 ✅** |
| **lineedit** | **console 原始字节、用户态回显/退格、8 条历史 ↑/↓、Tab 命令与路径补全** | **M4-7 ✅** |
| **userlib** | **Lua/FPU/stdio；SMP AP online、per-CPU GS、swapgs、TSS/IDT/timer** | **M4-39 基础 ✅** |
| **ipc** | **pipe 4 KiB 环形缓冲、阻塞读写、EOF/EPIPE、dup2、最多 8 段管道（fd 全关闭）** | **M4-11 ✅** |
| **signal** | **SIGPIPE/SIGINT/SIGTSTP/SIGCONT/SIGSEGV、PGID 广播、STOPPED、waitpid、jobs/fg/bg** | **M4-16 ✅** |
| **block** | **PCI 枚举、legacy virtio-blk、virtqueue/DMA、512B 扇区读写、持久化自检** | **M5-1 ✅** |
| vfs | superblock / inode / dentry / file / mount、ramfs | M4 |
| block/fs | virtio-blk、自研简单文件系统 | M5 |
| smp/net/gui | 多核、网络、图形 | 远期 |

---

## 5. 子系统设计要点

### 5.1 引导与体系结构层

**M1（已完成）**：GRUB/Multiboot2 → 32 位保护模式 → 自建长模式跳板（CPUID、页表、PAE、EFER.LME、GDT、far jump）→ `kernel_main()`。

**M2（已完成）**：

- **GDT/TSS**：在 C 中重建 GDT（null / 64 位代码 / 数据 / TSS），并用 `ltr 0x18` 加载 TSS。M3b-1 起 TSS.RSP0 在每次任务切换时更新为该任务的内核栈顶（Ring 3 中断入口已就绪）。
- **IDT**：256 个门，0–31 为 CPU 异常，32–47 为 PIC 重映射后的 IRQ，48–255 指向默认桩。
- **统一汇编桩**：保存 15 个通用寄存器 + 中断号/错误码，把栈帧作为 `registers_t *` 交给 C；返回时恢复寄存器并 `iretq`。
- **异常处理**：打印向量名、错误码、RIP/CS/RFLAGS/RSP/SS；`#PF` 额外打印 CR2；随后 `panic`。
- **PIC/PIT**：8259A 重映射到 `0x20–0x2F`，8254 通道 0 以 100 Hz 触发 IRQ0，处理完发 EOI。
- **中断开关**：初始化全部完成后执行 `sti`，主循环 `hlt` 等待中断。

### 5.2 内存管理

- **M1**：恒等映射前 1 GiB（2 MiB 大页）；无分配器。
- **M2（已完成）**：解析 Multiboot2 memory map；对前 1 GiB 实现 **4 KiB 页的 Bitmap 物理页分配器**，保留低 1 MiB、内核镜像与 Multiboot2 info；提供 `pmm_alloc_page()` / `pmm_free_page()` 与自检。
- **M3a（已实现）**：`pmm_alloc_pages()` 连续页分配 + `kmalloc/kfree` 自由链表内核堆（区域/块头/分割/合并/自检）。
- **M3b-1（已实现）**：每任务独立 PML4（复制内核映射）、CR3 切换、per-task TSS.RSP0、主动 CR3 自检。
- **M3b-2（已实现）**：用户空间映射（PML4[1] + 各级 U/S 位）、用户栈、Ring 3 `iretq`、`int 0x80` syscall（write/exit/getpid）、内嵌用户程序。
- **M3c-1（已实现）**：`exit` → ZOMBIE、`wait()` 阻塞并回收子进程、空闲任务回收孤儿；用户页/PML4/内核栈完整释放。
- **M3c-2（已实现）**：ELF64 加载器（三遍扫描、按页处理共享页）、C 用户程序、`userlib.h`。
- **M3c-3（已实现）**：`copyin/copyout`、页错误异常表 fixup（-EFAULT 不 panic）、SMAP（支持时启用）、AC/DF 防护。
- **下一步**：M4 VFS（文件描述符、ramfs、init/shell），或先补 ELF 段权限/SMEP/按需分页。
- 原则：页表操作、地址空间切换、缺页处理全部自研，不依赖 Linux 的 `mm_struct` 等概念布局。

### 5.3 进程与调度（M3）

> **M3a 已实现**：`struct task`、每任务 16 KiB 内核栈、`switch_to` 汇编、环形 RR 调度与 PIT 100 ms 抢占；两个 Ring 0 内核线程交替输出已通过 QEMU/VirtualBox 实测。
>
> **M3b-1 已实现**：每任务 PML4 + CR3 切换 + TSS.RSP0 per-task，CR3 主动自检 PASS；Ring 3 与 syscall 是下一步。
>
> **M3b-2 已实现**：Ring 3 用户任务（iretq）、用户页表 U/S 映射、`int 0x80` syscall，且 Ring 3 期间 PIT 中断经 TSS.RSP0 正确切栈。
>
> **M3c-1 已实现**：进程状态机（READY/RUNNING/WAITING/ZOMBIE）、`sys_exit` 僵尸化、`sys_wait` 阻塞与回收、孤儿回收器；父/子两个 Ring 3 进程已实测。
>
> **M3c-2 已实现**：真正的 ELF64 用户程序（`PT_LOAD` 按页映射、共享页安全、`.bss` 清零），C 语言 Ring 3 程序已运行；页表释放与生命周期回收继续有效。
>
> **M3c-3 已实现**：用户指针统一走 copyin/copyout；未映射用户页由异常表 fixup 优雅返回 `-EFAULT`；QEMU 下 SMAP 探测读被拦截，VirtualBox 不支持时自动降级。

- 自研任务结构（暂名 `lt_task`）：PID、状态、内核栈、保存上下文、地址空间引用、打开文件表、父子关系、统计信息。
- 上下文切换：先汇编保存/恢复 callee-saved 寄存器与 RSP；后续扩展 FPU/SIMD 状态。
- 调度器演进：**M3 轮转（RR）** → 优先级多级队列 → 参考 CFS 思想的虚拟运行时间（自研实现）。
- M2 的 PIT 中断是抢占式调度的基础。

### 5.4 中断与异常（M2 已实现）

- 异常向量 0–31 全部有桩；对自带错误码的异常（8/10/11/12/13/14/17/21/29/30）不重复压栈。
- IRQ 0–15 → 向量 32–47；PIC 主/从片级联（IRQ2）。
- 当前所有异常都进入 `panic`（打印现场后停机），暂不做恢复或按需分页。
- 后续（M3）：缺页异常用于按需分页；`syscall`/`sysret` 或自研中断门作为系统调用入口；M6：APIC/IOAPIC 替代 PIC。

### 5.5 VFS（M4-1 已起步）

> **M4-1 已起步**：每任务 fd 表 + `file`/`file_operations` + console 字符设备；`write(fd, buf, len)` 已经走 VFS 接口。inode/dentry/superblock/ramfs 已由 M4-2 完成。
>
> **M4-2 已实现**：`superblock`/`inode`/`dentry` 自研骨架、绝对路径解析、ramfs 4 KiB 块链、`open/read/close`，用户态 filetest 已完成大小文件读写与 EOF 验证。
>
> **M4-3 已实现**：COM1 行输入 + `console_read`、`mkdir`、目录 `read`（dirent）、`O_TRUNC`，以及可交互的内置命令 shell（init，pid=3）。
>
> **M4-4 已实现**：fork 深拷贝用户空间 + `fork_regs` 子进程返回、execve 从 ramfs 加载 ELF、unlink + `i_open_count` delete-on-close；shell 增加 `run`/`rm`，`/bin/hello` 可被执行。

分层设计（概念参考公开 OS 教材，结构自研）：`lt_superblock`、`lt_inode`、`lt_dentry`、`lt_file`、`lt_mount`；进程内文件描述符表把整数句柄映射到 `lt_file`。首个文件系统：内存文件系统（ramfs/tmpfs）。

### 5.6 设备模型

- 字符设备：UART（M1 轮询 + M2 存在性探测；后续改为中断驱动）。
- 定时器：PIT 100 Hz（M2）；后续 APIC timer。
- 块设备：virtio-blk（M5）。
- 设备注册/匹配机制自研；M2 阶段仍是静态初始化。

### 5.7 系统调用 ABI（M3b-2 已落地）

> **M3b-2 已落地第一版**：`int 0x80`（IDT 门 DPL=3），`rax`=调用号，`rdi/rsi/rdx` 为参数；已实现 `write`、`exit`、`getpid`，调用号从 1 开始（与 Linux 无关）。

| 项目 | 约定 |
| --- | --- |
| 触发指令 | `syscall`（M3；早期可先用自研中断号） |
| 调用号 | `RAX`，从 `0x100` 开始（避开 Linux 号段，明确表示独立 ABI） |
| 参数 | `RDI, RSI, RDX, R10, R8, R9`（最多 6 个） |
| 返回值 | `RAX`；错误用负值（`-LT_E*`），与 Linux errno 编号无关 |
| 指针 | 用户指针必须校验；通过 `copyin/copyout` 拷贝 |
| 版本 | 分发表带版本号，允许渐进扩展 |

首批调用（草案）：`exit`、`write`、`read`、`open`、`close`、`brk`、`mmap`、`getpid`、`yield`、`wait`。

### 5.8 日志与 panic

- `kprintf` 同时输出到 UART（存在时）与 VGA；无 libc/libgcc。
- `panic()` 打印横幅后 `cli; hlt` 永久停机；异常处理先打印完整现场再调用 `panic`。
- 后续：分级日志（DEBUG/INFO/WARN/ERROR/PANIC）、更完整的寄存器/栈回溯。

---

## 6. 数据布局与 ABI 原则

1. 面向用户态的结构体**不透明**，或显式固定布局并用静态断言校验。
2. 使用固定宽度整数类型（`u8/u16/u32/u64`），内核内部不用 `long` 表达磁盘/协议字段。
3. 句柄（fd、pid 等）用整数，不向用户态暴露内核指针。
4. 系统调用表版本化；新增调用只能追加。
5. 内核与用户态之间只传递 POD 数据，禁止直接解引用用户指针（M3 起强制 `copyin/copyout`）。
6. 中断栈帧布局由汇编与 `registers_t` 共同定义，并用 `static_assert(sizeof(registers_t) == 22 * 8, ...)` 在编译期锁定。

---

## 7. 内存布局（M2 实测）

| 物理地址 | 内容 |
| --- | --- |
| `0x000B_8000` | VGA 文本缓冲区 |
| `0x0010_0000` | `_kernel_start`：Multiboot2 头 → `.text` → `.rodata` → `.data` → `.bss` |
| `0x0010_0000 – 0x0011_9000` | M2 内核镜像（`_kernel_end = 0x119000`，含 IDT 4 KiB、PMM bitmap 32 KiB、TSS 栈 16 KiB 等） |
| `0x0012_2AF0` | GRUB 放置的 Multiboot2 info（实测，位置由 GRUB 决定） |
| `0x0000_0000 – 0x0000_09FC00` | 低端可用内存（被 PMM 保留低 1 MiB） |
| `0x0010_0000 – 0x1FFE_0000` | QEMU `-m 512M` 下的可用 RAM（PMM 管理） |

- **虚拟地址空间**：`[0, 1 GiB)` 恒等映射，页大小 2 MiB，`CR3` 仍指向 boot.S 中的 PML4。
- **物理页分配器**：位图 `262144` 位（32 KiB），管理 1 GiB；M2 实测 `free=130759` 页（约 511 MiB 可用，扣除内核/低端/MB2 info）。
- **限制**：只管理前 1 GiB；超出部分（若虚拟机给更多内存）暂时忽略并留待 M3 虚拟内存阶段处理。

---

## 8. 目录结构（M2 实际）

```
lantest/
├── boot.S                     # Multiboot2 头 + 长模式跳板 + 早期 GDT/栈
├── kernel.c                   # kernel_main：按顺序初始化各子系统
├── linker.ld                  # 1 MiB 加载、_kernel_start/_kernel_end
├── grub.cfg                   # GRUB2 Multiboot2 引导项（输出仅 VGA）
├── Makefile                   # 构建 / ISO / QEMU / GDB / test / test-exception
├── include/
│   ├── types.h io.h kprintf.h uart.h vga.h panic.h multiboot2.h pmm.h kmalloc.h task.h syscall.h elf.h uaccess.h fs.h console.h errno.h vfs.h ramfs.h lantestfs.h
│   └── x86_64/{gdt,idt,isr,irq,paging,usermode}.h
├── libk/
│   ├── kprintf.c              # 自研格式化输出（无 libc/libgcc）
│   └── panic.c
├── drivers/
│   ├── uart.c                 # COM1 轮询 + 存在性探测
│   ├── vga.c                  # 80x25 文本控制台
│   ├── console.c              # console 字符设备（fd 0/1/2，COM1 行输入）
│   ├── pci.c                  # legacy PCI 配置空间枚举
│   ├── keyboard.c             # PS/2 键盘 IRQ1 + 环形缓冲（M4-6）
│   └── virtio_blk.c           # virtio-blk：队列/DMA/扇区读写（M5-1）
├── fs/
│   ├── file.c                # file 对象 + 每任务 fd 表 + fd 生命周期
│   ├── lantestfs.c           # M5-2：LantestFS 持久化文件系统（M5-3a 间接块）
│   ├── pipe.c               # M4-8：pipe 环形缓冲 + 读写等待队列
│   ├── vfs.c                 # 路径解析、vfs_open（O_CREAT）
│   └── ramfs.c               # ramfs：inode/dentry + 4 KiB 块链读写
├── arch/x86_64/
│   ├── gdt.c                  # GDT + 64 位 TSS + ltr
│   ├── idt.c                  # 256 门 IDT、异常分派、现场打印
│   ├── isr_asm.S              # 48 个异常/IRQ 桩 + 统一入口 + 跳表
│   ├── switch.S               # switch_to 上下文切换 + 首次进入 trampoline
│   ├── paging.c               # 每任务 PML4 创建、CR3 读写
│   ├── usermode.S             # iretq 进入 Ring 3
│   ├── uaccess_asm.S          # rep movsb 拷贝 + .ex_table fixup
│   └── irq.c                  # PIC 重映射、PIT 100 Hz、EOI、tick
├── mm/
│   ├── multiboot2.c           # tag 遍历、memory map 查找
│   ├── pmm.c                  # 物理页 Bitmap 分配器 + 连续页分配 + 自检
│   ├── kmalloc.c              # 自由链表内核堆（region/块头/合并）
│   ├── elf.c                  # ELF64 按页加载（共享页/BSS/回滚）
│   ├── uaccess.c              # copyin/copyout、fixup、SMAP 自检
│   └── vma.c                  # M4-14：vm_area 链表 + mmap/fork 管理
├── sched/
│   ├── scheduler.c            # task 结构、环形 RR、PIT 抢占
│   └── signal.c               # M4-10：最小信号投递 + SIGPIPE
├── syscall/
│   └── syscall.c              # int 0x80 分发（write/exit/getpid）
├── user/
│   ├── parent.c child.c filetest.c hello.c unlinktest.c  # C 语言测试程序
│   ├── ls.c cat.c echo.c head.c crt.h  # 外部命令与 naked 启动头
│   ├── cowtest.c mmaptest.c  # M4-14：COW 与 mmap 自检
│   ├── init.c                 # init：fork/exec /bin/sh，退出后自动重启（M4-6）
│   ├── shell.c                # /bin/sh：行编辑器/history/Tab + 内置命令 + run/rm
│   ├── userlib.h              # int 0x80 syscall 内联封装
│   └── user.ld                # ELF 链接脚本（PHDRS 页重叠测试）
M4-ap-timer-iret.md、M4-swapgs.md、截图
└── .gitignore / .gitattributes
```

演进目标（M3b 起继续拆分）：`mm/vm.c`、`syscall/`、`fs/`、`user/`（`sched/` 已在 M3a 落地）。

---

## 9. 里程碑

| 里程碑 | 目标 | 状态 |
| --- | --- | --- |
| **M1 引导** | GRUB/Multiboot2 → 64 位 C 内核；UART + VGA + kprintf；QEMU run/debug | **✅ 完成** |
| **M2 中断与内存** | GDT/TSS、256 门 IDT、异常/IRQ 桩、PIC 重映射、PIT 100 Hz、Multiboot2 内存图、物理页 Bitmap 分配器 | **✅ 完成** |
| **M3a 内核堆与调度** | kmalloc/kfree、连续页分配、`struct task`、`switch_to`、环形 RR、PIT 抢占、两个内核线程 | **✅ 完成** |
| **M3b-1 页表切换** | 每任务 PML4、CR3 切换、per-task TSS.RSP0、CR3 主动自检 | **✅ 完成** |
| **M3b-2 用户态** | Ring 3（`iretq`）、用户页表/栈（U/S 位）、`int 0x80` syscall、内嵌用户程序 | **✅ 完成** |
| **M3c-1 进程生命周期** | 状态机、exit→ZOMBIE、wait 阻塞/回收、孤儿回收器、用户页与页表释放 | **✅ 完成** |
| **M3c-2 ELF 加载器** | ELF64 解析、`PT_LOAD` 按页映射、共享页/BSS、`e_entry`、C 用户程序 | **✅ 完成** |
| **M3c-3 用户内存安全** | copyin/copyout、`.ex_table` fixup、SMAP、AC/DF 防护、恶意指针测试 | **✅ 完成** |
| **M4-1 fd 表与 console** | 每任务 fd 表、`file`/`file_operations`、console 设备、`write(fd,...)` | **✅ 完成** |
| **M4-2 VFS 核心与 ramfs** | `superblock`/`inode`/`dentry`、绝对路径解析、ramfs 块链、open/read/close、filetest | **✅ 完成** |
| **M4-3 shell** | COM1 输入 + console_read、mkdir、目录 read/dirent、O_TRUNC、init/shell | **✅ 完成** |
| **M4-4 fork/exec/unlink** | 深拷贝 fork、execve 重建地址空间、unlink + delete-on-close、shell run/rm | **✅ 完成** |
| **M5-1 virtio-blk** | PCI 枚举、legacy virtio 握手、virtqueue/DMA、扇区读写、boot_count 持久化 | **✅ 完成** |
| **M5-2 LantestFS** | 4 KiB 块布局、超级块/inode/位图/目录、8 块缓存、format/mount、持久化文件 | **✅ 完成** |
| **M5-3a 间接块** | 12 直接块 + 1 级间接块（~4.2 MiB）、bmap、cache 淘汰修复 | **✅ 完成** |
| M5-3b 备份/日志 | 双超级块、fsck、journaling | 计划中 |
| **M4-5 argv/envp** | execve 参数栈、naked `_start`、Shell run 参数、持久化 `/bin/hello` | **✅ 完成** |
| **M4-6 外部 shell + 键盘** | `/bin/sh` 持久化、init fork/exec + 重启、PS/2 IRQ1 环形缓冲 | **✅ 完成** |
| **M4-7 Shell 行编辑器** | 原始字节 console、回显/退格、8 条历史 ↑/↓、Tab 命令/路径补全、动态 RW 链接脚本 | **✅ 完成** |
| **M4-8 管道** | pipe/dup2、阻塞等待、Shell `cmd1 \| cmd2` | **✅ 完成** |
| **M4-9 外部命令** | `/bin/ls` `/bin/cat` `/bin/echo` 独立 ELF、Shell fork/execve、重定向、管道 stage exec | **✅ 完成** |
| **M4-10 SIGPIPE** | 最小信号 pending/投递、SIGPIPE 默认终止、`/bin/head`、`cat /big.bin \| head` | **✅ 完成** |
| **M4-11 多级管道** | N 段建 N−1 pipe、每段 dup2、子/父关闭全部 fd、wait 全部子进程 | **✅ 完成** |
| **M4-12 PGID/Ctrl-C/后台** | task pgid、setpgid/set_fg/detach、SIGINT 前台组、`&` + detach | **✅ 完成** |
| **M4-13 作业控制** | TASK_STOPPED、SIGTSTP/CONT、waitpid、作业表、jobs/fg/bg、EXEC 512 KiB | **✅ 完成** |
| **M4-14 COW/VMA** | 页引用计数、COW fork、#PF 复制、VMA 链表、匿名 mmap | **✅ 完成** |
| **M4-15 按需分页** | mmap 只建 VMA、#PF demand 分配清零页、COW/demand 同入口 | **✅ 完成** |
| **M4-16 SIGSEGV** | 用户 #PF 投递 SIGSEGV、默认终止 139、segvtest | **✅ 完成** |
| **M4-17 File-backed/ELF lazy** | file-backed VMA、mmap_file、vfs_read_at、ELF PT_LOAD→VMA、BSS/代码权限 | **✅ 完成** |
| **M4-18 Page Cache** | `(inode,page_index)` 哈希、只读代码页共享、PMM refcount、两次 ls 命中 | **✅ 完成** |
| **M4-19 munmap/mprotect** | VMA 拆分/删除/权限、PTE 清空/refcount--/COW、muntest | **✅ 完成** |
| **M4-20 用户态 malloc** | 64 KiB chunk/free list、大块 mmap/munmap、calloc/realloc、malloctest | **✅ 完成** |
| **M5-3b Journaling** | Redo Log 单块事务、commit/replay、备份超级块、故障注入 | **✅ 完成** |
| **M4-21 共享页/脏页回写** | MAP_SHARED、PTE_SHARED fork 共享、msync 写回、Page Cache LRU | **✅ 完成** |
| **M4-22 用户态 libc** | printf/snprintf、字符串/内存、atoi、libctest | **✅ 完成** |
| **M4-23 FILE 流** | fopen/fread/fwrite/fgetc/fgets/fprintf、Shell/ls/cat/echo/head printf 重构 | **✅ 完成** |
| **M4-24 lseek/缓冲 stdio** | SYS_LSEEK、FILE 512B 读缓冲、fseek/ftell、a/a+ 追加 | **✅ 完成** |
| **M4-25 FILE 写缓冲** | wbuf[512]、fputc/fwrite 缓冲、fflush/fclose/fseek/ftell 自动 flush | **✅ 完成** |
| **M4-26 Lua 前置** | 16 页用户栈、setjmp/longjmp、全局 errno、sys_time、fflush(NULL) | **✅ 完成** |
| **M4-27 SSE/FPU** | CR4.OSFXSR/OSXMMEXCPT、CR0.MP/NE、fninit、USER_FLOAT_CFLAGS、floattest | **✅ 完成** |
| **M4-28 Lua 移植** | 17 个标准头、multi-TU libc、Lua core+base、/bin/lua、`print("Hello...")` | **✅ 完成** |
| **M4-29 Lua 标准库** | table/string/utf8、脚本路径、UTF-8 输出 | **✅ 完成** |
| **M4-30 FPU/io/os/REPL** | FXSAVE/FXRSTOR、Lua math/io/os、REPL、Lua 写文件 | **✅ 完成** |
| **M4-31 math/REPL** | x87/SSE/Taylor math、精确整数 pow、REPL 多行 | **✅ 完成** |
| **M4-32 SMP 地基** | QEMU -smp4、MADT、x2APIC、spinlock、PMM 加锁 | **✅ 完成** |
| **M4-33 AP 唤醒** | 0x6000 trampoline、INIT/SIPI、AP online=3、跨核 spinlock | **✅ 完成** |
| **M4-34 per-CPU/锁** | GS per-CPU current、kprintf 锁、run_queue_lock、AP idle | **✅ 完成** |
| **M4-35 生命周期锁** | task_alloc/reap/count 加锁；AP 用户调度仍关闭 | **部分完成** |
| **M4-37 Per-CPU TSS/IDT** | 8 个 TSS、AP gdt/ltr、AP idt、0x30 异常、GS base 修复 | **✅ 完成** |
| **M4-38 AP timer/iret** | AP CS 重载、0x30 返回、LVT timer 0x31、EOI、10k ticks | **✅ 完成** |
| **M4-39 swapgs** | KERNEL_GS_BASE、user_enter/ISR swapgs；AP 队列回滚 | **基础完成** |
| M4-40 AP 用户调度 | per-CPU ready queue、AP schedule、CR3/TLB | 计划中 |
| M5 存储 | virtio-blk、自研简单文件系统、持久化 | 计划中 |
| M6 进阶 | SMP、APIC、网络、图形等 | 远期 |

---

## 10. 构建、运行与调试

```bash
# 0) 依赖（WSL2 Ubuntu 示例）
sudo apt install -y build-essential grub-pc-bin grub-common xorriso mtools qemu-system-x86 gdb

cd ~/lantest

make                    # 编译内核并打包 lantest.iso
make run                # QEMU 图形窗口运行（WSLg / 桌面 Linux）
make run-nographic      # 无图形环境：串口输出到终端
make test               # 无头启动测试：校验 tick 中断 + 物理页自检
make test-exception     # 构造一次故意的 #PF，校验异常处理器与 panic
make debug              # QEMU 暂停，GDB stub 监听 :1234
make gdb                # 另一终端：连接、在 kernel_main 下硬件断点并继续
make clean
```

> `lantest.iso` 也可以直接用 VirtualBox/VMware 以 BIOS 方式启动；VirtualBox 未配置串口时，内核会自动只使用 VGA 输出。

---

## 11. 依赖与第三方边界

- GCC / binutils / GNU Make / GRUB / xorriso / QEMU / GDB 仅在**构建期与运行期**使用，不进入内核代码。
- Multiboot2 是公开引导规范，按规范自行实现头部与跳板。
- 内核不包含任何第三方内核代码。
- 未来的 Linux 兼容层（若做）：作为**可选用户态 ABI 兼容模块**，不改变内核基础 ABI，不作为默认构建。

---

## 12. 风险与开放问题

1. **1 GiB 恒等映射限制**：M3 需要更完整的地址空间方案（higher-half 或按需映射），PMM 也要扩展到更高物理地址。
2. **仅 BIOS 引导**：UEFI 直启需要另一套入口。
3. **PIC/PIT 为传统设备**：后续应迁移到 APIC/IOAPIC/APIC timer；当前 100 Hz 足够驱动调度时钟。
4. **双故障（#DF）没有独立 IST 栈**：目前仍使用当前栈；M3 建议为 #DF 配置 IST1。
5. **单核假设**：M6 前所有代码可按单核编写，但需避免写出无法加锁的结构。
6. **无 libc/libgcc**：64 位十进制输出用软件除法；后续自研 libk 需继续避免隐式运行时依赖。
7. **串口存在性探测**：通过 16550 scratch 寄存器（offset 7）判断；无串口环境自动降级为仅 VGA，避免死等 LSR。
8. **工具链版本漂移**：`-mgeneral-regs-only`、`-z max-page-size`、`-Map` 等参数依赖较新的 GCC/binutils（已记录实测版本）。
9. **VirtualBox 已实测**：M2 ISO 已在 VirtualBox 7.2.16（BIOS、无串口、512 MB）上启动并稳定输出 tick；仍建议每次改动后在 VirtualBox 与 QEMU 各回归一次。
10. **M3a 的堆与调度器是不可重入的单核实现**：`kmalloc/kfree`、`schedule()` 均假设单 CPU 且不会被中断重入；Ring 3 前还需要在任务切换时更新 TSS.RSP0。
11. **用户地址空间尚未落地**：内核恒等映射占用 PML4[0]；Ring 3 用户映射将使用 PML4[1]（`0x0000_0080_0000_0000`）并设置各级 U/S 位，避免与内核映射冲突。
12. **用户指针安全已由 M3c-3 覆盖**：copyin/copyout + 异常表 fixup + SMAP（CPU 支持时）；仍需按 ELF 段权限/NX、SMEP 与按需分页等进一步加固。
13. **ELF 加载器仍是第一版**：忽略 `p_align`/`p_flags`（用户页统一 RWX）、不支持 PT_INTERP/动态链接；后续按段设置权限/NX，并接入 copyin/copyout。
14. **M4-1 的 fd 层仍是初版**：固定 32 个 fd、无 open/read/close、无 inode/dentry；fd 0/1/2 各一个独立 file 对象，后续 ramfs/dup 再扩展引用计数语义。
15. **ramfs 还是单层只读路径的初版**：只支持绝对路径、根目录下的普通文件；无 mkdir/目录 read/unlink/truncate，inode/dentry 不回收，单 superblock 无 mount。
16. **Shell 已外部化、键盘/行编辑/历史/Tab 已完成，但 envp 仍为空**：没有管道/后台任务；左右光标编辑与持久历史未实现。
17. **swapgs 已修，但 AP 本地队列/用户调度代码在崩溃后回滚**：AP 仍 idle；TLB shootdown 未做。

---

## 13. 变更记录

| 日期 | 版本 | 内容 |
| --- | --- | --- |
| 2026-10-27 | v0.1 | 初始设计文档；M1 引导里程碑完成 |
| 2026-10-27 | v0.2 | M2 完成：GDT/TSS、IDT/异常、PIC/PIT、Multiboot2 内存图、物理页分配器；项目迁移到 WSL 家目录 `~/lantest` |
| 2026-10-27 | v0.2.1 | 补充 VirtualBox 7.2.16 实测记录；boot.S 增加作者注释 |
| 2026-10-27 | v0.3 | M3a 完成：连续页分配、kmalloc/kfree、task/switch_to、环形 RR + PIT 抢占、内核线程实测 |
| 2026-10-27 | v0.3.1 | M3b-1 完成：每任务 PML4、CR3 切换、per-task TSS.RSP0、CR3 自检（QEMU + VirtualBox 实测） |
| 2026-10-27 | v0.4 | M3b-2 完成：GDT 用户段、PML4[1] 用户映射、iretq Ring 3、int 0x80 syscall（write/exit/getpid）、Ring 3 中断切栈实测 |
| 2026-10-28 | v0.5 | M3c-1 完成：进程状态机、exit/wait/僵尸回收、孤儿回收器、用户页与页表释放（QEMU + VirtualBox 实测） |
| 2026-10-28 | v0.6 | M3c-2 完成：ELF64 按页加载器、共享页/BSS 处理、C 用户程序与 userlib（QEMU + VirtualBox 实测） |
| 2026-10-28 | v0.7 | M3c-3 完成：copyin/copyout、异常表 fixup、SMAP、恶意指针防护（QEMU SMAP on / VirtualBox 降级） |
| 2026-10-28 | v0.8 | M4-1 完成：file/file_operations、每任务 fd 表、console 字符设备、write(fd,...)（QEMU + VirtualBox 实测） |
| 2026-10-28 | v0.9 | M4-2 完成：VFS 骨架、绝对路径解析、ramfs 4 KiB 块链、open/read/close，filetest 大小文件 + EOF 实测 |
| 2026-10-28 | v0.10 | M4-3 完成：COM1 行输入、mkdir、目录 dirent read、O_TRUNC、init/shell；QEMU 串口真实交互 + VirtualBox 实测 |
| 2026-10-28 | v0.11 | M4-4 完成：fork 深拷贝、execve、unlink delete-on-close、shell run/rm；QEMU 交互 + VirtualBox 实测 |
| 2026-10-28 | v0.12 | M5-1 完成：PCI 枚举、legacy virtio-blk、virtqueue/DMA、扇区读写与跨重启 boot_count 持久化 |
| 2026-10-28 | v0.13 | M5-2 完成：LantestFS 超级块/inode/位图/8 块缓存/format/mount；Shell 写文件重启后可读 |
| 2026-10-28 | v0.14 | M4-5 完成：argv/envp 用户栈、naked _start、Shell run 参数、/bin/hello 持久化（QEMU 双启动实测） |
| 2026-10-28 | v0.15 | M4-6 完成：外部 /bin/sh + init 重启、PS/2 键盘 IRQ1、QEMU sendkey 实测 |
| 2026-10-28 | v0.16 | M4-7 完成：console 原始字节、Shell 行编辑/历史/Tab、动态 RW 链接脚本；QEMU sendkey 实测 |
| 2026-10-28 | v0.17 | M4-8 完成：pipe/dup2 + Shell 管道；LantestFS 一级间接块支撑 54 KiB /bin/sh |
| 2026-10-28 | v0.18 | M4-9 完成：/bin/ls、/bin/cat、/bin/echo 独立 ELF；Shell 外部命令/重定向、管道 stage exec |
| 2026-10-28 | v0.19 | M4-10 完成：最小信号机制 + SIGPIPE，cat /big.bin \| head 写端 141 终止 |
| 2026-10-28 | v0.20 | M4-11 完成：最多 8 段管道，fd 全关闭，3/4 段实测通过 |
| 2026-10-28 | v0.21 | M4-12 完成：PGID、Ctrl-C/SIGINT 前台组、后台 `&` + detach |
| 2026-10-28 | v0.22 | M4-13 完成：TASK_STOPPED、SIGTSTP/CONT、waitpid、jobs/fg/bg；exec 上限 512 KiB |
| 2026-10-28 | v0.23 | M4-14 完成：页引用计数、COW fork、#PF 复制、VMA/mmap；cowtest/mmaptest PASS |
| 2026-10-28 | v0.24 | M4-15 完成：mmap 只建 VMA，#PF demand 分配清零页；mmaptest PASS |
| 2026-10-28 | v0.25 | M4-16 完成：用户 #PF 投递 SIGSEGV，退出码 139，内核不再 panic |
| 2026-10-28 | v0.26 | M4-17 完成：file-backed VMA/mmap_file、ELF PT_LOAD→VMA 惰性加载、BSS/代码权限 |
| 2026-10-28 | v0.27 | M4-18 完成：inode/page_index Page Cache，只读代码页跨 exec 共享命中 |
| 2026-10-28 | v0.28 | M4-19 完成：VMA 拆分/删除/权限，munmap/mprotect，muntest PASS |
| 2026-10-28 | v0.29 | M4-20 完成：用户态 malloc/free/calloc/realloc（mmap/munmap），malloctest PASS |
| 2026-10-29 | v0.30 | M5-3b 完成：LantestFS v2 Redo Log + backup superblock；SB 损坏恢复与 journal replay 实测 |
| 2026-10-29 | v0.31 | M4-21 完成：MAP_SHARED、PTE_SHARED fork 共享、msync 脏页写回、Page Cache LRU |
| 2026-10-29 | v0.32 | M4-22 完成：用户态 printf/snprintf、字符串/内存函数、atoi；libctest PASS |
| 2026-10-29 | v0.33 | M4-23 完成：FILE 流 fopen/fread/fwrite/fprintf；Shell/ls/cat/echo/head 用 printf 重构 |
| 2026-10-29 | v0.34 | M4-24 完成：SYS_LSEEK、FILE 512B 读缓冲、fseek/ftell、a/a+ 追加 |
| 2026-10-29 | v0.35 | M4-25 完成：FILE wbuf[512]、fflush 真落盘、fclose/fseek/ftell 自动 flush |
| 2026-10-29 | v0.36 | M4-26 完成：16 页用户栈、setjmp/longjmp、errno、sys_time、fflush(NULL) |
| 2026-10-29 | v0.37 | M4-27 完成：CR4/CR0 SSE 启用、fninit、USER_FLOAT_CFLAGS、floattest PASS |
| 2026-10-29 | v0.38 | M4-28 完成：Lua 5.4.7 core+base 移植，启动任务与 shell run /bin/lua 输出 Hello |
| 2026-10-29 | v0.39 | M4-29 完成：Lua table/string/utf8 库，脚本输出 HELLO + UTF-8 中 |
| 2026-10-29 | v0.40 | M4-30 完成：FPU FXSAVE/FXRSTOR、Lua io/os/math、REPL、Lua 写 /hello.txt |
| 2026-10-29 | v0.41 | M4-31 完成：x87/SSE/Taylor math、2^10=1024.0、REPL 多行 function/end |
| 2026-10-29 | v0.42 | M4-32 完成：MADT 4 CPU、x2APIC、spinlock、PMM 加锁；AP SIPI 留 M4-33 |
| 2026-10-29 | v0.43 | M4-33 完成：0x6000 AP trampoline、INIT/SIPI、AP online=3、跨核 spinlock=300000 |
| 2026-10-29 | v0.44 | M4-34 完成：GS per-CPU current、kprintf 锁、schedule 锁、AP idle、percpu ok=3 |
| 2026-10-29 | v0.45 | M4-35 部分完成：task_alloc/reap/count 加锁；AP 用户调度仍关闭 |
| 2026-10-29 | v0.46 | M4-37 完成：8 个 per-CPU TSS、AP gdt/ltr、AP idt、0x30 异常、GS base 修复 |
| 2026-10-29 | v0.47 | M4-38 完成：AP CS far-return、0x30 正常返回、LVT timer 0x31、10562 ticks |
| 2026-10-31 | v0.58 | M4-47b：ICR delivery-status 排空修复 IPI 投递（探针 irq+1、ACK TIMEOUT 归零）；SYS_WAIT/SYS_WAITPID 检查+睡眠与退出+唤醒共用 run_queue_lock 临界区；wake IPI 处理器只 EOI 不调度 |
| 2026-10-31 | v0.57 | M4-47a：page_cache 驱逐改为锁外 file_close，拆除 pcache_lock↔vfs_lock ABBA；fresh-disk 8/10（原 ~1/3） |
| 2026-10-30 | v0.56 | M4-46：kmalloc/virtio/VFS 锁 + deferred reap + pipe 唤醒 + 3×3 stress worker PASS；AP idle 轮询 |
| 2026-10-30 | v0.55 | M4-45：TLB shootdown 0x30 + page_cache/pipe/VFS 锁；压测待 M4-46 |
| 2026-10-30 | v0.54 | M4-44：IPI 0xFE 队列唤醒；TLB/VFS 锁待 M4-45 |
| 2026-10-30 | v0.53 | M4-43：active_cpus + reap 等待；TLB/IPI 仍待 M4-44 |
| 2026-10-30 | v0.52 | M4-42：CPU1/2/3 各自本地队列 + timer schedule，三个 apuser status=42 |
| 2026-10-29 | v0.51 | M4-41：per-CPU queue + AP timer schedule，user-apuser 自动在 CPU1 执行 status=42 |
| 2026-10-29 | v0.50 | M4-40：AP 原子认领 user-apuser，Ring3 sys_exit(42)，cpu=3 status=42 |
| 2026-10-29 | v0.49 | M4-39b：swapgs 移入 isr_common，按栈上 CS 判断 |
| 2026-10-29 | v0.48 | M4-39 基础：KERNEL_GS_BASE、user_enter/ISR swapgs；AP 用户任务尝试回滚 |
