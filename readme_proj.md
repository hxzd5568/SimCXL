# SimCXL 会话总结

本文凭记忆记录本次会话对 SimCXL 项目的理解、涉及文件与踩过的坑。

## 项目介绍

SimCXL 是基于 gem5 的全系统、cycle 级 CXL 模拟器，支持 CXL 三种子协议
（CXL.io / CXL.cache / CXL.mem）与三类设备（Type-1/2/3），含 Classic 与 Ruby
两套内存子系统。

本会话核心场景：用 `PyTrafficGen` 合成流量源**代替真实的 GPU（H100/cuda device）**，
通过 CXL Type-3 内存扩展器做 checkpoint/restore 的双向带宽测试：

- **checkpoint save**：GPU 状态写入 CXL（写方向）
- **restore**：GPU 从 CXL 读回状态（读方向）

CXL 后端内存为 DDR5-4400（2 通道），Classic 配置曾用 HBM 做对比。

## 文件摘要

### 配置脚本（`configs/example/gem5_library/`）
- `x86-cxl-type3-gpu-with-ruby.py`：Ruby 版 GPU→CXL 带宽测试，含 completion 等待修复 + E820 预留区同步。
- `x86-cxl-type3-with-classic.py`：Classic 版，CXL 后端是 HBM。
- `x86-cxl-checkpoint.py`：CXL checkpoint workflow benchmark（多模式：gpu/hbm、dma-copy、memcpy）。
- `x86-cxl-checkpoint-restore.py`：**本会话新增**，一次模拟内先写后读的 save/restore 双向流程。
- `x86-cxl-type3-benchmark.py`：CXL Type-3 基线 benchmark（numactl + lmbench/stream）。

### Ruby 协议（`src/mem/ruby/protocol/`）
- `MESI_Two_Level-dma.sm`：DMA machine 状态机（READY 态 + TBE 表），DMA_READ/DMA_WRITE 发往目录。
- `MESI_Two_Level-dir.sm`：目录，处理 DMA_READ/DMA_WRITE，转 MEMORY_READ/WB 到 CXL 后端。
- `CXL_MESI_Two_Level-*.sm`：CXL 专用协议变体（Type-2 用）。

### Ruby 系统（`src/mem/ruby/system/`）
- `DMASequencer.{hh,cc}`：DMA 请求入口，**本会话改为并行拆分多 cache-line 请求**。
- `RubyPort.{hh,cc}`：DMA 与内存系统的端口/回调（`ruby_hit_callback`、`recvFunctional`）。
- `RubySystem.cc`：Ruby 系统序列化（checkpoint 需先 `memWriteback()`）。

### 其他
- `src/cpu/testers/traffic_gen/base_gen.cc`：TrafficGen 的 blocksize 校验（本会话放宽）。
- `src/cpu/testers/traffic_gen/base.cc`：`createLinear`、`bytesWritten`/`bytesRead` 统计累加。
- `src/dev/x86/cxl_mem_ctrl.cc`：CXL 内存控制器（cxl_rsp_port / mem_req_port，proto_proc_lat）。
- `src/python/gem5/components/boards/x86_board.py`：X86Board，E820 预留 CXL 顶部 64MiB。
- `readme_cxl.md`：本会话产出的测试方法与结论文档。

## 易错点

### 1. 测试数据量太小 → 带宽被固定开销压垮
约 1ms 的固定开销（DMA 流水线冷启动 + 尾部排出）会主导小数据量计时。
4 MiB 时写带宽只有 ~3.7 GB/s，64 MiB 21.6，**512 MiB 才到稳态 28.5 GB/s**。
之前误判「Ruby 比 Classic 慢」「tWR 卡死带宽」，实际都是数据量不足的假象。

### 2. TrafficGen 计时偏早（注入吞吐 ≠ 端到端吞吐）
`LinearGen` 到 data_limit 后立刻切 `ExitGen`，`exitSimLoop()` **不等待在飞写响应**。
naive 的 `copy_bytes/elapsed` 度量的是注入吞吐。修复：`max_outstanding_reqs` +
exit 后轮询 `bytesWritten == copy_bytes`（写）或 `bytesRead`（读），再计时。

### 3. blocksize 受 cache line 限制
`StochasticGen` 原来 `blocksize > cacheLineSize` 直接 fatal，导致 128/256B 请求无法使用。
本会话放宽为 `blocksize % cacheLineSize == 0`。

### 4. `--gpu-copy-mib > 64` 覆盖 Linux 管理的 CXL 内存
`x86_board.py` E820 只固定预留顶部 64MiB，脚本 reserve 却随 copy 大小涨，二者不同步。
需按实际 reserve 覆盖 E820 中 CXL 的 Linux 可见大小。

### 5. `--dma-copy` 不是真复制
两条无数据依赖的并发流（读 DRAM + 写 CXL），读回数据不喂给写，非真正的 DRAM→CXL copy。

### 6. 读比写慢（DRAM 本质，非 bug）
写是 posted（fire-and-forget），可重排/合并/流水线；读是阻塞（要等 tCL 返回），
row miss 的 precharge+activate 代价暴露在关键路径。DDR5 读 ~20.6 GB/s，写 ~28.5 GB/s。

### 7. Ruby checkpoint 需 memWriteback
`RubySystem::serialize` 会因无 cache trace 而 fatal，checkpoint 前必须调 `memWriteback()`。

### 8. 当前模型缺 PCIe 前端
Ruby 路径（PyTrafficGen→DMASequencer→Directory→CXLMemCtrl）语义上 ≈ Home Agent
之后的 64B 内存事务段，**缺 GPU PCIe packetizer + PCIe link + TLP 终止/转换**。
64B 拆分点可近似 Root Complex/CXL Host Bridge 的转换结果，但不能声称模拟了 128/256B TLP。

## 关键结论

- 256B 大包 vs 64B：**写 +12%**（协议层请求数减 4 倍、header 摊薄），**读无收益**
  （并行拆分让 DRAM 总线利用率 45%→59%，但读带宽仍被 DRAM 读延迟卡死）。
- 稳定带宽（Ruby + DDR5，512MiB）：写 ~28.5 GB/s，读 ~20.6 GB/s。
- `DMASequencer` 现在会并行拆一个多 cache-line 请求为多个 64B 子请求（对应
  PCIe MRd 256B → 4×CXL.mem MemRd）。

---

## 2026-11-06 会话更新：CXL 内存预算扫描（target3）进度与问题

> 承接 `target2.md` / `exec_summary_v3.md` 的 E0–E5，本会话推进 `target3.md` 的
> 「CXL 内存预算 → 加速比」扫描。详见 `configs/example/gem5_library/target3.md`。

### 已完成（可编译；周期仿真未跑通，见下方阻塞）

1. **宿主分析模型** `tests/cxl_tests/simckpt/analytical_model.py`
   - 新增 `cxl_budget_channels()` / `cxl_backend_bw()` / `dual_bound_optimal()` /
     `budget_sweep()` + `--budget-sweep`/`--write-scale` CLI。
   - 修复 `s2_dual_bound()` 忽略 `n_cxl_ch` 的隐患（`cxl_bw` 改 `min(link, n_cxl_ch×17.6)`）。
   - 预算→加速比（写 staging，host 8ch 上行限）：**1 GB→1.63×、2/4/8 GB→2.0×**。
2. **配置脚本** `configs/example/gem5_library/x86-cxl-ckptd-e6.py`
   - 新增 `--gpus`(4/8)、`--cxl-lanes`、`--cxl-mem-size`（GB→通道=min(2×GB,8)）、
     `--split-dram-frac`（缺省按理论速率算）、`--probe-cxl`。
3. **benchmark** `tests/cxl_tests/simckpt/bench/ckptbench_e6_budget.c`
   - 泛化到 8 GPU + 可变 CXL lane，整卡粒度最优分流 `k=round(N×f_dram)`。
   - `SIMCKPT_MAX_QUEUES` 8→16（guest 侧；设备侧 `dmaPorts` 用 vector 本无上限）。
4. **关键发现**：`x86_board.py` 的 S2 拓扑本就用 `std::vector` 动态建 lane，**无需改
   board、无需 scons 重编**，`--gpus 8 --cxl-lanes 1` 即可建 9 队列。
5. **资源就位**：按 README 的 Google Drive 链接拉取 `vmlinux`(6.12, 38MB) 与
   `parsec.img`(24GB)，解压后经 `debugfs` 注入 `ckptbench_e6_budget`，最终放
   `/home/work/simcxl-resources/`。

### 关键结论

- 单 CXL expander 下 **4 卡与 8 卡加速比相同**（都 ≈2.0×）：聚合被「上行 55.53 +
  CXL 链路 55.53」卡死，与 GPU egress 无关。
- 8 卡要赢 4 卡需 ≥2 个 expander：`dual_bound_optimal(8, 8, 8, cxl_links=2)`→3.0×、
  `cxl_links=3`→3.54×（CXL 后端 140.8 限）。

### 踩坑 / 阻塞

1. **KVM 不可用（当前最大阻塞）**：容器无 `/dev/kvm`（无 `/proc/kvm`、`open` 报
   `EPERM`）。gem5 全系统必须 KVM 快进 Linux 启动，纯 Timing 启动需数小时不可行。
   需宿主机加载 `kvm_amd/kvm_intel` 模块，且容器 `docker run --device /dev/kvm`
   （或 `--privileged`）重建后才能跑 Phase 3。
2. **无 loop 设备**：容器无 `/dev/loop*`、无法 mount 磁盘镜像，注入 benchmark 改用
   `debugfs` 直接写 ext4 分区 + `dd` 回写（partition offset = 2048×512）。
3. **gem5 运行依赖缺失**：`gem5.opt` 缺 `libtcmalloc_minimal.so.4` 与
   `libprotobuf.so.32`（Ubuntu 24.04 包名带 `t64` 后缀）。旧容器 apt 装过，重启即丢。
4. **`/root` 是临时层**：资源曾放 `/root/simcxl-resources/`（overlay，重启即丢），
   已迁到持久盘 `/home/work/simcxl-resources/`，并把 20 个 config 的 `local_path`
   从 `/root/...` 改为 `/home/work/...`。

### 待办

- [ ] **Phase 3 周期仿真**（有 KVM 后）：4/8 卡 × {1,2,4,8}GB，快速扫描 32MiB +
      512MiB 确认端点，比对分析模型（<5% 误差判据）。
- [ ] **Phase 4 N-expander 扩展**：`dual_bound_optimal(cxl_links=k)` 扫 4/8 卡 ×
      1–3 expander，量化「8 卡何时优于 4 卡」。
