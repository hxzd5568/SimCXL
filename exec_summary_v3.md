# SimCXL target2 执行总结 v3（S2 4-GPU 共享上行 + E2/E3 建模补全）

本文是 `exec_summary_v2.md` 的续篇，记录 `configs/example/gem5_library/target2.md`
`10. 状态与待办` 里**最后两项**的开发过程：

1. **S2 完整 4 GPU 周期仿真**（共享上行 crossbar，E5）
2. **E2**（CXL lane P2P 直连 + CXL.io 初始命令时间 + 写完成开销）/**E3**
   （Gen4 GPU 分层）

在 v2.1 已交付「两条独立 Gen5 x16 线（每 lane 一条 PcieLink）」的基础上，本版把
单 lane 链路拆成**两层**：GPU 侧 Gen4 x16（27.77 GB/s）与交换机侧 Gen5 x16
（上行/expander 55.53 GB/s），并用 `NoncoherentXBar` 把 4 条 GPU DRAM 线**汇入一条
共享 Gen5 上行**，从而在周期仿真里真正表达 S2 的「多卡挤爆共享上行 → CXL P2P 释放
上行」机理。

---

## 0. 目标回顾

target2 两个待办场景：

- **S2**：4 GPU（各 Gen4 x16 = 27.77，合计 111 GB/s）staging 到 host DRAM 走一条
  共享 Gen5 x16 上行（55.53）；分一部分 staging 到 switch 本地 CXL expander
  （Gen5 x16 P2P，不占上行）。期望 `B_dual ≈ 2 × B_dram_only ≈ 2.0×`。
- **E2/E3**：把链路建模补全为——GPU 链路 Gen4、上行/expander Gen5（分层）；CXL 线
  由 on-device DMA 走 P2P（不占上行）；CPU 只发一次 CXL.io 初始命令；写方向加
  「写完成回程」开销（非 posted 写，写 ≈ 读 / 1.5）。

---

## 1. 改动清单（代码落点）

| 文件 | 改动 |
|---|---|
| `src/dev/storage/PcieLink.py` | 新增 `write_scale`（写方向串行化比例，默认 1.0）与 `init_cmd_latency`（首请求一次性前导延迟，默认 0ns） |
| `src/dev/storage/pcie_link.hh` | 新增成员 `writeScale` / `initCmdLatency` / `firstReqSeen` |
| `src/dev/storage/pcie_link.cc` | ① 写请求串行化 `×writeScale`（接收侧 + 发送间距侧）；② 首请求加 `initCmdLatency`；③ **修复** `LinkResponsePort::trySendTiming` 把 `sendEvent` 调度到过去时刻的 bug（`std::max(…, curTick())`） |
| `src/python/gem5/components/boards/x86_board.py` | 新增 `simckpt_s2_topology` / `simckpt_num_queues` / `simckpt_num_dram_lanes` / `simckpt_num_cxl_lanes` 构造参数；S2 拓扑：4 条 Gen4 GPU 线 → `NoncoherentXBar` → 共享 Gen5 上行 + 2 条 Gen5 CXL P2P 线；新增 `_simckpt_ruby_dma_ports()` 把上行 + CXL 线的 mem 侧端口接 Ruby |
| `configs/example/gem5_library/x86-cxl-ckptd-e5.py` | 新增 S2 配置（`--gpu-lanes/--cxl-lanes/--write-scale/--cxl-io-latency/--ckpt-mib/--dram-channels/--cxl-channels`） |
| `tests/cxl_tests/simckpt/bench/ckptbench_e5_s2.c` | 新增 S2 benchmark：DRAM-only（4 lane → 上行）vs dual（2 DRAM + 2 CXL） |
| `tests/cxl_tests/simckpt/Makefile` | 新增 `ckptbench_e5_s2` 目标 |
| `tests/cxl_tests/simckpt/analytical_model.py` | 新增 `write_bw(read_bw, write_scale)`（E2 写完成开销） |

### 1.1 S2 拓扑（E5）

```
SimCkptDevice (num_queues=6)
  dma[0..3] -> PcieLink(Gen4 x16) ─┐
                                    ├-> NoncoherentXBar -> PcieLink(Gen5 x16 上行) -> Ruby(DRAM)
  dma[4..5] -> PcieLink(Gen5 x16)  ─┴───────────────(P2P, 不经过上行)──────────────> Ruby(CXL)
```

- 4 条 GPU DRAM 线（Gen4，27.77 GB/s/条）经 `NoncoherentXBar` fan-in 到**一条共享
  Gen5 上行**（55.53）→ 这就是 DRAM-only 的瓶颈。
- 2 条 CXL 线（Gen5 直连，on-device DMA P2P）**不经过上行**，dual 模式下把上行
  腾出来。

### 1.2 E2 三项建模

| 子项 | 建模方式 | 落点 |
|---|---|---|
| CXL P2P 直连 | CXL 线有独立 Gen5 PcieLink、不接共享上行（数据面不占上行） | `x86_board.py` S2 分支 |
| CXL.io 初始命令时间 | `init_cmd_latency`（默认 1us）作为首请求一次性前导延迟 | `PcieLink` + e5 config |
| 写完成开销 | `write_scale`（默认 1.0；=1.5 时写带宽 = 读 / 1.5，贴齐论文 ~1.5× 读写差） | `PcieLink` + e5 config |

> **建模边界（诚实说明）**：「P2P」在本版周期仿真里实现为 *CXL 线不经过共享上行*，
> 即 CXL 流量不占 switch 上行（A2 判据）。真正「绕过 host Ruby 目录、不经
> DMASequencer」的 on-device DMA 通路仍属更深一层扩展，与 v2 §1.5 的说明一致。

---

## 2. 关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| `PcieLink.line_rate_gtps`（GPU 线） | 16（Gen4） | E3 分层：GPU 链路 Gen4 |
| `PcieLink.line_rate_gtps`（上行/expander/CXL） | 32（Gen5） | 上行与 expander 同速 |
| `PcieLink.write_scale` | 1.0（S2 主测）/ 1.55（E2 写完成演示） | 写 = 读 / scale |
| `PcieLink.init_cmd_latency`（CXL 线） | `1us` | CXL.io 一次性前导延迟 |
| `PcieLink.tlp_overhead_bytes` | 35 | 同 v2.1（无 flit） |
| `NoncoherentXBar.width` | 64（512-bit） | **必须够宽**，否则 xbar 成瓶颈（见踩坑 3） |
| `NoncoherentXBar` frontend/forward/response | 1 / 0 / 1 cycles | 纯数据面 fan-in，低开销 |
| `SimCkptDevice.num_queues` | 6（4 DRAM + 2 CXL） | 4 GPU + 2 CXL 线 |
| DRAM / CXL 后端 | 8 子通道 / 8 子通道 | 各 140.8 GB/s，非瓶颈 |
| `--ckpt-mib` | 8 / 32 | 总数据量（4 GPU 平分） |

有效带宽常数（与 v2 一致）：Gen5 x16 = 55.49 GB/s（55.53 标称）、Gen4 x16 =
27.75 GB/s（27.77 标称）；`write_scale=1.55` 时 Gen5 x16 写 ≈ 35.8 GB/s。

---

## 3. 执行方式

```bash
# 1) 编译（C++ 改动 + .py marshalling 都必须重编）
cd <SimCXL>
scons build/X86/gem5.opt -j$(nproc)

# 2) 编译 guest benchmark（静态）
cd tests/cxl_tests/simckpt
make ckptbench_e5_s2

# 3) 注入 disk image
mkdir -p /tmp/img && mount -o loop,offset=$((2048*512)) /root/simcxl-resources/parsec.img /tmp/img
cp ckptbench_e5_s2 /tmp/img/home/test_code/ckptbench_e5_s2
chmod +x /tmp/img/home/test_code/ckptbench_e5_s2 && sync && umount /tmp/img

# 4) 运行 S2（主测，write_scale=1.0）
./build/X86/gem5.opt -d m5out-e5-s2-32 \
  configs/example/gem5_library/x86-cxl-ckptd-e5.py \
  --ckpt-mib 32 --dram-channels 8 --cxl-channels 8

# 5) 运行 E2 写完成开销演示（write_scale=1.55）
./build/X86/gem5.opt -d m5out-e5-ws \
  configs/example/gem5_library/x86-cxl-ckptd-e5.py \
  --ckpt-mib 8 --dram-channels 8 --cxl-channels 8 --write-scale 1.55

# 6) 宿主机分析模型
python3 -c "import sys; sys.path.insert(0,'tests/cxl_tests/simckpt'); \
  import analytical_model as m; \
  print(m.s2_dual_bound(4)); print(m.write_bw(m.PCIE_GEN5_X16,1.55))"
```

结果见各 `m5out-*/board.pc.com_1.device`。`ckptbench_e5_s2` 参数：`--ckpt-mib`
（总 MiB，4 GPU 平分）、`--chunk`（默认 4096）。

---

## 4. 结果

### 4.1 S2：4 GPU 共享上行加速比（write_scale=1.0，PASS，mismatches=0）

| 数据量 | DRAM-only（4 GPU→上行→DRAM） | dual（2 DRAM + 2 CXL P2P） | 加速比 |
|---|---|---|---|
| 8 MiB | 53.75 GB/s | 106.19 GB/s | **1.975×** |
| 32 MiB | 53.77 GB/s | 106.30 GB/s | **1.977×** |

关键结论：

- **DRAM-only ≈ 53.8 GB/s ≈ 上行 Gen5 上限（55.53），与 4×GPU egress（111）无关**：
  4 条 GPU 线（各 26.58 GB/s）把共享上行打满，上行成为硬瓶颈——正是 S2 要验证的
  「多卡挤爆上行」。
- **dual ≈ 106.3 GB/s ≈ 上行 55.53 + CXL P2P 55.53**：CXL 线不占上行，两条 Gen5 线
  可加，聚合带宽越过上行上限。
- **加速比 1.977× ≈ 分析模型 2.0×（误差 1.15%，< 5% 目标）**，直接验证 A1 的 S2
  判据 `B_dual ≈ B_line1 + B_line2` 且显著高于 DRAM-only。

### 4.2 E3：Gen4 GPU 分层

- 每条 GPU 线实测 `own ≈ 26.58 GB/s`（Gen4 x16 有效 27.75 的 ~96%），证明 GPU 链路
  已是 Gen4 分层、与上行/expander 的 Gen5 分离。
- S1（单 GPU 共享 Gen4 link）加速比 1.58× 仍由分析模型给出（`s1_dual_bound()` =
  17.6 → 27.77，1.58×）；周期仿真的单 lane 是「线链路」而非「单 GPU 共享 Gen4
  link」，这一边界与 v2 §1.5 一致。

### 4.3 E2：写完成开销 + CXL.io + P2P

| 项 | 结果 |
|---|---|
| 写完成开销（write_scale=1.55，8 MiB） | DRAM-only **53.75 → 34.87 GB/s（0.649×）**，dual 69.60（加速比仍 1.996×） |
| Gen5 x16 写带宽（分析） | 55.53 / 1.55 = **35.83 GB/s**，实测 34.87（~97%） |
| CXL P2P（不占上行） | dual 106 > 上行 53.8，证明 CXL 线流量不计入上行方向（A2 判据） |
| CXL.io 初始命令时间 | `init_cmd_latency=1us` 一次性前导延迟，不影响稳态带宽（与 target2「计入端到端但不影响稳态」一致） |

写完成开销的加速比**仍为 ~2.0×**：因为写完成开销对称地作用于 DRAM 与 CXL 两条线，
双线可加性不受影响——这是与论文「写比读慢 ~1.5×」相容的合理结果。

### 4.4 回归（E1 2-lane 独立链路，2 MiB，8ch，PASS）

```
single DRAM : 55.43 GB/s
single CXL  : 55.38 GB/s
dual-lane   : 109.23 GB/s
```

与 v2.1 完全一致，确认 `write_scale=1.0` / `init_cmd_latency=0ns` 默认值不改变既有
行为。

---

## 5. 遇到的困难与解决（踩坑）

1. **`num_queues` 读回是 `m5.params.Unsigned` 对象，不是 int**：在
   `_setup_board` 里 `assert n_dram + n_cxl == num_queues` 时，`4+2==6` 却失败。
   原因是 `device.num_queues` 读回的是 gem5 参数包装对象，`==` 比较 int 恒为
   False。解决：直接用 `self._simckpt_num_queues`（构造参数里的纯 int）做比较与
   循环上界。

2. **`_setup_board` 在 `__init__` 里跑，config 里后设 `num_queues` 来不及**：
   `X86Board(...)` 构造期间就执行 `_setup_board`，config 里
   `board.…simckpt_device.num_queues = 6`（构造之后）对已创建的链路数量无效。
   解决：把 `num_queues`/`dram_lanes`/`cxl_lanes` 提升为 board **构造参数**
   `simckpt_num_queues` 等，在 `_setup_board` 一开始就写进 device。

3. **`NoncoherentXBar` 无默认 `width`/延迟，且 width 太小会成瓶颈**：xbar 的
   `width`（字节/周期）限制吞吐，`IOXBar` 的 16B 在 2.4GHz 下只有 ~38 GB/s，低于
   上行 55.53。解决：显式 `width=64`（512-bit ≈ 153 GB/s）+ 低延迟
   （frontend=1/forward=0/response=1），让 xbar 只做 fan-in、不限制带宽。

4. **`PcieLink` 响应端口把 `sendEvent` 调度到过去时刻**：`LinkResponsePort::
   trySendTiming` 原来直接 `schedule(sendEvent, transmitList.front().second)`，
   在 `NoncoherentXBar` 引入的 retry 流量下会命中 `when >= curTick()` 断言
   （`eventq.hh:759`）。解决：改为 `std::max(transmitList.front().second,
   curTick())`，与请求端口已有的 `std::max` 防护对齐。这是原代码的潜在 bug，被
   fan-in retry 路径首次暴露。

5. **`.py` 改动必须重编**（复现 v2 §1.6 第 7 条）：`x86_board.py` 被 marshalling
   进二进制（zlib 压缩 blob），改完不 `scons` 会跑旧拓扑。教训沿用。

---

## 6. 交付与验收对照

| 验收 | v3 结果 |
|---|---|
| A1 双线可加（S2） | 周期仿真 S2 **1.977×**（DRAM-only 53.77 → dual 106.30），与分析 2.0× 误差 1.15% |
| A2 两线不共享上行 | CXL 线走独立 Gen5 link + 不接共享上行；dual 106 > 上行 53.8 |
| A3 多通道后端 | DRAM/CXL 均 8 子通道（140.8 GB/s），非瓶颈 |
| A4 纯 TLP 无 flit | 两线同式（128b/130b + TLP 35B），Gen5 有效 55.43 实测不变 |
| A5 On-device DMA/P2P + CXL.io | CXL 线 P2P（不占上行）；`init_cmd_latency` 建模 CXL.io；写完成开销 `write_scale` 实测 0.649× |
| A6/A7/A8 正确性/统计 | mismatches=0；per-queue issue/done tick 周期精确统计 |

---

## 7. 状态与待办（更新）

- [x] E0 实现 + 验证（v2）
- [x] v2.1 核心改动重测（无 flit + Gen5 line_cxl + 8ch）
- [x] 分析模型更新（无 flit）：S1 1.58×、S2 2.0×
- [x] **S2 完整 4 GPU 周期仿真**（共享上行 NoncoherentXBar，E5）：1.977×
- [x] **E2**（CXL P2P + CXL.io `init_cmd_latency` + 写完成 `write_scale`）/
       **E3**（Gen4 GPU 分层）：均已建模 + 周期仿真验证
- [ ] （可选后续）真正绕过 host Ruby 目录的 on-device DMA 通路（CXL 线不经
  DMASequencer 直连 CXLMemCtrl）；多 CXL expander、N GPU 扫描 + 分析模型回填
