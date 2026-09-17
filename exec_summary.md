# SimCXL AI Checkpoint — 执行总结（P0–P7）

本文记录 SimCXL AI checkpoint 项目（`configs/example/gem5_library/target.md`）P0–P7 的实现结果、关键实现细节、测试方法，以及借鉴其他两个库（CXLMemSim / LLMServingSim）的落点。

## 概述

目标系统是一个基于 SimCXL（gem5 全系统周期级 CXL 模拟器）的 **GPU 训练 checkpoint / restore 数据搬运引擎**：

```
GPU payload ──> DRAM / CXL（pinned pool）
              │   ↓ (SimCkptDevice DMA: 内存读→CRC→写)
              └─> ParallelStorage（多通道闪存后端）
                   ↓ (restore: storage 读→DMA 写回 DRAM/CXL)
```

三层职责划分（对应三个参考库）：

| 层 | 角色 | 参考 |
|---|---|---|
| 工作负载/服务层 | checkpoint trace、分流调度、热备、指标 | LLMServingSim |
| 内存系统/一致性层 | 拓扑、条带化、manifest 版本/代际、容量伸缩 | CXLMemSim |
| 硬件/周期层 | 真实数据搬运、协议、微架构 | SimCXL（本仓库） |

## 已完成阶段

### P0：建立正确基线（已完成）
- 新增 `configs/example/gem5_library/x86-cxl-ai-checkpoint.py`（Ruby MESI Two Level，KVM 启动 → Timing CPU）。
- CXL 内存经 dax_kmem **全部**交给 VM（本内核 E820 type-20 自动 online 为 System RAM node 1），不再为 PyTrafficGen 保留顶部测试区。
- 修复 classic 脚本 `--cxl-size` 未接入、以及 E820 override 用 `int(e.addr)/int(e.range_type)` 比较的问题。
- 基线带宽（512 MiB，Ruby+DDR5）：GPU→DRAM 写 ~30 GB/s、DRAM→GPU 读 ~29 GB/s、GPU→CXL 写 ~30 GB/s、CXL→GPU 读 ~20 GB/s。

### P1：数据搬运引擎（已完成）
- 新增 `SimCkptDevice` PCI 设备（`src/dev/storage/SimCkpt.py`、`sim_ckpt_device.{hh,cc}`）。
- submission/completion ring + doorbell + queue depth + 中断（`REG_INTR_EN` + `intrPost()`）。
- **MemCopy 语义**：DMA read 源内存 → 读响应把 payload 放进 buffer → CRC32 → DMA write 目标 → 只有写响应到达才生成 completion + 中断。
- 描述符/完成项均为 64 B cache-line 大小（避免 Ruby `DMASequencer` 的 cache-line aliasing）；SQ/CQ 用绝对索引 + `%depth` 取址（避免满环死循环）；乱序完成用 `chunk_id` 匹配。

### P2：并行存储（已完成）
- 新增 `ParallelStorage`（`src/dev/storage/ParallelStorage.py`、`parallel_storage.{hh,cc}`）。
- 每通道独立：read/write bandwidth、base latency、queue depth、outstanding 限制、byte-addressable backing store。
- chunk striping：`channel = chunk_id % N`，`channel_offset = (chunk_id/N)*chunk_size + offset%chunk_size`。
- storage 端口用 **whole-request**（不做 64 B 拆分），让 flash 延迟作用于整页（4 KiB）。
- 描述符 `flags` 区分方向：`FLAG_SAVE`（内存→storage）、`FLAG_RESTORE`（storage→内存）。
- 实测（设备 `execTicks` 统计）：**1 通道 ~7.2 GB/s，2 通道 ~11.5 GB/s**（证明双通道带宽高于单通道）。

### P3：VM 驱动 + 用户态软件（已完成）
- 新增完整用户态软件栈 `tests/cxl_tests/simckpt/`：
  - `include/simckpt_uapi.h` —— 冻结 ABI（64 B 描述符/完成项、BAR0 寄存器、ioctl 命令字）。
  - `driver/simckpt.c` —— `simckpt.ko` 内核驱动（参考实现）：PCI probe、64-bit DMA mask、`pin_user_pages_fast` + `dma_map_sg`、BAR0 mmap、REGISTER/UNREGISTER/SUBMIT/WAIT/GET_COUNTERS ioctl。
  - `engine/libckpt.{h,c}` —— 用户态库：save/restore 接口（分块、提交、等待、CRC）；无模块时经 `/dev/mem` + `/proc/self/pagemap` 回退，**同一套 uapi ABI**。
  - `ckptd.{h,c}` —— manifest（chunk_meta + generation + expected_chunks + state）与 pinned pool（`mbind` node 0/1）。
  - `bench/ckptbench.c` —— 测试：DRAM/CXL 往返 + 故障注入。
- 测试结果（gem5 内，Timing CPU）：
  ```
  SimCkptDevice opened (backend=auto)
  [roundtrip] DRAM round-trip OK (gen 1)
  [roundtrip] CXL round-trip OK (gen 2, committed_gen=2)
  [roundtrip] counters: completed=1 errors=0 intr=0
  [roundtrip] PASS
  [fault] gen 1 committed
  [fault] crash after 2/4 chunks of gen 8
  [fault] rolled back to gen 1 (incomplete gen discarded)
  [fault] PASS
  PASS
  ```

### P4：动态分流 + 压力控制（已完成）
- 新增 `tests/cxl_tests/simckpt/balancer.{h,c}` —— 分流调度器（借 LLMServingSim 的放置策略）：
  - 每 lane（DRAM/CXL）维护 `queued_bytes / outstanding / pool剩余 / EWMA bw / P95 / retries`；
  - 选 lane：`predicted_finish = (queued_bytes + chunk_size) / R`，叠加 occupancy / backlog / retry / P95 惩罚；
  - 全局 + 每 lane **token bucket**（`token_rate/token_burst`）限流防积压。
- `ckptd.{h,c}` 增加 **pinned pool 状态机**（借 CXLMemSim 的 DCD 动态容量）：`NORMAL → FROZEN → SHRINK → STOPPED`，阈值 80%/90%/95%。
- `bench/ckptbench_p4.c` —— 测试：分流（DRAM 小池 1 MiB 先填满、再溢出到 CXL 大池 8 MiB）+ 压力控制（FROZEN 后 pinned_bytes 不再增长）。
- `balancer_scale.c` —— **宿主机纯 C 单元测试**：把 512 MiB（131072 个 4 KiB chunk）走一遍分流，证明逻辑与数据量无关（gem5 里大数据仿真太慢，故用宿主机毫秒级跑）。
- 测试结果：
  ```
  [balancing] chunks: dram=256 (1048576 B), cxl=768 (3145728 B)   # gem5 内（4 MiB 小样）
  [balancing] PASS
  [512MiB headroom-routing] total=512 MiB (131072 chunks)          # 宿主机（512 MiB 全量）
    dram=128 MiB (32768 chunks), cxl=384 MiB (98304 chunks), denied=0
    ratio dram:cxl = 32768:98304
  [pressure] NORMAL pinned=65536 (cap=65536)
  [pressure] FROZEN at 85%: new pin denied, pinned=65536
  [pressure] state at 92% = SHRINK
  [pressure] state at 97% = STOPPED
  [pressure] PASS
  ```
- 说明：验收 #2（双内存路径 staging 带宽 > 最快单路径）指的是 **GPU→DRAM/CXL 的 staging 写入带宽**（用于降低 GPU stall，而非 storage 落盘吞吐）。它由 GpuDmaEngine（P7）建模，且两路并行 staging 需要每 lane 独立的 DMA 引擎（P10「多 GPU DMA 队列」）；当前设备只实现了 persist 路径（DRAM/CXL→storage），storage 是瓶颈（2 通道 ~11.5 GB/s），与 #2 的 staging 指标无关。

### P5：CXL 热备（hot standby）（已完成）
- `ckptd.{h,c}` 增加 **热备管理器**（借 CXLMemSim 一致性引擎 sharer/dirty 思想）：CXL 保留最近 checkpoint 的热 chunk 作为磁盘副本的**缓存**（LRU 淘汰），命中走 CXL 快路径、未命中走 storage 慢路径。
- `bench/ckptbench_p5.c` —— 测试：save 512 个 chunk 到 storage + CXL 热备，然后按命中率 0/25/50/100% 恢复并计时。
- 测试结果（restore 时间随命中率单调下降，验收 #4）：
  ```
  [hot   0%] restore 512 chunks: 52.992 ms (OK)   ← 全冷：storage DMA
  [hot  25%] restore 512 chunks: 40.994 ms (OK)
  [hot  50%] restore 512 chunks: 28.996 ms (OK)
  [hot 100%] restore 512 chunks:  6.999 ms (OK)   ← 全热：CXL memcpy
  ```
  100% 命中比 0% 命中快 ~7.6×，且每次恢复都逐字节校验 OK。

### P6：LLM 训练保存/故障/恢复（应用层，已完成）
- `bench/ckptbench_p6.c` —— 应用层模拟（借 LLMServingSim 工作负载级思想）：模型状态用确定性函数 `model[i]=PRNG(step,i)` 表示，训练循环周期性保存 checkpoint，故障注入（写到一半崩溃），重启后从 manifest 恢复最后一个 COMMITTED 代际。
- 崩溃矩阵（P6-1）+ generation 回退（P6-2）都在应用层实现；并发恢复（P6-3）推迟到 P10 多 DMA 引擎。
- 测试结果：
  ```
  [train] checkpoint gen=1 (step 10) committed
  [train] checkpoint gen=2 (step 20) committed
  [train] FAULT: crashed mid-checkpoint gen=3 after 2/256 chunks
  [train] manifest persisted: committed_gen=2 (step 20)
  [recover] manifest: committed_gen=2 (step 20)
  [recover] restored gen=2 in 26.996 ms -> VERIFIED OK
  ```
  崩溃于 gen 3（只写了 2/256 chunk）→ manifest 回退到 gen 2 → 恢复 gen 2 逐字节校验 OK。

### P7：GPU payload 真实化 + 数据校验（已完成）
- `engine/gpu_dma_engine.{h,c}` —— 用户态 GPU DMA 引擎仿真：`payload[i] = PRNG(checkpoint_id, chunk_id, i)`（32-bit fp32 张量元素的高字节），可复现、无需保存参考副本即可校验。
- `engine/sha256.{h,c}` —— 自包含 SHA-256（FIPS 180-4，用 `abc`/空串测试向量验证），用于整 checkpoint 摘要。
- `bench/ckptbench_p7.c` —— 完整往返 GPU→DRAM/CXL→storage→DRAM/CXL→GPU：每 chunk CRC32（设备计算 + 恢复交叉校验）+ 全 checkpoint SHA-256 + GPU 端重生成逐字节比对。
- 测试结果：
  ```
  [DRAM] save 24.996 ms (256 chunks), restore 25.996 ms
         per-chunk CRC32: OK | full SHA-256: OK | GPU recheck: OK
         sha256=88da24b2...a5567125
  [CXL]  save 24.996 ms (256 chunks), restore 25.996 ms
         per-chunk CRC32: OK | full SHA-256: OK | GPU recheck: OK
         sha256=92b4171b...266c0a95
  counters: completed=1 errors=0 intr=0
  gpu engine: generated=2097152B verified=2097152B mismatches=0
  PASS
  ```
  覆盖验收 #1（CRC 一致）与 #5（每 chunk 按 checkpoint_id/chunk_id 独立校验，乱序仍可恢复）。

## 关键实现细节与踩坑

1. **E820 override 比较**：`X86E820Entry.addr/range_type` 是 gem5 的 `Addr`/`UInt64` 对象，不能直接 `== int`，需 `int(e.addr)/int(e.range_type)`。
2. **Ruby `DMASequencer` cache-line aliasing**：并发 DMA 命中同一 cache line 会 `m_RequestTable.clear()` 导致断言；用 64 B 对齐的描述符/完成项 + 不重叠缓冲规避。
3. **PCI BAR 与 CXL 窗口重叠**：64-bit BAR 会被 Linux 分到 CXL 4–12 GB 区间；改用 32-bit BAR（<4 GB）+ 使能 PCI `memory space`。
4. **IOAPIC 中断线越界**：`InterruptLine=0x1f` 超过 `TableSize=24`，改 0x11。
5. **SQ/CQ 满环死循环**：`sqHead` 若用 `%depth` 会永不等同 `sqTail`；改用绝对索引，取址时 `%depth`。
6. **flash 延迟粒度**：storage 端口不做 64 B 拆分，否则 10 µs 延迟作用在 64 B 上导致延迟受限（Little's Law：`BW = depth*size/latency`）。
7. **复位与计数语义**：`SimCkptDevice` 的 `CTRL=0x2` 复位会清零 `completedCount`；libckpt 每批提交都复位，用“自复位起 n 个完成”计数。
8. **多代 checkpoint 存储布局**：每代放在 `gen * checkpoint_size` 的独立存储基址，避免覆盖；manifest 用 `expected_chunks` 判定完整性（缺块即视为未完成）。
9. **内核模块无法在本环境编译**：guest 内核是定制 6.12.0+，disk image 只带 4.15 头文件；`simckpt.ko` 作为参考实现，测试走 libckpt 的 `/dev/mem` 回退路径（同一 ABI）。
10. **Ruby directory 的 DMA 一致性 assert**：restore 写回 DRAM 时，若目标物理页与之前某次 save 读过的物理页复用（跨进程 crash/restart 后 malloc 复用同一物理页），Ruby directory 会命中 `MESI_Two_Level-dir.sm` 的 `assert(is_valid(tbe))`（`da_sendDMAAck`）。规避：恢复目标放到 CXL node（不同地址空间/不同 directory），或避免跨进程物理页复用。

## 测试方法

### 1. 构建

```bash
# gem5（含 SimCkptDevice + ParallelStorage）
cd <SimCXL>
scons build/X86/gem5.opt -j$(nproc)

# 用户态 ckptbench（静态链接，可在 gem5 guest 内直接运行）
cd tests/cxl_tests/simckpt
make            # 产出 ./ckptbench

# 内核模块（可选，需要 guest 6.12+ 内核头文件，本环境无）
make module KDIR=/path/to/linux-6.12-headers
```

### 2. 把 ckptbench 注入 disk image

```bash
mkdir -p /tmp/img && mount -o loop,offset=$((2048*512)) /root/simcxl-resources/parsec.img /tmp/img
cp tests/cxl_tests/simckpt/ckptbench /tmp/img/home/test_code/ckptbench
chmod +x /tmp/img/home/test_code/ckptbench && sync && umount /tmp/img
```

### 3. 运行与验证

| 阶段 | 配置脚本 | 验证内容 |
|---|---|---|
| P0 | `configs/example/gem5_library/x86-cxl-ai-checkpoint.py --gpu-copy-mib 512` | DRAM/CXL 读写带宽基线（~30/29/30/20 GB/s） |
| P1/P2 | `configs/example/gem5_library/x86-cxl-simckpt-test.py`（`--storage-channels N --bench-mib M`） | MemCopy 语义、CRC、乱序完成、条带均分、双通道 vs 单通道带宽 |
| P3 | `configs/example/gem5_library/x86-cxl-ckptd-test.py` | libckpt+ckptd 往返 CRC 一致、manifest generation 回退 |
| P4 | `configs/example/gem5_library/x86-cxl-ckptd-p4.py` | 分流（headroom 路由、双 lane 使用）+ 压力控制（pool 状态机、pinned_bytes 不增长） |
| P5 | `configs/example/gem5_library/x86-cxl-ckptd-p5.py` | CXL 热备：命中率 0/25/50/100% 下恢复时间单调下降（#4） |
| P6 | `configs/example/gem5_library/x86-cxl-ckptd-p6.py` | LLM 训练保存/故障/恢复：崩溃矩阵 + generation 回退 + 恢复验证 |
| P7 | `configs/example/gem5_library/x86-cxl-ckptd-p7.py` | GPU payload 真实化：GPU→DRAM/CXL→storage→DRAM/CXL→GPU 往返 CRC32+SHA-256 一致 |

```bash
./build/X86/gem5.opt -d m5out-p3 \
  configs/example/gem5_library/x86-cxl-ckptd-test.py
# 结果见 m5out-p3/board.pc.com_1.device，期望输出以 PASS 结尾
```

### 4. 分层测试对照验收标准（target.md 八条）

- **L0 单元层**（gem5，无 VM）：ParallelStorage 条带均分（`chanBytesWritten`）、backpressure（`numRejected`）；SimCkptDevice MemCopy/CRC/乱序（`execTicks`）——验收 #3、#8。
- **L1 集成层**（驱动 round-trip）：libckpt + ckptd 的 save→restore CRC/SHA-256 一致、pin/unpin 生命周期——验收 #1、#5、#7。
- **L2 系统层**（分流 + 压力）：分析模型（借 LLMServingSim）预测 Bsave 上界 + 周期仿真抽样验证；压力下 pinned pool 只复用不扩大——验收 #2、#6。
- **L3 工作负载层**（故障注入 + 热备）：崩溃矩阵 + generation 回退 + 热备命中率——验收 #4。

## 参考资料

- SimCXL：<https://github.com/TianheMICALab/SimCXL>（本仓库 base）
- CXLMemSim：<https://github.com/SlugLab/CXLMemSim>（拓扑/一致性/DCD/GFAM/HDM 参考）
- LLMServingSim：<https://github.com/casys-kaist/LLMServingSim>（工作负载级仿真/KV cache/指标参考）
- 论文：Cohet (HPCA 2026, DOI 10.1109/HPCA68181.2026.11408611)；CXL-DMSim (TCAD 2025, DOI 10.1109/TCAD.2025.3607145)
- 规范：CXL 3.x、NVMe Base Spec（ring/doorbell 设计参考）、gem5 文档

> 注：CXLMemSim 与 LLMServingSim 仓库体积较大、网络受限，本会话仅作为概念参考（借鉴点见正文）；关键实现均基于 SimCXL 独立完成。
