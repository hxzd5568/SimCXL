# target3.md —— CXL memory 预算扫描：动态数据量 / 动态 CXL 容量 / 最优分流 / 加速比

> 本文是 `target2.md` 的续篇。前提：先读 `target2.md`（双线 staging 拓扑与 E0–E5）、
> `exec_summary_v3.md`（S2 4-GPU 共享上行 + E2/E3 已交付）。
>
> 一句话目标：把 S2 的「固定 4 GPU + 固定 8 GB CXL」推广成一个**可扫描的预算空间**，
> 在每个 CXL memory 预算下①动态调测试数据量、②按预算动态调 CXL 后端通道数（后端带宽
> 随之变化）、③按理论速率做最优负载分流、④测出**相对 host-DRAM 单路径的加速比**，
> 得到「CXL 预算 → 加速比」的完整曲线。负载方向为**写 staging**，主场景 **4 卡同时写 /
> 8 卡同时写**。

---

## 一、目标澄清

与 target2 的关系：target2 已经交付了「两条独立 PCIe 线可加」的机理（E0–E5），S2 在
固定 8 GB CXL 下加速比 ≈2.0×。target3 不推翻任何机理，只把 **CXL 后端容量当作自变量**
（`CXL 预算 → 后端带宽 → CXL 线速率 → 最优分流 → 加速比`），从而回答：

> 「给 CXL 只买 1 GB 和买 8 GB，对 4 卡 / 8 卡同时写的 staging 加速分别差多少？钱花在哪
> 里边际收益归零？」

负载固定为**写**（GPU→内存 staging，即 target2 阶段 1）；读/落盘（阶段 2）不进关键路径，
不在本计划范围。

---

## 二、理论模型（宿主分析模型，已实现）

### 2.1 公式

```
B_cxl_line  = min(CXL Gen5 x16 链路, 预算后端)      # 预算后端 = n_ch × 17.6 GB/s
B_dram_line = min(上行 Gen5 x16, host 后端)         # host 8 ch → 55.53 GB/s
DRAM-only(基线) = min(N × 27.77, B_dram_line)       # 所有 N 卡走共享上行
dual(最优)      = min(N × 27.77, B_dram_line + B_cxl_line)
speedup         = dual / DRAM-only
最优分流比      = B_dram_line : B_cxl_line           # 两线同时完成（水填充）
```

常量（沿用 target2，无 flit，效率 ≈86.8%）：Gen5 x16 = 55.53 GB/s、Gen4 x16 =
27.77 GB/s、DDR5 32-bit 子通道 = 17.6 GB/s。

CXL 预算 → 通道映射（本次约定）：`n_ch = min(2 × 预算GB, 8)`，即 1/2/4/8 GB →
2/4/8/8 子通道 → 后端 35.2/70.4/140.8/140.8 GB/s。

### 2.2 预算扫描结果（write_scale=1.0，host=8 ch）

| CXL 预算 | 通道 | CXL 线速率 | 4GPU dual | 4GPU 加速比 | 8GPU 加速比 | 最优分流 DRAM:CXL |
|---|---|---|---|---|---|---|
| 1 GB | 2 | 35.20 | 90.73 | **1.63×** | 1.63× | 61 : 39 |
| 2 GB | 4 | 55.53 | 111.06 | **2.00×** | 2.00× | 50 : 50 |
| 4 GB | 8 | 55.53 | 111.06 | **2.00×** | 2.00× | 50 : 50 |
| 8 GB | 8 | 55.53 | 111.06 | **2.00×** | 2.00× | 50 : 50 |

### 2.3 三个关键结论

1. **加速比在 2 GB 饱和**：CXL 后端一旦 ≥ CXL Gen5 链路（55.53），加速比封顶 2.0×；
   低于 2 GB 时 CXL 后端是瓶颈（1 GB → 1.63×）。即「CXL 买到 2 GB 就够喂满一条 Gen5
   link」，再多是浪费（对单 expander 而言）。
2. **单 expander 下 4 卡与 8 卡加速比完全相同**（都 111 GB/s）：聚合带宽被
   「上行 55.53 + CXL 链路 55.53」卡死，不是被 GPU egress 卡死。4 卡 egress 111 恰好
   打满两线，8 卡的 222 egress 只能排队。
3. **8 卡要赢 4 卡，必须加 expander**（每个 expander = 一条 Gen5 x16 = 55.53）：

   | 配置 | 1 expander | 2 expander | 3 expander |
   |---|---|---|---|
   | 4 GPU | 2.00× | 2.00×（egress 上限 111） | 2.00× |
   | 8 GPU | 2.00× | **3.00×**（166.6 GB/s） | **3.54×**（196.3 GB/s，后端 140.8 限） |

   4 卡无论加多少 expander 都停在 2.0×（GPU egress 上限）；8 卡 2 expander → 3.0×，
   3 expander → 3.54×（此时 CXL 后端 140.8 成为新瓶颈）。

---

## 三、开发计划（Phase 0–4）

### Phase 0 — 宿主分析模型 ✅（本次已交付）

- `tests/cxl_tests/simckpt/analytical_model.py` 新增：
  - `cxl_budget_channels()` / `cxl_backend_bw()`：预算 GB → 通道数 → 后端带宽。
  - `dual_bound_optimal(n_gpu, n_dram_ch, cxl_gb, write_scale, cxl_links)`：最优双路径
    上界 + 最优分流比 + 加速比。
  - `budget_sweep()` + `--budget-sweep` / `--write-scale` CLI。
- 顺带修复 `s2_dual_bound()` 中 `cxl_bw = CXL_BACKEND` 忽略 `n_cxl_ch` 的隐患
  （改为 `min(link, n_cxl_ch×17.6)`，4-GPU 默认结果不变）。
- 运行：`python3 analytical_model.py --budget-sweep`，输出 §2.2/§2.3 表格。

### Phase 1 — gem5 config/board 改动（需 scons）

1. `configs/example/gem5_library/x86-cxl-ckptd-e5.py`（或新建 `-e6.py`）：
   - 新增 `--cxl-mem-size`（GB），与 `--cxl-channels` 绑定写入
     `ChanneledMemory(DDR5_4400_4x8, n_ch, 64, size=...)`；预算→通道用 §2.1 的映射。
   - `--gpu-lanes` 支持 8；`--cxl-lanes` 可变。
2. `src/python/gem5/components/boards/x86_board.py`：
   - S2 分支 `n_dram/n_cxl` 的默认 `4/2` 泛化到 `simckpt_num_dram_lanes=8`、
     `simckpt_num_cxl_lanes` 可变；8 卡 = 8 条 Gen4 GPU 线 fan-in 到 1 条共享上行。
   - CXL 后端 `ChanneledMemory` 通道数 = 预算通道数（「预算→速率」的周期仿真落点）。

### Phase 2 — benchmark 改动（gcc 可编，gem5 内运行）

- 新 `tests/cxl_tests/simckpt/bench/ckptbench_e6_budget.c`：
  - 泛化 `MAX_GPUS=8`、CXL lane 数可变；按命令行传入的最优分流比（或按 per-lane 实测
    速率自算）路由 chunk。
  - 量两档：DRAM-only（基线）与 dual（最优分流），输出加速比。
  - **只分配各 lane 实际用到的缓冲**——现有 `ckptbench_e5_s2.c` 把 `dram[8]+cxl[8]` 双份
    全量分配，预算小（1 GB）时 CXL 侧会 OOM。

### Phase 3 — 周期仿真验证（两级数据）

- **快速扫描**：4/8 卡 × {1,2,4,8} GB，每点总数据 16–32 MiB，DRAM-only + dual 各跑一次，
  比对 Phase 0 模型（沿用 exec_summary_v3 的「模型 vs 实测 <5%」判据），出完整曲线。
- **全量确认**（§5.3）：S1 512 MiB；S2 4 卡在 CXL 1/8 GB 两端点跑 512 MiB；S2 8 卡在
  8 GB 跑 512 MiB。确认大数据的稳态带宽与扫描点、模型三者一致（小数据没有低估）。

### Phase 4 — 宿主全量 + N-expander 扩展

- `balancer_scale.c` / `topology_scale.c` 全量（512 MiB–4 GiB，毫秒级）验证最优分流
  的可加性上界。
- `dual_bound_optimal(cxl_links=k)` 扫 4/8 卡 × 1–3 expander，出「8 卡何时优于 4 卡」
  完整曲线（§2.3 表 2）。

---

## 四、可编译性清单

| 部件 | 编译方式 | 备注 |
|---|---|---|
| `analytical_model.py` | 无需编译 | ✅ 本次已改，秒级 |
| `balancer.c/h`、`topology.c/h`、`*_scale.c` | `make`（gcc） | 宿主跑，无需 gem5 |
| `bench/ckptbench_*.c` | `make`（gcc 静态） | 编译无需 gem5，运行需 gem5 guest |
| `src/dev/storage/*`（PcieLink/SimCkpt） | `scons build/X86/gem5.opt` | C++ 改动必须重编 |
| `src/python/.../x86_board.py` | `scons` | .py 被 marshalling 进二进制，改完必须重编 |
| `configs/.../*.py` | 不编译 | 由 `gem5.opt` 直接解释（但引用的 board 在二进制里） |

---

## 五、数据量与仿真规模（两级数据：快速扫描 + 全量确认）

### 5.1 三个独立的约束，不要混

| 约束 | 值 | 说明 |
|---|---|---|
| **周期仿真（快速扫描）** | 每点总数据 **16–32 MiB**（4 卡每卡 4–8 / 8 卡每卡 2–4） | 覆盖全部预算点 × 卡数，快 |
| **周期仿真（全量确认）** | 代表性端点跑 **512 MiB** | 见 §5.3，仅 2–4 个端点，慢 |
| **宿主模型/scale 数据量** | 无上限（512 MiB–4 GiB） | 纯 C/Python，毫秒级 |
| **guest 内存硬上限** | host ≤ 3 GiB（可用 ~2 GiB）；CXL = 预算 − 64 MiB | 8 卡每卡 ≤ 256 MiB（DRAM 绑）；4 卡每卡 ≤ 512 MiB |

### 5.2 为什么新拓扑 16–32 MiB 就到稳态（而非 readme 的 512 MiB）

`readme_cxl.md` 与 `target2.md` §7 里「512 MiB 才能到稳态」的结论来自**旧 PyTrafficGen
Ruby DMA 路径**（`x86-cxl-type3-gpu-with-ruby.py`）：那条路径有 ~1 ms 的固定开销（DMA
流水线冷启动 + 尾排），小数据量会被固定开销主导，才需要 512 MiB 摊薄。

**target2 的 SimCkptDevice + PcieLink 拓扑不一样**：PcieLink 是纯带宽限模型（按
tick/byte 直算，无 ~1 ms 冷启动主导），`exec_summary_v3.md` 实测 S2 用 **8 MiB 与 32 MiB**
就分别得到 53.77 / 106.3 GB/s（≈理论 55.53 / 111 的 97%），**8 MiB 就已到稳态**。

所以 16–32 MiB 用于「扫曲线形状」是足够且高效的——每个预算点几十秒墙钟，全网格跑得完。

### 5.3 512 MiB 作为「全量确认端点」纳入计划

为满足对稳态带宽的高置信要求，把 512 MiB 作为**确认性端点**纳入周期仿真，但不铺满全
网格（否则 8 预算点 × 4/8 卡 × DRAM-only/dual 就是几天墙钟）。约定：

- **S1（单 GPU）**：512 MiB 单卡可直接周期仿真（`m5out-e4-s1-256` 已跑过 256 MiB），
  作为 S1 稳态带宽的最终确认。
- **S2（4 GPU）**：512 MiB 总数据 = 每卡 128 MiB，在**两个代表性端点**（CXL 1 GB 与
  8 GB）跑一次 DRAM-only + dual，确认与 16–32 MiB 扫描点、与模型三者一致。
- **S2（8 GPU）**：512 MiB 总数据 = 每卡 64 MiB，只跑 1 个端点（8 GB）做存在性确认；
  全网格仍用 16–32 MiB。

墙钟成本（诚实标注）：KVM→Timing 切换后 benchmark 全部代码（`mmap/mbind` + 逐页 touch、
`/proc/self/pagemap` 批量读、每个 chunk 填一个 descriptor）都在**慢速 Timing CPU** 上跑，
开销 O(chunk 数)。512 MiB = 131072 chunk，guest 侧 setup 需几分钟到几十分钟/run；这也是
为什么 512 MiB 只放在少数端点，而非全网格。数据量从 64 MiB 提到 512 MiB，带宽结论只从
「理论的 ~97%」抬到「~99.5%」，收益是**置信度**而非量级——它验证「小数据扫描没低估
稳态带宽」，本身不改变加速比结论。

---

## 六、待确认 / 扩展

- [ ] Phase 1–3 的 gem5 C++/board 改动需 `scons` 重编（耗时），是否现在继续实现。
- [ ] 8 卡需要 8 条 DRAM lane + 可变 CXL lane 的 S2 泛化，`NoncoherentXBar` 宽度
      （当前 512-bit）需确认 8 条 Gen4 线 fan-in 仍不成为瓶颈。
- [ ] 多 expander（`cxl_links=k`）作为可选扩展，用于量化「8 卡何时优于 4 卡」。

## 七、参考资料

- 仓库内：`target2.md`（拓扑与 E0–E5）、`exec_summary_v3.md`（S2 4-GPU 实测 1.977×）、
  `readme_cxl.md`（旧 Ruby DMA 路径的 512 MiB 结论来源）、
  `tests/cxl_tests/simckpt/analytical_model.py`（Phase 0 已实现）。
