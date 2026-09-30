# target2.md —— GPU 双线 staging 加速（CXL 同域拓扑）改造计划

> 本文是 `target.md` 的续篇。阅读前提：先读 `target.md` 的
> `**********************changes to` 之后的拓扑变更，以及本仓库 `readme_cxl.md`、
> `exec_summary.md`（P0–P10 已完成的成果）。
>
> 一句话目标：把 checkpoint staging 从「单共享 PCIe 上行」改成「两条独立、
> 每条多通道的 PCIe 数据线」，让 GPU 可以在不争抢同一条上行通道的前提下，并行向
> Host DRAM（4 DIMM）与 CXL expander（直连 PCIe switch，Gen5 x16，4 DIMM =
> 8 DDR 子通道）staging，从而降低 GPU stall、提高 staging 带宽。

> **本版核心修正（相对上一版）**：CXL expander 不再经过 CXL switch，而是作为端点
> **直连 PCIe switch（Gen5 x16）**；CPU 只发 CXL.io（控制面）给 expander，数据搬运
> 由 expander 的 on-device DMA 主导，且这条 DMA 是**纯 PCIe TLP（无 CXL.mem 的
> 68B flit）**。由此：① CXL 线不再乘 64/68 flit 折算，与 DRAM 线同式（128b/130b +
> TLP header/LCRC）；② line_cxl 从 Gen4 x16 升为 Gen5 x16（与上行同速）；③ CXL
> 后端从 2×32 升为 4 DDR×50 = 200 GB/s。

---

## 一、背景：拓扑从「单共享上行」变成「CXL 同域双线」

### 1.1 改造前（target.md 原文的上半部，旧单共享上行拓扑）

```
GPU ──(1 条共享 PCIe/CXL uplink)── Root Complex ──┬─ Host DRAM (2ch)
                                                   └─ CXL Type-3 (2ch)
```

GPU 把数据 copy 到 host DRAM 与 copy 到 CXL Type-3，**两条路都要挤同一条 PCIe
上行通道**。在单卡时上行尚可，一旦多卡（或 GPU 同时发 DRAM + CXL 两路流），上行
就变成瓶颈。这正是 P10 里验收 #2 失败的根因之一：两 lane 并发的总带宽 ~25 GB/s，
反而低于最快单 lane 的 ~31 GB/s。

旧代码里这个共享上行就是 `X86Board._setup_board` 里所有 queue 复用的同一个
`PcieLink`（`simckpt_device.dma` 单端口）。E0 已把它改成每 lane 独立 `PcieLink`。

### 1.2 改造后（target.md `changes to` 之后的拓扑，本版修正：无 CXL switch）

```
                        CPU ── Host DRAM Controller
                                 ├─ DDR0
                                 ├─ DDR1
                                 ├─ ...
                                 └─ DDR7        （4 DIMM / 8 条 DDR 子通道）
                         │
                     Root Complex   (PCIe 5.0 上行, Gen5 x16)
                         │
                     PCIe Switch    (PCIe 5.0)
                   ├── GPU 0 (PCIe 4.0, Gen4 x16)
                   ├── GPU 1 (PCIe 4.0, Gen4 x16)
                   ├── GPU 2 (PCIe 4.0, Gen4 x16)
                   ├── GPU 3 (PCIe 4.0, Gen4 x16)
                   └── CXL Expander (端点直连, Gen5 x16)
                         │  on-device DMA controller
                         └── 4 DIMM = 8 DDR 子通道 (200 GB/s)
```

关键点（本版修正）：

1. **无 CXL switch**：CXL expander 作为**端点直连 PCIe switch（Gen5 x16）**，与 GPU
   同域，不再经过 CXL switch。
2. **CPU 只走控制面**：CPU 发 **CXL.io（控制面）** 给 expander，用于设备发现/配置/
   DMA 描述符与 doorbell；真正的 **GPU→CXL DDR 数据搬运由 expander 的 on-device
   DMA controller 主导**（PCIe P2P，直击 GPU HBM）。
3. **数据面是纯 PCIe TLP**：这条 DMA 是纯 PCIe TLP，**没有 CXL.mem 的 68B flit**，
   因此 CXL 线不再乘 64/68 折算。

于是两条**互不争抢上行**的 staging 数据线：

- **Line 1（GPU→DRAM）**：`GPU → PCIe Switch → Root Complex（上行 Gen5 x16）→
  Host DRAM Controller → 4 DIMM`。这条线**占用 switch 上行**。
- **Line 2（GPU→CXL）**：`GPU → PCIe Switch → CXL Expander（Gen5 x16 直连）→
  CXL DDR`。这条线是 **switch 本地 P2P**，**不经过 switch 上行、不经过 Root
  Complex/CPU**，由 on-device DMA 发起，纯 TLP 数据面。

两条线的链路速率现在**同为 Gen5 x16**（上行与 expander 直连同速），因此可加性更强；
CXL 后端（200 GB/s）也不再是瓶颈。多卡时的「上行瓶颈」被解除，staging 带宽从
`≤ 上行` 提升到 `≈ B_line1 + B_line2`。

---

## 二、理论依据（论文 + 规范）

论文：**"Replacing NVMe Staging in LLM Inference with a High-Bandwidth CXL
Memory Expander with an On-Device DMA Controller"**（Burru, Nalla, Prasad,
SIGCOMM 2026，DOI 10.1145/3789240.3822568）。

核心结论（直接支撑本拓扑）：

1. **CXL 内存扩展器带 on-device DMA controller**，用它替代 NVMe staging。LLM
   inference 的 KV cache / 中间张量溢出时，不再走 NVMe（Gen5 SSD 每盘仅
   ~14 GB/s 读 / ~7 GB/s 写），而是 stage 到 CXL expander。
2. **当代 GPU 原生不参与 CXL 协议**，论文的做法是：**设备端 DMA 控制器直接驱动
   PCIe P2P 传输，直击 GPU HBM**；CXL 只在 host 侧用于容量扩展与设备管理。
   ——本版把它细化成：**CPU→expander 走 CXL.io（仅控制面：发现/配置/描述符/
   doorbell），数据面由 on-device DMA 发起纯 PCIe TLP 的 P2P 传输**。
3. **数据面无 CXL.mem flit**：GPU↔expander 的搬运是纯 PCIe TLP（MRd/MWr），没有
   CXL.mem 的 68B flit 帧开销，因此 CXL 线与 DRAM 线按同一套物理层公式建模
   （128b/130b 线编码 + TLP header/LCRC/framing/DLLP），不再乘 64/68。
4. 实测带宽：expander 达 **~51 GB/s 读（约 Gen5 x16 峰值的 80%）**、**~33 GB/s
   写**；同一代 Gen5 SSD 单盘仅 14/7 GB/s。写比读慢 ~1.5×，论文归因于 PCIe 写
   完成语义（posted vs non-posted）。
5. **多线程描述符提交**把达到峰值带宽的 block size 从 8 MB 降到 256 KB，支撑 KV
   cache 更新这类细粒度、高频 staging 负载。

配套数值（用于本计划的合理性标定）：

| 项 | 数值 |
|---|---|
| PCIe Gen5 x16 有效带宽 | raw 64 GB/s → **55.53 GB/s**（128b/130b + TLP header/LCRC/framing/DLLP） |
| PCIe Gen4 x16 有效带宽 | raw 32 GB/s → **27.77 GB/s**（同效率系数） |
| 4× GPU 聚合 egress（Gen4 x16） | 4 × 27.77 ≈ **111 GB/s** |
| CXL expander 后端 | **4 DIMM = 8 DDR 子通道 = 200 GB/s**（50 GB/s / 64-bit DIMM） |
| Host DRAM 后端 | 4 DIMM = 8 DDR 子通道（DDR5，带宽充足，非 S2 瓶颈） |
| Gen5 NVMe x4 单盘读 / 写 | ~14 / ~7 GB/s |

效率系数说明：Gen5 x16 raw = 32 GT/s × 16 × 8b/B = 64 GB/s；经 128b/130b 线编码与
TLP header（16B）+ LCRC（4B）+ framing/DLLP（~2+2B）摊薄后，有效 ≈ 55.53 GB/s
（≈86.8%）。Gen4 x16 用同一系数 → 27.77 GB/s。两条线（上行、expander）同为
Gen5 x16，故 B_line1 = B_line2 = 55.53 GB/s。

论文明确区分两条语义路径：**host 侧（DRAM）** 走普通 PCIe + 内存控制器；
**expander 侧（CXL）** 走 P2P + on-device DMA（纯 TLP）。这正好对应本计划的
Line 1 / Line 2。

---

## 三、目标澄清：优化的是 staging，不是 storage 落盘

沿用 target.md 第六节的分层结论，但把优先级更明确地钉死在 **staging** 上：

- **阶段 1（同步，GPU 可见）**：GPU 把状态写入 DRAM / CXL（staging），写完立刻回
  到训练/推理。**这是首要优化对象**，指标是 **staging 带宽** 与 **GPU stall time**。
- **阶段 2（异步，后台）**：DRAM / CXL → storage 慢慢落盘，不阻塞 GPU，是次要
  目标。

**本拓扑（双独立上行 + 多通道后端）的全部价值都落在阶段 1**：两条独立 PCIe 线让
staging 带宽可加性成立（`B_line1 + B_line2`），多通道后端（Host 4 DIMM + CXL
expander 4 DIMM = 8 DDR）让每条线都能吃到多通道并发。storage 落盘
（ParallelStorage）保持不变、不进关键路径。

因此 target2 的验收矩阵**以 staging 带宽 / GPU stall 为主指标**，storage 吞吐、
CXL 热备等原矩阵项降为从属指标（见第五节）。

---

## 四、与 target.md 的关系（继承 P0–P10，新增 E 阶段）

P0–P10 已交付：SimCkptDevice（多队列 + per-link 统计 + FLAG_STAGE staging 描述
符）、ParallelStorage（N 通道条带）、libckpt/ckptd/balancer/manifest（guest 软件
栈）、PcieLink（带宽受限链路，已建模 128b/130b + TLP 开销）、分析模型
`analytical_model.py`。这些**全部复用**。

target2 唯一要推翻的是 **P10 里的一个建模假设**：P10 把 SimCkptDevice 的
`dma` 从 `VectorRequestPort`（每 lane 一个 DmaPort）**回退成了单个共享 `dmaPort`，
并在前面放了一个共享 `PcieLink`**——这正好建模了「单共享上行」旧拓扑。target2 要
把它改回「每 lane 独立 DmaPort + 每 lane 独立 PcieLink」，并对 DRAM lane 与 CXL
lane 分别接到不同后端（host DRAM vs CXL），从而建模新拓扑的双独立线。

新增阶段编号用 **E0–E5**（epoch 2），避免与 P0–P10 混淆。

---

## 五、微调后的验收矩阵

保留原有「正确性」与「分链路统计」的骨架，把「双路径带宽」从"高于最快单路径"
升级为"两条独立 PCIe 线可加"，并新增 3 条拓扑/物理层现实性指标。**粗体为主
（staging）指标，普通字体为保留的从属指标。**

| # | 验收项 | 指标 / 判据 | 对应阶段 | 与旧矩阵的关系 |
|---|---|---|---|---|
| **A1** | **双独立 PCIe 线 staging 带宽可加** | 覆盖两个场景：**S1 单 GPU 双路径（每路 1 通道）加速比 ≈1.5–1.6×**；**S2 4 GPU 双路径加速比 ≈2.0×**。判据 `B_stage ≈ B_line1 + B_line2`，且 `B_stage > max(B_line1, B_line2)`，同时 `B_stage > 旧共享上行拓扑的 B_stage` | E0/E4 | 升级旧 #2 |
| **A2** | **两条线不共享 switch 上行** | CXL lane 的流量不经过 Root Complex 上行（per-link 计数器证明 CXL lane 的字节不计入 uplink 方向） | E0/E2 | 新增 |
| **A3** | **多通道后端线性扩展** | host DRAM 4 DIMM 与 CXL expander 4 DIMM（8 DDR 子通道 = 200 GB/s）各自的多通道并发使单线带宽随通道数上升；S2 里 CXL 后端不再成为瓶颈 | E1/E4 | 拆自旧 #2/#3 |
| **A4** | **纯 TLP、无 flit（物理层现实性）** | CXL 线与 DRAM 线同式建模：128b/130b + TLP header/LCRC/framing/DLLP，**不乘 64/68 flit 折算**；Gen5 x16 有效 ≈ 55.53 GB/s、Gen4 x16 ≈ 27.77 GB/s（对齐论文量级） | E3 | 更新 |
| **A5** | **On-device DMA / P2P 语义（核心）** | CXL 线由 expander 的 on-device DMA 发起、直击 GPU HBM；CPU 只发 CXL.io 控制面（发现/配置/描述符/doorbell），数据面纯 TLP；建模 CPU→expander 的 **CXL.io 初始命令时间** | E2 | 升为核心 |
| A6 | 数据一致（GPU→DRAM/CXL→…→GPU CRC/SHA 一致） | 每 chunk CRC32 + 全 checkpoint SHA-256 + GPU 端逐字节重放比对 | E4 | 保留旧 #1/#5 |
| A7 | 分链路统计 | 每条线独立统计排队/带宽/延迟/重试/完成字节（BAR 只读寄存器） | E0 | 保留旧 #8 |
| A8 | CXL 热备命中恢复加速 | 命中率 0/25/50/100% 下恢复时间单调下降 | E4 | 保留旧 #4 |
| A9 | 后台落盘与压力/生命周期 | 2 存储通道 > 单通道；压力下 pinned pool 只复用不扩大；在飞 DMA 完成前禁 unpin | E4 | 保留旧 #3/#6/#7 |

> 主指标 A1–A5 全部围绕 staging；A6–A9 保证不回归 target.md 已交付的正确性与
> 从属功能。A1 是核心，A2/A3/A5 是"为何能加速"的机理，A4 是"算得对不对"（本版
> 关键变化：**A4 改为纯 TLP 无 flit**，**A5 升为 on-device DMA 核心语义 + CXL.io
> 初始命令时间**）。

---

## 六、开发计划（E0–E5）

原则：**最小改动复用 P0–P10 成果，只在数据通路与后端拓扑上动手**；每个阶段都给出
关键接口、文件位置与验证方法，且都可用宿主机（无 gem5）单测或 gem5 周期仿真闭环。

### E0：双独立 PcieLink 骨架（推翻"单共享上行"）

目标：把 SimCkptDevice 从「单 dmaPort + 单 PcieLink」改回「每 lane 一个 DmaPort
+ 每 lane 一个 PcieLink」，DRAM lane 与 CXL lane 分别接不同后端。

改动：

1. `src/dev/storage/SimCkpt.py`：`dma` 恢复为 `VectorRequestPort("Per-lane DMA
   port (one per queue)")`。
2. `src/dev/storage/sim_ckpt_device.{hh,cc}`：恢复 P10 的 `dmaPorts`（每 queue 一个
   `DmaPort`）；`getPort("dma", idx)`、`init()` 逐 lane 检查连通；`tryFillSlots`/
   `onDmaDone`/`postCompletion` 改回用 `dmaPorts[qIdx]`。
3. `src/dev/x86/SouthBridge.py`：去掉对 `simckpt_device` dma 的 `continue` 特判
   （或改为逐 element 连接）。
4. `src/python/gem5/components/boards/x86_board.py`：
   - `self.simckpt_pcie_links = [PcieLink(...), PcieLink(...)]`（每 lane 一条）；
   - lane 0 的 `PcieLink.mem_side_port` 接 Ruby DMASequencer（走 host 内存）；
   - lane 1 的 `PcieLink.mem_side_port` 接 CXL 通路；
   - `get_dma_ports()` 返回两条 link 的 `mem_side_port`；
   - `simckpt_device.dma[i] = links[i].cpu_side_port`。
5. 给 `PcieLink` 增加 per-link 方向统计（见 A2：记录经该 link 的写字节数，供
   验证 CXL lane 不占上行）。

验证：

- gem5 跑 `x86-cxl-ckptd-e1.py`（复用现有 pure-stage 脚本），确认两个 lane 各自
  走独立 link、无共享上行。
- 宿主机：把 PcieLink 的 `computeTickPerByte` 单测（Gen4 vs Gen5、128b/130b、TLP
  开销）加进 `tests/cxl_tests/simckpt/`（无 gem5 也能算）。

### E1：多通道后端（4 DIMM host DRAM + CXL expander 4 DIMM = 8 DDR）

目标：每条线吃到多通道并发，让单线带宽不再被单通道上限卡死；CXL 后端升到 200 GB/s
后不再成为 S2 瓶颈。

改动：

1. `configs/.../x86-cxl-ckptd-e1.py`（及后续 e2/e4 脚本）：
   - host DRAM：`ChanneledMemory(DDR5_4400_4x8, 8, 64, size=...)` —— 4 DIMM，每个
     DIMM = 2 条 32-bit 子通道 = 8 子通道（对齐拓扑图 DDR0..DDR7）。
   - CXL DRAM：`ChanneledMemory(DDR5_4400_4x8, 8, 64, size=...)` —— expander 后端
     4 DIMM = 8 DDR 子通道（50 GB/s / 64-bit DIMM，合计 ~200 GB/s）。
2. `src/python/gem5/components/boards/x86_board.py`：`_setup_io_devices` 里 CXL
   memory 接线保持单 `cxl_mem_ctrl`，但把 `cxl_dram` 换成 8 通道 `ChanneledMemory`
   （单 expander、多 DDR；"多个 expander" 留作 E5 扩展）。
3. 确认 host DRAM 的 E820 / 地址区间与 4 DIMM 交错正确（`ChanneledMemory` 已做
   地址交错，无需新代码，但要验证 range）。

验证：

- 复用 `ckptbench_e1` 的 single-lane 模式：lane 0（4 DIMM）与 lane 1（CXL 4 DIMM）
  各自单线带宽应随通道数上升，且 CXL 后端带宽 ≥ Gen5 x16 链路（55.53 GB/s），不再
  成为瓶颈。
- 宿主机：扩展 `analytical_model.py` 的通道数扫描，验证
  `B_line ≤ min(link, Σ channel_bw)` 的取 min 逻辑。

### E2：On-device DMA / P2P 语义 + CXL.io 控制面建模

目标：让 CXL 线在语义上"由 expander 的 on-device DMA 发起、直击 GPU HBM、数据面纯
TLP"，CPU 只走 CXL.io 控制面；并建模 CPU→expander 的 **CXL.io 初始命令时间**。

现状与差距：当前 Ruby 通路是
`GPU(PyTrafficGen/SimCkptDevice) → DMASequencer → Directory → CXLMemCtrl`，这在
语义上仍经过 host 的 Ruby 目录（即 host 侧发起）。新拓扑里 CXL 线是 P2P，由 expander
的 on-device DMA 发起，数据面不经 host 目录、无 CXL.mem flit。

改动（分两步，先语义后精确）：

1. **语义层（MVP）**：给 `CXLMemCtrl` 增加"on-device DMA"能力描述——在
   `cxl_mem_ctrl.{hh,cc}` 增加 `devDmaLatency` / 一组"发起者身份"统计，把 CXL
   lane 的 DMA 描述成"expander 侧 P2P 读 GPU HBM"，显式区分 `host_initiated` 与
   `device_initiated` 两套路径统计（A5/A7 的依据）。
2. **控制面（CXL.io）**：CPU 发 CXL.io（发现/配置/DMA 描述符/doorbell）给 expander，
   建模为一次**初始命令时间**（`init_cmd_latency`，例如 CPU→switch→expander 的
   CXL.io TLP 往返 + expander 解析 + DMA 引擎启动）；数据搬运一旦启动就不再占 CPU。
3. **通路层**：让 lane 1 的 `PcieLink.mem_side_port` 直接接 CXL 通路（绕过 host
   Ruby 目录），或接一个"P2P 直连"端口，使 CXL lane 的请求不进入 host Directory /
   不占 host DRAM 控制器。
4. **写完成开销建模**：在 PcieLink 的写方向加入"写完成回程"占用（非 posted 写要等
   completion，贴齐论文 ~1.5× 读/写差）。

验证：

- gem5：lane 1 单独 staging 时，host DRAM 控制器统计字节数为 0、Ruby 目录统计为 0，
  只有 CXL lane 计数器增长（A2/A5 的判据）；CXL.io 初始命令时间作为固定前导延迟
  计入 staging 总时间。
- 宿主机：PcieLink 读/写不对称单测，验证写带宽 ≈ 读带宽 × 0.6–0.65。

### E3：PCIe 物理层精细化（无 flit，两线同式）+ 对标论文

目标：把链路建模精度提到论文可对标水平，并落实"**CXL 线纯 TLP、无 64/68 flit 折算**、
与 DRAM 线同式"。

改动：

1. `PcieLink` 参数化：GPU link 为 Gen4（`line_rate_gtps=16`），switch 上行与
   expander 直连为 Gen5（`line_rate_gtps=32`）。**CXL 线不再乘 64/68**，与 DRAM 线
   同式（128b/130b + TLP header/LCRC/framing/DLLP）→ 单 Gen5 x16 有效 ≈ 55.53 GB/s。
2. 增加 `max_payload_bytes`/MPS 与多描述符提交的建模（对应论文"多线程描述符提交把
   峰值 block size 从 8 MB 降到 256 KB"）：SimCkptDevice 的 queue_depth/批量提交
   与 PcieLink 的 `req_size`（credit 深度）联动。
3. 读/写不对称参数化（E2-4 的落点）：`PcieLink` 写方向按"posted + completion"建模。

验证（对标论文，用 gem5 单 lane Gen5 x16 测量）：

| 项 | 期望 |
|---|---|
| 单 Gen5 x16 有效带宽 | ≈ 55.53 GB/s（128b/130b + TLP 开销，无 flit） |
| 单 Gen5 x16 读 | ~51 GB/s（≈80% 有效峰值） |
| 单 Gen5 x16 写 | ~33 GB/s（写完成开销，读/写 ≈1.5× 差） |
| 峰值 block size（多描述符） | 从 ~8 MB 降到 ~256 KB |

### E4：双线 staging 验收 + 分流策略重标定

目标：跑通第五节 A1–A9，**落实第七节 S1/S2 两个核心加速比场景**，证明新拓扑确实
提高 staging 速度。

改动：

1. 新 bench：
   - `tests/cxl_tests/simckpt/bench/ckptbench_e4_s1.c`：**S1 单 GPU 双路径**——
     single DRAM（1 通道）、single CXL（1 通道）、dual（1 DRAM ch + 1 CXL ch），
     单卡数据 ≥256 MiB（默认 512 MiB），输出加速比 `B_dual / B_single`。
   - `tests/cxl_tests/simckpt/bench/ckptbench_e4_s2.c`：**S2 4 GPU 双路径**——
     DRAM-only 基线（4 GPU 全走上行→4 DIMM）vs dual（分走 DRAM 4 DIMM + CXL
     2 lane），每 GPU ≥256 MiB，输出 `B_dual / B_dram_only`。
   - 二者都带"旧共享上行"对照组（临时把两 lane 复用一个 link），直接量化 A1。
2. `balancer.{h,c}` 重标定：去掉"单共享上行"的隐式串行假设，两 lane 的
   `predicted_finish_i = queued_bytes_i / R_i` 不再叠加共享上行惩罚项；分流量按
   `B_line1`/`B_line2` 的比例自然形成（沿用 target.md 第五节的"最近 N 个 chunk
   去向"统计）。
3. 更新 `analytical_model.py`：新增 `bsave_bound_dual_link()`，并更新为无 flit 版本
   （`B_line = min(链路有效带宽, Σ channel_bw)`，CXL 与 DRAM 同式），
   `B_stage = min(Σ GPU egress, B_line1 + B_line2)`，给 `topology.{h,c}` 加"每 line
   独立 link"的候选路径。

验证：

- 宿主机 `e4_scale.c`：S1（512 MiB 单卡）与 S2（4×512 MiB）全量毫秒级验证均衡
  split 与可加性上界（周期仿真太慢，全量走宿主机；见第七节"数据量与仿真规模"）。
- gem5 `x86-cxl-ckptd-e4.py`：跑 A1–A9，输出分链路统计与 S1/S2 的 staging 带宽与
  加速比，期望 `B_stage ≈ B_line1 + B_line2` 且显著高于"共享上行"对照组。

### E5：分析模型协同 + 扩展（多 expander、多 GPU）

目标：把周期仿真的抽样结论回填分析模型，并扩展到拓扑的完整形态（多个 CXL expander
直连 switch、多 GPU 各自独立 Gen4 link）。

改动：

1. `analytical_model.py` 支持 N 个 CXL expander、M 个 GPU 的扫描（每 GPU 独立
   Gen4 x16，switch 上行 Gen5 x16 为多 GPU 去 DRAM 的共享瓶颈，每个 expander
   Gen5 x16 直连、on-device DMA、纯 TLP）。
2. 周期仿真抽样验证分析模型在关键拓扑点的预测（借 P8 的 trace 回放 + 对照框架）。
3. 输出论文级分链路报告（staging 带宽、GPU stall time、每 lane P95、上行占用率、
   CXL.io 初始命令时间）。

验证：

- 分析模型与周期仿真在 1 GPU/4 GPU、1/N CXL expander 等关键点的误差 <5%（沿用 P8
  的"模型 vs 实测并排"输出格式）。

---

## 七、两个核心验收测试场景（S1 / S2）

A1 的两个加速比场景。两者都以"写 staging"为方向（GPU→内存），用 steady-state 带宽
度量；**单卡数据量 ≥256 MiB，默认/推荐 512 MiB**（readme_cxl.md 结论：512 MiB 才
能摆脱 ~1 ms 固定开销、到达稳态 ~28+ GB/s）。本版额外计入 CPU→expander 的 **CXL.io
初始命令时间**（固定前导延迟，见 A5/E2）。

有效带宽（本版统一效率系数 ≈86.8%）：Gen5 x16 raw 64 → **55.53 GB/s**；Gen4 x16
raw 32 → **27.77 GB/s**。

### S1：单 GPU 双路径（每路 1 通道）加速比

问题：单卡只有一个 Gen4 x16 link（有效 ≈27.77 GB/s），而单条 DDR 子通道只有
~17.6 GB/s。旧拓扑下 GPU 只写 DRAM 单通道时，链路远未吃满；新拓扑下把数据分流到
两条独立后端（1 条 DRAM 通道 + 1 条 CXL 通道），让两条通道的控制器并行吸收，从而
吃到 GPU link 的全部带宽。

| 项 | 值 |
|---|---|
| 拓扑 | 1× GPU（Gen4 x16，有效 27.77 GB/s）；Line1 = 1 条 DRAM 通道；Line2 = 1 条 CXL 通道 |
| 基线 | 单路径：仅 DRAM 1 通道，`B_single ≈ 17.6 GB/s`（通道受限） |
| 实验 | 双路径：1 DRAM ch + 1 CXL ch 并发 `B_dual` |
| 数据量 | **≥256 MiB，推荐 512 MiB**（每 GPU） |
| 指标 | `speedup = B_dual / B_single` |
| 期望 | `B_dual ≈ min(27.77, 17.6+17.6)=27.77 GB/s`（GPU link 限）；**speedup ≈ 1.58×（~1.5–1.6×）** |

判据：`speedup > 1.3` 且 `B_dual` 逼近单卡 Gen4 x16 link 上限（27.77 GB/s，证明两
路不因共享上行互相踩踏，单卡就能靠双后端并行把 GPU link 吃满）。S1 的加速比受
GPU 自身 Gen4 link 上限约束，与 CXL 后端容量/链路代际关系不大，故本版仍 ~1.5–1.6×。

### S2：4 GPU 双路径（多通道，CXL 后端充足）加速比

问题：4 卡合计 egress ≈ 4×27.77 = 111 GB/s，而 switch 上行只有 Gen5 x16 有效
≈55.53 GB/s。旧拓扑"只写 DRAM"时，4 卡全挤同一条上行，上行就是硬瓶颈。新拓扑把
一部分 staging 迁到 switch 本地的 CXL expander（Gen5 x16 直连、on-device DMA、纯
TLP、不占上行），给 DRAM 线腾出上行，总 staging 带宽越过上行上限。

| 项 | 值 |
|---|---|
| 拓扑 | 4× GPU（各 Gen4 x16 = 27.77 GB/s，合计 111）；Line1 = 上行 Gen5 x16 = 55.53；Line2 = expander Gen5 x16 = 55.53（后端 4 DIMM = 200 GB/s，非瓶颈） |
| 基线 | 传统只写 DRAM：4 GPU 全走上行，`B_dram_only = min(111, 55.53) = 55.53 GB/s`（上行限） |
| 实验 | 双路径：分流到 DRAM（走 55.53 上行）+ CXL expander（55.53 P2P）并发 `B_dual` |
| 数据量 | **每 GPU ≥256 MiB（合计 ≥1 GiB），推荐每 GPU 512 MiB（合计 2 GiB）** |
| 指标 | `speedup = B_dual / B_dram_only` |
| 期望 | `B_line1 = B_line2 = 55.53`；`B_dual = min(111, 55.53+55.53) = 111 GB/s`；**speedup ≈ 2.0×**（旧模型 1.45×） |

判据：`speedup > 1.5`（期望 ≈2.0×），且 CXL lane 的 per-link 统计显示其流量不计入
上行方向（联动 A2）；CXL 后端 200 GB/s 不再成为瓶颈（联动 A3）。**本版 S2 从旧
1.45× 升到 2.0× 的根因：line_cxl 从 Gen4 x16（带 flit 折算）升到 Gen5 x16 直连
（无 flit），与上行同速，两线可加值翻倍。**

### 数据量与仿真规模（可行性与一致性）

- **稳态阈值**：512 MiB 是摆脱固定开销、到达稳态带宽的最低量级，S1 必须满足；S2
  按每 GPU 256 MiB 起步。
- **固定开销两项**：① DMA 流水线冷启动/尾排（~1 ms，readme_cxl.md）；② CPU→
  expander 的 **CXL.io 初始命令时间**（发现/配置/描述符/doorbell，E2 建模，固定
  前导延迟）。两者都计入端到端 staging 时间，但不影响稳态带宽。
- **周期仿真成本**：S1（≤512 MiB）可直接在 gem5 里跑；S2（4×512 MiB=2 GiB）全量
  周期仿真过慢，采用 P8–P10 一贯的"**宿主机毫秒级全量单测 + gem5 抽样验证**"：
  - 宿主机 `e4_scale.c`：S2 全量 4×512 MiB 跑均衡 split + 可加性上界；
  - gem5：S2 用 4×64/128 MiB 抽样点，验证带宽/加速比趋势与宿主机一致，再外推到
    512 MiB。
- **计时口径**：沿用 readme_cxl.md 的修正方法——`max_outstanding_reqs` + 等
  `bytesWritten == copy_bytes` 后才计时，度量"落库完成"的端到端带宽，而非注入
  吞吐。

---

## 八、合理性论证（为什么这个模拟内容既现实、又能加速 staging）

1. **两线可加性是物理真实，不是假设**。论文的核心机制就是"expander 在 switch 本地
   用 on-device DMA 发 P2P、纯 TLP 直击 GPU HBM、CPU 只走 CXL.io 控制面"，因此
   CXL 线天然不占上行、无 CXL.mem flit。我们的 E0/E2 把这个机理显式建模成"两条
   独立 PcieLink + CXL 线绕开 host 目录 + 纯 TLP"，是论文拓扑的直接仿真。
2. **PCIe 层按规范建模，不手拍效率系数**。`PcieLink` 按 128b/130b 线编码 + TLP
   header/LCRC/framing/DLLP 开销算 `tickPerByte`；**CXL 线与 DRAM 线同式（无
   64/68 flit）**，Gen4/Gen5 分速率（有效 27.77 / 55.53 GB/s）。E3 对标论文
   （Gen5 x16 读 ~51/写 ~33 GB/s）是**可复现的收敛目标**。
3. **多通道后端（Host 4 DIMM + CXL expander 4 DIMM = 8 DDR）匹配现实服务器**。
   拓扑图 DDR0..DDR7 = 4 个 DDR5 DIMM（每 DIMM 2 子通道），CXL expander 后端
   4 DIMM = 8 子通道 ≈ 200 GB/s；`ChanneledMemory` 已做地址交错，直接复用。
4. **加速来源清晰、可量化**。旧拓扑 `B_stage ≤ 上行（Gen5 x16 ≈ 55.53 GB/s）`；新
   拓扑 `B_stage ≈ B_line1 + B_line2`（两线同速 Gen5 x16），且 Line 2 不占上行。
   4 GPU 合计 egress 111 GB/s 时，DRAM-only = 55.53（上行限），dual = 111 →
   **2.0×**。A1 的"新 > 旧"对照组直接度量这个加速。
5. **复用已交付资产，不重复造轮子**。SimCkptDevice 多队列、per-link 统计、
   FLAG_STAGE、libckpt/ckptd/balancer/manifest、ParallelStorage、分析模型全部沿用；
   target2 只动"数据通路 + 后端拓扑"这一层，风险可控。
6. **每个阶段可独立验证**。E0–E3 各有宿主机单测或 gem5 小样闭环，E4 一次性跑
   验收矩阵，E5 才做全量扩展——符合 P0–P10 一贯的"宿主机毫秒级单测 + gem5 抽样"
   分层方法（见 `exec_summary.md` 测试方法一节）。

---

## 九、参考资料

- 论文（本计划理论依据）：*Replacing NVMe Staging in LLM Inference with a
  High-Bandwidth CXL Memory Expander with an On-Device DMA Controller*,
  Burru, Nalla, Prasad, SIGCOMM 2026, DOI 10.1145/3789240.3822568。
- 同组配套论文：*CXL Memory for LLM Inference Staging via an On-Device DMA
  Controller*, DOI 10.1145/3837053.3837356。
- PCIe 基础规范（128b/130b、Gen4=16 GT/s、Gen5=32 GT/s、MaxPayloadSize、
  posted/non-posted 写完成语义）。
- CXL 3.x 规范（CXL.mem、Type-3 设备、HDM 解码、P2P）。
- 仓库内：`target.md`（前作验收矩阵与 P0–P10）、`exec_summary.md`（已交付成果）、
  `readme_cxl.md`（GPU→CXL 带宽方法）、`src/dev/storage/pcie_link.{hh,cc}`、
  `src/dev/storage/sim_ckpt_device.{hh,cc}`、`src/dev/x86/cxl_mem_ctrl.{hh,cc}`、
  `tests/cxl_tests/simckpt/analytical_model.py`。
