# SimCXL target2 执行总结 v2（GPU 双线 staging 加速）

本文记录 `configs/example/gem5_library/target2.md`（GPU 双线 staging 加速，CXL
同域拓扑）的开发过程：参数设定、各阶段成果、遇到的困难与解决办法。配套规划与验收
矩阵见 target2.md。

> **本版（v2.1）更新**：按核心改动重测——**无 CXL switch（expander 端点直连 PCIe
> switch，Gen5 x16）、纯 TLP 无 flit、line_cxl Gen4→Gen5、CXL 后端 2×32→4 DDR×50
> = 200 GB/s（8 DDR 通道）、加入 CPU→expander CXL.io 初始命令时间**。新增 §1 记录
> 重测方法与结果；§2–§5 为上一版（机制验证）结果，保留作对照。

---

## 0. 目标回顾

把 checkpoint staging 从「单共享 PCIe 上行」改成「两条独立、每条多通道的 PCIe
数据线」：

- Line 1（GPU→DRAM）：`GPU → PCIe Switch → Root Complex（上行 Gen5 x16）→ Host DRAM`
- Line 2（GPU→CXL）：`GPU → PCIe Switch → CXL Expander（端点直连 Gen5 x16，
  on-device DMA，纯 TLP）→ CXL DDR`（switch 本地 P2P，不占上行）

核心指标：两条线 staging 带宽可加（`B_stage ≈ B_line1 + B_line2`），并显著高于旧
共享上行拓扑。两个测试场景：S1（单 GPU 双路径，每路 1 通道）、S2（4 GPU 双路径，
多通道 + CXL 后端充足）。

---

## 1. 核心改动重测（v2.1）

### 1.1 三处模型修正 + 一处新增

| 改动 | 内容 | 代码落点 |
|---|---|---|
| ① 移除 flit 折算 | CXL 线不再乘 64/68，与 DRAM 线同式（128b/130b + TLP header/LCRC） | `PcieLink` 本就无 flit 因子，两线同式；无需改代码 |
| ② line_cxl Gen4→Gen5 | expander 直连 Gen5 x16，与上行同速，有效 ≈55.53 GB/s | `x86_board.py` 两 lane 均 `line_rate_gtps=32`；`tlp_overhead_bytes` 24→**35**（64→55.5 GB/s） |
| ③ CXL 后端 2×32→4 DDR×50 | 4 DIMM = 8 DDR 子通道 = 200 GB/s（不再成为瓶颈） | config `--cxl-channels 8`（`ChanneledMemory(DDR5_4400_4x8, 8, ...)` = 140.8 GB/s，远超 55.5 线限，等效） |
| ④ CXL.io 初始命令时间 | CPU→expander 控制面（发现/配置/描述符/doorbell）固定前导延迟 | 分析模型已计入；周期仿真实现待 E2 |

### 1.2 有效带宽标定

- Gen5 x16：`line_rate_gtps=32, num_lanes=16, tlp_overhead_bytes=35` →
  `32×16×128/130 = 504.6 Gbps`，`×256/(256+35) = 443.8 Gbps ≈ 55.5 GB/s`（目标
  55.53）。
- 实测（纯写 staging，单 lane）：single DRAM **55.43 GB/s**，single CXL（8ch）
  **55.38 GB/s**——与 55.53 一致（±0.2%）。

### 1.3 重测结果：CXL 后端 2ch → 8ch 的对比（`ckptbench_e1` 纯写，2 MiB，PASS）

| CXL 后端 | single DRAM | single CXL | dual-lane | sum | dual/single DRAM |
|---|---|---|---|---|---|
| 2 通道（旧） | 55.43 | **31.94**（后端受限） | 87.00 | 87.38 | 1.57× |
| **8 通道（新）** | 55.43 | **55.38**（链路受限） | **109.58** | 110.82 | **1.98×** |

关键结论：

- **line_cxl 升到 Gen5 后，CXL 后端从 2ch（35.2 GB/s）升到 8ch（140.8 GB/s）把
  CXL lane 从后端受限（31.94）提到链路受限（55.38）**，与 DRAM lane（55.43）同速。
- dual-lane 从 87.00 → **109.58 GB/s ≈ sum（110.82）**，两条 Gen5 x16 线完全可加。
- 这直接验证 ②+③：`B_line1 = B_line2 = 55.4 GB/s`，`B_dual ≈ 110.8 GB/s`，与
  target2 S2 的 `B_line1=B_line2=55.53, B_dual=111` 一致。

### 1.4 S1 / S2 重算（分析模型 `analytical_model.py`，已更新为无 flit 版本）

| 场景 | 结果 |
|---|---|
| S1（1 GPU，1 DRAM ch + 1 CXL ch） | single 17.60，dual = min(27.77, 35.2) = 27.77，**1.58×** |
| S2（4 GPU） | DRAM-only = min(111, 55.53) = 55.53，dual = min(111, 111.06) = 111，**2.00×** |
| S2（2 GPU） | 1.00×（2 GPU 合计 55.5 = 上行，无争抢） |

有效带宽常数：Gen5 x16 = 55.53，Gen4 x16 = 27.77（4 GPU 聚合 = 111）。

**S1 从旧 1.98× 降到 1.58× 的原因**：旧周期仿真用 per-lane Gen5 link（每 lane 一条
55.4 链路），单 GPU 双路径被误建模成"两条独立 55.4 链路"，dual = 2×17.6 = 35.2
（通道限）→ 1.98×；正确的单 GPU 是**一条共享 Gen4 x16 link（27.77）**，dual 被
GPU link 卡在 27.77 → 1.58×。**这是建模边界，见 §1.5。**

**S2 从旧 1.56× 升到 2.0× 的原因**：line_cxl 从 Gen4（带 flit，~29 GB/s）升到
Gen5（无 flit，55.53），与上行同速，两线可加值翻倍。

### 1.5 建模边界（诚实结论）

- **周期仿真的 `PcieLink` 是"线链路"（Gen5），不是"GPU 链路"（Gen4）**。每 lane
  一条 Gen5 `PcieLink` 建模的是"DRAM 线 / CXL 线"的上行/expander 链路，而非单 GPU
  的 Gen4 上行。因此：
  - 周期仿真的 2-lane 结果（dual 109.58）正确验证了 **S2 的"两线可加"机制**；
  - 但周期仿真**不能直接**给出 S1 的 1.58×（那需要单 GPU 一条共享 Gen4 link），
    S1 的 1.58× 由分析模型给出。
- **S2 的"4 GPU 聚合 111 GB/s > 上行 55.53"这一约束**也由分析模型给出（周期仿真
  的单 lane 无法表达"多 GPU 共享上行"）。
- 完整 S1（共享 Gen4 GPU link）与 S2（4 lane + 共享上行 crossbar）的周期仿真留作
  target2 E5。

### 1.6 新增困难（相对上一版）

7. **`x86_board.py` 改参数字面值也需重新 `scons`**：gem5 stdlib 的 `.py`（含
   `x86_board.py`）被 marshalling 进二进制（`build/X86/python/.../x86_board.py.cc`），
   改 `tlp_overhead_bytes 24→35` 后不重编仍跑出旧值（单 lane 57.63 而非 55.43）。
   教训：任何 `src/python/` 下的改动都要 `scons build/X86/gem5.opt` 后再跑。

---

## 2. 编译与测试方法（新增 E1 / E2 组件）

新增组件：`PcieLink`（`src/dev/storage/pcie_link.{hh,cc}` / `PcieLink.py`，已在
`SConscript` 注册）、`ckptbench_e1`（E1 纯写 staging bench）、`ckptbench_e4_s1`
（S1 bench）、`x86-cxl-ckptd-e1.py` / `x86-cxl-ckptd-e4.py`（config），以及
`x86_board.py` 的每-lane `PcieLink` 接线与 `tlp_overhead_bytes=35`。

### 2.1 构建 gem5（含 E0/E3 代码改动）

```bash
cd <SimCXL>
scons build/X86/gem5.opt -j$(nproc)
```

> 坑：`src/python/` 下的 stdlib `.py`（含 `x86_board.py`）被 marshalling 进二进制，
> 改 `tlp_overhead_bytes`/接线后**必须重编**，否则跑旧值（§1.6 第 7 条）。

### 2.2 编译 guest 侧 benchmark（静态，无需内核模块）

```bash
cd tests/cxl_tests/simckpt
make ckptbench_e1 ckptbench_e4_s1
# 产出 ./ckptbench_e1、./ckptbench_e4_s1（-O2 -static）
```

依赖：`engine/libckpt.c ckptd.c engine/gpu_dma_engine.c`（见 Makefile）。

### 2.3 把 benchmark 注入 disk image

```bash
mkdir -p /tmp/img && mount -o loop,offset=$((2048*512)) /root/simcxl-resources/parsec.img /tmp/img
cp ckptbench_e1    /tmp/img/home/test_code/ckptbench_e1
cp ckptbench_e4_s1 /tmp/img/home/test_code/ckptbench_e4_s1
chmod +x /tmp/img/home/test_code/ckptbench_e1 /tmp/img/home/test_code/ckptbench_e4_s1
sync && umount /tmp/img
```

### 2.4 运行 E1 纯写 staging 测试（验证 ②line_cxl Gen5 + ③CXL 后端 8ch）

```bash
# 8 通道 CXL 后端（新，v2.1）
./build/X86/gem5.opt -d m5out-e2-8ch \
  configs/example/gem5_library/x86-cxl-ckptd-e1.py \
  --ckpt-mib 2 --dram-channels 8 --cxl-channels 8

# 2 通道 CXL 后端（旧，对照组）
./build/X86/gem5.opt -d m5out-e2-2ch \
  configs/example/gem5_library/x86-cxl-ckptd-e1.py \
  --ckpt-mib 2 --dram-channels 4 --cxl-channels 2
```

结果见 `m5out-e2-*/board.pc.com_1.device`（期望 8ch：single CXL 55.38、dual
109.58；2ch：single CXL 31.94、dual 87.00）。`ckptbench_e1` 参数：`--ckpt-mib`
（数据量 MiB）、`--chunk`（默认 4096）。

### 2.5 运行 S1（`ckptbench_e4_s1`，descriptor FLAG_STAGE，≥256 MiB）

```bash
./build/X86/gem5.opt -d m5out-e4-s1 \
  configs/example/gem5_library/x86-cxl-ckptd-e4.py \
  --ckpt-mib 256 --dram-channels 1 --cxl-channels 1
```

结果见 `m5out-e4-s1/board.pc.com_1.device`。注意：descriptor 路径在大数据量下
受 guest 侧 `memset`/描述符构建开销限制（§7 困难 3/4/5），带宽/加速比优先用纯写
模式（§2.4）。

### 2.6 宿主机分析模型（S1 / S2 加速比，无需 gem5）

```bash
python3 tests/cxl_tests/simckpt/analytical_model.py
# 或直接调用：
python3 -c "import sys; sys.path.insert(0,'tests/cxl_tests/simckpt'); \
  import analytical_model as m; \
  print(m.s1_dual_bound()); print(m.s2_dual_bound(4))"
```

输出：S1 1.58×、S2(4 GPU) 2.00×。

---

## 3. E0：双独立 PcieLink 骨架（推翻"单共享上行"）【上一版·机制验证】

### 改动

| 文件 | 改动 |
|---|---|
| `src/dev/storage/SimCkpt.py` | `dma` 由单 `RequestPort`（继承 `DmaDevice`）改回 `VectorRequestPort("Per-lane DMA port (one per queue)")` |
| `src/dev/storage/sim_ckpt_device.hh` | 恢复 `std::vector<std::unique_ptr<DmaPort>> dmaPorts`（每 lane 一个 DmaPort） |
| `src/dev/storage/sim_ckpt_device.cc` | 构造器恢复 `dmaPorts.emplace_back(std::make_unique<DmaPort>(this, sys, p.sid, p.ssid))`；`init()` 逐 lane 检查连通；`getPort("dma", idx)` 按 lane 返回；`tryFillSlots/onDmaDone/postCompletion` 改回 `dmaPorts[qIdx]->dmaAction(...)` |
| `src/dev/x86/SouthBridge.py` | 去掉 E1 加入的 `dev_attr == 'simckpt_device': continue` 特判，恢复通用 `hasattr(dma_port, "elements"): continue` |
| `src/python/gem5/components/boards/x86_board.py` | 单 `simckpt_pcie_link` → 每 lane 一个 `PcieLink`（`self._simckpt_pcie_links` + `setattr(self, f"simckpt_pcie_link{i}", link)` 注册 child）；`simckpt_device.dma[i] = link.cpu_side_port`；`get_dma_ports()` 返回每个 link 的 `mem_side_port` |
| `configs/example/gem5_library/x86-cxl-ckptd-e1.py` | `board.simckpt_pcie_link` → `for link in board._simckpt_pcie_links` |

### 参数设定（E0）

- 每个 `PcieLink`：`req_size=1024, resp_size=1024, line_rate_gtps=32（Gen5）,
  num_lanes=16（x16）, max_payload_bytes=256, tlp_overhead_bytes=24, delay=0ns`。
- 两个 lane 的 link 参数一致（Gen5 x16 ≈ 63 GB/s），先只验证"两线独立"机理，Gen4/
  Gen5 分层放到 E3。
- `num_queues=2, queue_depth=32`。

### E0 验证结果（`--ckpt-mib 2`，纯写 staging，`ckptbench_e1`）

| 拓扑 | single DRAM | single CXL | dual-lane | sum |
|---|---|---|---|---|
| 旧：单共享 uplink（m5out-e1） | 57.45 | 31.95 | 55.85 | 89.39 |
| **新：每 lane 独立 link（m5out-e0-smoke）** | 57.40 | 31.94 | **88.71** | 89.35 |

- **dual 从 55.85 → 88.71 GB/s，≈ sum（89.35），两线可加性成立**：去掉共享上行后，
  DRAM lane（57.4）与 CXL lane（31.9）不再互相踩踏。
- 加速比：vs 最快单路径 88.71/57.40 ≈ **1.55×**；vs 旧共享上行 88.71/55.85 ≈
  **1.59×**。

---

## 4. E1：多通道后端【上一版】

- host DRAM：`ChanneledMemory(DDR5_4400_4x8, dram_channels, 64, size="3GiB")`；
  `DDR5_4400_4x8` = 32-bit 子通道（17.6 GB/s 理论）。S1 用 1 通道，S2 用 8 子通道
  （4 DIMM）。
- CXL DRAM：`ChanneledMemory(DDR5_4400_4x8, cxl_channels, 64, size="8GB")`；S1 用
  1 通道，S2 用 2 通道（2 CXL lane）。
- 复用 gem5 标准库 `ChanneledMemory`（地址交错已内置），无需新 C++ 代码；CXL 侧仍
  为单 `CXLMemCtrl` + 多通道（"2 个 CXL Type-3"留作 E5 扩展）。

---

## 5. S1：单 GPU 双路径（每路 1 通道）加速比【上一版】

### 测试设计

- 新 bench `tests/cxl_tests/simckpt/bench/ckptbench_e4_s1.c`（`FLAG_STAGE` 描述符，
  每 chunk 物理地址，不要求连续）；新 config
  `configs/example/gem5_library/x86-cxl-ckptd-e4.py`
  （`--dram-channels 1 --cxl-channels 1`）。

### S1 结果（纯写 staging，`--dram-channels 1 --cxl-channels 1`，2 MiB，PASS）

```
single DRAM : 15.84 GB/s
single CXL  : 16.00 GB/s
dual-lane   : 31.61 GB/s (sum 31.85 GB/s)
speedup vs fastest single : 1.976x
lane q0(DRAM): own 16.03 GB/s   lane q1(CXL): own 15.99 GB/s
mismatches: 0
```

- **加速比 ≈ 1.98×**，dual ≈ sum（31.61 ≈ 31.85），两条 1 通道线完全可加。
- 每条线 ~16 GB/s（单 DDR5 子通道 ~17.6 GB/s 的 ~91%），**通道受限而非链路受限**
  （GPU link 63 GB/s 远未打满），符合预期。

### 数据量口径说明

- S1 用纯写 staging 模式（跳过描述符 fetch + completion 写）测量**纯 payload 写
  带宽**，固定开销极小（2 MiB 已接近通道稳态，单通道 15.84 ≈ 17.6×0.9）。
- 256 MiB / 512 MiB 的"稳态带宽"要求源自 **descriptor 路径**的 ~1ms 冷启动/尾排
  开销（readme_cxl.md）；纯写模式不受此影响。S1 的**加速比（dual/single）**与数据
  量无关（两条线都通道受限），2 MiB 即可干净测出 1.98×。
- descriptor 路径在大数据量下会因**元数据（描述符 fetch）与 payload 写在 DRAM 总线
  上的读/写交织**而额外变慢，故带宽测量优先用纯写模式。

---

## 6. S2：4 GPU 双路径加速比【上一版】

### 分析模型（`analytical_model.py` 新增 `s1_dual_bound` / `s2_dual_bound`）

| 场景 | 结果 |
|---|---|
| S1（1 GPU，1+1 ch） | single 17.60，dual 35.20，**2.00×**（实测 1.98×） |
| S2（4 GPU） | DRAM-only 63.00，dual 98.20，**1.56×** |
| S2（2 GPU） | DRAM-only 63.00，dual 63.00，1.00×（无加速，见下） |

S2 的物理含义（关键结论）：

- **只有 GPU 数足够多、使总 egress 超过 switch 上行时，双线才有加速**。4 GPU 各
  Gen4 x16 = 4×31.5 = 126 GB/s > 上行 Gen5 x16 63 GB/s → 上行是瓶颈。
  - DRAM-only：min(126, 63, 140) = **63 GB/s（上行限）**。
  - Dual：DRAM 线走上行 63 + CXL 线（2 lane）P2P min(63, 35) = **98 GB/s**。
  - 加速比 **1.56×**。
- 2 GPU 合计 63 GB/s 恰好 = 上行，无争抢，故无加速（1.00×）——这解释了为什么必须
  用 4 GPU 才能看到 S2 效应。

### 周期仿真证据（2-lane，E0 的共享 vs 独立对比）

S2 的机理（"上行被多卡挤爆 → CXL 下放 switch 本地释放上行"）在 2-lane 上已被 E0
直接证明：

| 拓扑 | dual-lane |
|---|---|
| 共享上行（旧，m5out-e1） | 55.85 GB/s（两 lane 挤同一条 63 GB/s 上行） |
| 独立 link（新，m5out-e0-smoke） | 88.71 GB/s（CXL 不占上行） |

**加速比 88.71/55.85 ≈ 1.59×**，与 4 GPU 分析值 1.56× 一致量级。

### 4 GPU 完整周期仿真（待办/后续）

完整 4 GPU S2 需要：`num_queues=4`（4 个 GPU lane，各 Gen4 x16 PcieLink）+ 一条
**共享 Gen5 x16 上行 PcieLink**（DRAM 线多 lane 汇入，需 `NoncoherentXBar` 做
fan-in）+ 2 条 CXL lane 直连。这是 target2 E5 的扩展项；当前已交付的 2-lane 周期
证据 + 4 GPU 分析模型已经覆盖 A1 的核心判据（两线可加、上行解除）。

---

## 7. 遇到的困难与解决（重要）【上一版，1-6 条 + §1.6 第 7 条】

1. **孤儿节点 `Attempt to instantiate orphan node`**：把 `PcieLink` 列表存成 Python
   `list` 后，`PcieLink` 无 parent。裸 list 赋值会被 `SimObject.__setattr__` 拒绝
   （"has no parameter"）。解决：列表名用 `_` 前缀（走 `object.__setattr__`），并在
   循环里 `setattr(self, f"simckpt_pcie_link{i}", link)` 把每个 link 注册为 board
   child（走 `__setattr__ → add_child`）。

2. **ring depth 太小导致提交静默失败**：`SIMCKPT_RING_DEPTH=16384`，
   `simckpt_submit_batch` 里 `n > RING_DEPTH` 返回 `-EINVAL`，S1 bench 没查返回值
   → 门铃没响 → `wait_q` 忙等 → 卡死（256 MiB = 65536 chunk > 16384）。解决：
   `SIMCKPT_RING_DEPTH` 提到 131072（支持 512 MiB）。

3. **每 chunk 一次 pagemap 系统调用过慢**：`simckpt_phys` 每 chunk `open/lseek/
   read/close` `/proc/self/pagemap`，65536 chunk 放慢 guest setup。解决：S1 bench
   内加 `phys_bulk()`，一次 `open` + 一次 `pread(n_pages*8)` 批量读 PFN 再内存解析。

4. **guest 侧 `memset` 打满 Timing CPU（O(bytes)）**：`ckpt_pool_alloc` 用
   `memset(buf,0,size)` fault-in，256 MiB 缓冲在 Timing CPU 下需数十分钟。解决：S1
   bench 用 `alloc_staging()`——先 `mbind(MPOL_BIND)` 设 NUMA 策略、再每页 touch 1
   字节 fault-in（O(pages) 而非 O(bytes)），直接把页落在目标 node（DRAM node0 /
   CXL node1），无需 `MPOL_MF_MOVE` 迁移。已打印 phys 地址验证：dram[0]=0xb000000
   （node0），cxl[0]=0x2f3c00000（node1）。

5. **descriptor 路径双 lane 不重叠（元数据 memcpy 延迟）**：`simckpt_submit_batch`
   先 memcpy 描述符再响门铃，第二个 lane 的门铃被第一个 lane 的 memcpy（O(chunks)）
   延迟，导致双 lane 串行。解决：新增 `simckpt_submit_batch_noring` +
   `simckpt_ring`，先 memcpy 两条 lane 的描述符、再背靠背响两个门铃，双 lane 真正
   并发启动。

6. **纯写模式 vs descriptor 路径的选择**：纯写模式（无描述符 fetch）测的是纯
   payload 写带宽，干净且快；descriptor 路径多出"描述符 read 与 payload write 在
   DRAM 总线交织"的读/写反转开销。带宽/加速比测量优先用纯写模式。

---

## 8. 参数速查表（含 v2.1 更新）

| 参数 | 值 | 说明 |
|---|---|---|
| `PcieLink.line_rate_gtps` | 32 (Gen5) / 16 (Gen4) | 每 line 速率（v2.1：两线均 Gen5） |
| `PcieLink.num_lanes` | 16 (x16) | |
| `PcieLink.max_payload_bytes` | 256 | |
| `PcieLink.tlp_overhead_bytes` | **35**（v2.1，原 24） | header(16)+LCRC(4)+framing/DLLP，无 flit |
| Gen5 x16 有效带宽 | **55.53 GB/s**（v2.1，原 63） | 32×16×128/130×256/291 |
| Gen4 x16 有效带宽 | **27.77 GB/s**（v2.1，原 31.5） | 同效率系数 ~86.8% |
| 4× GPU 聚合 egress | **111 GB/s** | 4 × 27.77 |
| `DDR5_4400_4x8` | 32-bit 子通道 17.6 GB/s | 1 DIMM = 2 子通道 |
| CXL 后端 | **8 子通道（4 DIMM）= 140.8 GB/s**（v2.1，原 2ch） | 等效"4 DDR×50=200"，远超 55.53 线限 |
| `num_queues` | 2 (S1) / 4 (S2) | DMA lane 数 |
| `queue_depth` | 32 | 设备每 lane 并发槽 |
| `SIMCKPT_RING_DEPTH` | 131072 | 支持 512 MiB 单批 |

---

## 9. 交付与验收对照（v2.1 更新后）

| 验收 | v2.1 结果 |
|---|---|
| A1 双线可加（S1/S2） | S1 分析 **1.58×**（单 GPU Gen4 27.77 限）；S2 分析 **2.00×**（DRAM-only 55.53 → dual 111）；2-lane 周期证据 dual 109.58 ≈ sum 110.82 |
| A2 两线不共享上行 | CXL lane 走独立 link（周期 2-lane 验证） |
| A3 多通道后端 | CXL 后端 8ch = 140.8 GB/s（等效 200），不再成为瓶颈：single CXL 31.94→55.38 |
| A4 纯 TLP 无 flit | 两线同式（128b/130b + TLP 35B），Gen5 x16 有效 55.43 实测（目标 55.53） |
| A5 On-device DMA/P2P + CXL.io 初始命令时间 | 分析模型已计初始命令时间；周期仿真 P2P 直连待 E2 |
| A6/A7/A8 正确性/统计 | mismatches=0；per-queue issue/done tick 周期精确统计已用 |

---

## 10. 状态与待办

- [x] E0 实现 + 验证（上一版：dual 88.71 vs shared 55.85 GB/s）
- [x] **v2.1 核心改动重测**：无 flit + Gen5 line_cxl + 8ch CXL 后端 → single CXL 31.94→55.38，dual 87.00→**109.58 GB/s**
- [x] 分析模型更新（无 flit）：S1 1.58×、S2 2.0×
- [ ] S2 完整 4 GPU 周期仿真（共享上行 crossbar，E5）
- [ ] E2（CXL lane P2P 直连 + CXL.io 初始命令时间 + 写完成开销）/ E3（Gen4 GPU 分层）
