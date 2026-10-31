# Release Gate — v0.1.0 / 发布门槛

本标准用于把 v0.1.0 定为**正式**版本。所有条目必须**实测通过并留档数字**；
任何"看起来过了"不算数。/ Every item must be measured and recorded. "Looks fine"
is not evidence.

## 1. Correctness（正确性，硬性）

| # | 门槛 | 判定方式 |
| --- | --- | --- |
| G1 | 零已知 panic | 50 连 fresh-disk `make test`：`KERNEL PANIC` / `CPU EXCEPTION`（exception 测试除外）出现 0 次 |
| G2 | 无静默冻结 | 同一 50 连：每轮串口必须有终止标记，无 tick 停止 >5s |
| G3 | 无内存踩踏 | `kfree: double free / bad block magic / corrupt block header` 0 次；`kmalloc` 自检每轮通过 |
| G4 | 并发不变式 | 每轮 `stress: worker PASS` == 3；`ACK TIMEOUT` == 0；IPI probe `irq 3 -> 4` |
| G5 | 持久化 | `make test-persist` × 10 全绿（boot_count 1→2 + 二进制已在盘上） |
| G6 | 异常路径 | `make test-exception` × 5 全绿 |
| G7 | 构建 | `-Wall -Wextra` 零警告；`make clean && make` 可重复 |

## 2. Evidence（证据要求）

- 通过率必须写成 **N/M**，且 M ≥ 50（并发/调度类改动 M ≥ 100）。
- **每次必须用全新磁盘**（`rm -f disk.img && truncate -s 16M disk.img`）。复用脏盘
  得出的绿灯一律作废（M4-46 的教训）。
- 至少 3 个不同 QEMU 时序档位实测：默认、`-d int`（拖慢）、`-icount shift=7`
  （虚拟时钟），防止"改了时序所以不出现"的假修复。
- 一次只改**一个变量**；回滚不允许"顺手夹带"其他修改。
- 失败样本必须保留（`serial.log` + `qemu -d` 日志），修完用同一场景回归。

## 3. Hygiene（工程质量，v0.1.0 前完成）

| # | 项 |
| --- | --- |
| H1 | 已知 bug 全部有独立文档条目与复现配方（不允许"偶发，未查"） |
| H2 | 锁序有文档并有运行期检查（`spin_lock` 记录持有者 CPU 与调用点；违序即打印） |
| H3 | `docs/` 中英各一份索引；README 中英两半严格对等（章节数、图片、链接一致） |
| H4 | CI 在 push/PR 上跑 G1–G7 的抽样版本；Release workflow 产出 ISO + SHA256 |
| H5 | CHANGELOG 中英对称，含实测数字与已知问题 |

## 4. Non-goals（明确不追求，避免标准失控）

- 不追求真实硬件启动（QEMU-only 会在 README 声明）。
- 不追求网络/图形/POSIX 兼容。
- AP idle 的 `hlt` 省电优化排在 v0.1.0 之后（不阻塞发布）。

## 5. Current gap（截至本文提交）

| 门槛 | 现状 | 差距 |
| --- | --- | --- |
| G1 | ❌ 观察到 `kfree: double free` panic | 正在装堆取证 |
| G2 | ❌ 3–4/10 静默冻结（疑与 G1 同源） | 待 G1 修复后复测 |
| G3 | ❌ 有 | 同上 |
| G4 | ✅ 每绿灯轮 stress=3、ACK TIMEOUT=0 | 保持 |
| G5 | ✅ 1/1 | 需扩到 10 |
| G6 | ✅ 1/1 | 需扩到 5 |
| G7 | ✅ 零警告 | 保持 |
| H3 | ✅ README 已对等 | 保持 |

> 门槛提高的意义不是让发布变慢，而是让每个 ✅ 都可以被别人复现。
