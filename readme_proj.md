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
