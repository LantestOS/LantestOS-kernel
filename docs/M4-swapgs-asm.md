# M4-39b — swapgs 移入 isr_common

> 状态：**已完成并通过实测**（QEMU + make test）  
> 日期：2026-10-29  
> 关联：[M4-swapgs.md](./M4-swapgs.md)

---

## 1. 改动

之前 swapgs 在 C 的 `isr_handler` 里按中断来源切换，存在隐患：

- 中断处理内部调用 `schedule()` 切到内核线程后，返回路径仍按“来源是用户”再 swapgs；
- 内核线程返回时会拿到用户 GS。

现在移入 `isr_common` 汇编，按**栈上真实 CS** 判断：

入口（压寄存器前）：

```asm
isr_common:
    testb $3, 24(%rsp)   /* int_no/err/rip/cs */
    jz 1f
    swapgs
1:
    pushq %rax
    ...
```

出口（弹完寄存器、add $16 前）：

```asm
    popq %rax
    testb $3, 24(%rsp)
    jz 2f
    swapgs
2:
    addq $16, %rsp
    iretq
```

栈布局：

```
0  int_no
8  err_code
16 rip
24 cs
```

因此入口/出口都是 `24(%rsp)`。

## 2. 验证

- `make` 零警告；
- `make test` → **PASS**（task A=18, B=18）；
- BSP 用户态、int 0x80、fork、Lua、shell 全部正常。

## 3. 当前状态

- AP 仍 idle/hlt，不参与用户调度；
- AP 用户任务重开仍待 M4-40；
- 下一步按隔离策略：关 timer 抢占、协作式 AP 用户 `sys_exit`、打印 TSS/CR3/RIP/RSP。 
