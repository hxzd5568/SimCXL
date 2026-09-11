# GPU→CXL 带宽测试：方法与结论

本文记录 GPU→CXL（Type-3 内存扩展器）带宽测试的正确方法，以及一次关键的方法论修正的结论。

## 测试脚本

| 脚本 | 子系统 | CXL 后端介质 |
|---|---|---|
| `configs/example/gem5_library/x86-cxl-type3-with-classic.py` | Classic | HBM (`HBM_1000_4H_1x128`, 8 通道) |
| `configs/example/gem5_library/x86-cxl-type3-gpu-with-ruby.py` | Ruby (MESI Two Level) | DDR5 (`DIMM_DDR5_4400`, 2 通道) |

两个脚本都用 `PyTrafficGen` 作为合成 GPU DMA 流量源，路径分别是：

- Classic：`GPU DMA → iobus → CXLBridge/CXLMemCtrl → CXLMemBar → CXL DRAM`
- Ruby：`GPU DMA → iobus → Ruby DMA machine → dir → CXLMemCtrl → CXLMemBar → CXL DRAM`

## 计时方法（重要修正）

`PyTrafficGen` 的 `LinearGen` 在达到 `data_limit` 后立即切换到 `ExitGen`，而 `ExitGen`
直接调用 `exitSimLoop()`，**不会等待在飞写请求的响应返回**。因此 naive 的
`copy_bytes / elapsed` 度量的是「注入吞吐」，不是「最后一字节落库并收到完成响应的
端到端吞吐」。

`x86-cxl-type3-gpu-with-ruby.py` 已修正为：

1. `PyTrafficGen(max_outstanding_reqs=...)` 限制在飞请求数；
2. `handle_exit` 在 `ExitGen` 退出后用 `scheduleTickExitFromCurrent` 反复轮询，
   直到 `bytesWritten == copy_bytes` 才计时（`bytesWritten` 在 `recvTimingResp`
   里累加，代表真正落库完成）；
3. 计时前 `assert bytesWritten == copy_bytes`，确保没有未返回的响应。

## 关键结论：测试数据量必须足够大

带宽测试中，小数据量会被约 1ms 的固定开销（DMA 流水线冷启动 + 尾部排出）主导，
严重低估带宽。Ruby 配置写带宽随 copy 大小变化：

| copy 大小 | Ruby 写带宽 |
|---|---|
| 1 MiB | 1.02 GB/s |
| 2 MiB | 1.97 GB/s |
| 4 MiB | 3.68 GB/s |
| 8 MiB | 6.64 GB/s |
| 16 MiB | 10.99 GB/s |
| 64 MiB | 21.62 GB/s |
| **512 MiB** | **28.47 GB/s** |

Classic 配置同样受影响（4 MiB 仅 4.96 GB/s）。64 MiB 仍受约 0.86ms 固定开销影响
（稳态带宽约 28.5 GB/s，64 MiB 只有 21.62）。**推荐固定使用
`--gpu-copy-mib 512` 及以上的数据量进行带宽实验。**

数据量足够大后，Ruby 配置（DDR5 后端）与 Classic 配置（HBM 后端）都能达到约
28 GB/s 量级的带宽，二者并无「Ruby 显著更慢」的差异。之前的 3.6~3.7 GB/s 全部
来自 4 MiB 小数据量下的固定开销，而非 Ruby 路径本身的问题。

512 MiB 下稳态带宽（Ruby，DDR5 后端）：

| 操作 | 带宽 |
|---|---|
| write | 28.47 GB/s |
| read | 20.75 GB/s |

## 256B 请求 vs 64B 请求

在 64 MiB 数据量下对比 Ruby 的 block/request 大小：

| 配置 | 写带宽 |
|---|---|
| 64B block + 64B request | 21.62 GB/s |
| 256B block + 256B request | 21.62 GB/s |

**256B 相对 64B 没有带宽收益。** 原因是 DRAM 提交速率由列到列延迟 `tCCD_L`
（DDR5-4400 为 5ns，对应 64B/5ns × 2ch ≈ 25.6 GB/s 上限）决定，与请求粒度无关：
无论 64B 还是 256B 请求，最终都拆成同样多的 64B DRAM burst 提交。256B 只节省
协议/头部开销，而头部开销不是 DRAM 不是瓶颈时的带宽限制因素。

## 复现命令

```bash
# 构建（Type-3 内存扩展器，Ruby 协议）
scons build/X86/gem5.opt -j`nproc`

# Ruby 配置，512 MiB 写带宽
./build/X86/gem5.opt \
    configs/example/gem5_library/x86-cxl-type3-gpu-with-ruby.py \
    --gpu-copy-mib 512 --gpu-op write

# Ruby 配置，512 MiB 读带宽
./build/X86/gem5.opt \
    configs/example/gem5_library/x86-cxl-type3-gpu-with-ruby.py \
    --gpu-copy-mib 512 --gpu-op read
```

脚本已自动按 `--gpu-copy-mib` 扩大 CXL 顶部预留区，并同步修改 E820 中 Linux 可见
的 CXL 大小（`cxl_size - reserve_size`），避免测试区与 Linux 管理的 CXL 内存冲突。

## 尚未解决的建模边界

当前模型适合系统级吞吐趋势，不构成精确的 PCIe/CXL 链路级模型。未建模内容包括：
PCIe 速率与 lane 数、TLP/CXL FLIT 头部开销、credit flow control、GPU MPS/MRRS、
IOMMU/ATS、GPU DMA descriptor 与 copy-engine 调度。

- `--gpu-request-size 128/256` 受 `StochasticGen` 的 `blocksize <= cacheLineSize`
  限制（cache line 为 64B），当前不能直接使用；GPU 的 128/256B burst 应建模为
  连续多个 64B cache-line 请求，而非单个 TrafficGen packet。
- `--dma-copy` 模式同时启动两条无数据依赖的并发流量（读 DRAM + 写 CXL），并非
  真正的 DRAM→CXL copy。
