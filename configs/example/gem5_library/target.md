可以实现，但不能直接把现有 PyTrafficGen + SimpleMemory disk 拼起来。最快且合理的路线是：
保留 SimCXL 的 CPU/DRAM、CXL Type-3、Ruby DMA 和 Full-System VM；新增一个由 VM 驱动的 64 位 PCIe checkpoint DMA 设备，以及一个多通道存储后端。控制策略、内存池和恢复调度运行在 VM 里的 ckptd，真正的数据搬运、队列竞争和带宽限制由 gem5 设备模型完成。

这样既能让软件真实运行在启动的 Linux VM 中，也能测到 DRAM+CXL、多个存储通道并发带来的带宽增益。
一、SimCXL 现在能用什么，缺什么
当前 develop 分支已经具备：
- 单个 x86 VM。
- 普通 DRAM。
- 一个 CXL Type-3 内存设备，地址从 4 GiB 开始。
- Linux 可将 CXL soft-reserved 内存通过 dax_kmem 转成 NUMA System RAM。
- Ruby DMA 能把大请求拆成多个 cache line 并发执行。
- GPU→CXL、CXL→GPU 的合成流量测试。
- KVM 快速启动，然后切换 Timing/O3 仿真。
但有三个关键缺口。
1. 现有“disk”不是真磁盘
[classic脚本中的disk模型 (line 305)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/configs/example/gem5_library/x86-cxl-type3-with-classic.py:305)只是：
PyTrafficGen → NoncoherentXBar → SimpleMemory
它没有块设备、文件系统、LBA、DMA读取源数据，也不对 VM 暴露。所谓 CXL→disk 实际只是另一个流量发生器向 SimpleMemory 写入新数据，并没有先读取 CXL 数据。
2. 现有 DRAM→CXL 不是数据拷贝
[checkpoint脚本 (line 190)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/configs/example/gem5_library/x86-cxl-checkpoint.py:190)同时启动一个 DRAM read generator 和一个 CXL write generator。仓库文档也明确说明两者[没有数据依赖 (line 112)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/readme_cxl.md:112)。
因此它可以测并发流量，不能验证 checkpoint 内容正确。
3. 当前 IDE 不适合直接 CXL→disk
CXL 内存从 [0x100000000 (line 136)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/src/python/gem5/components/boards/x86_board.py:136) 开始，而 IDE DMA 的 PRD 地址字段只有 [32 位 (line 72)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/src/dev/storage/ide_disk.hh:72)。
因此：
- DRAM→IDE disk 可以是真正 DMA；
- CXL→IDE disk 无法直接表达超过 4 GiB 的地址；
- Linux 很可能要用低端 DRAM bounce buffer；
- 不适合作为你的 CXL 直接存储路径。
仓库也没有现成 NVMe 模型。
二、推荐的目标架构
┌──────────────────────── VM/Linux guest ────────────────────────┐
│                                                                │
│  Training/ckptbench                                            │
│          │                                                     │
│       libckpt                                                   │
│          │                                                     │
│       ckptd                                                     │
│  ┌──────────────┬─────────────┬─────────────┐                  │
│  │分流调度器     │Pinned Pool  │热备/Manifest│                  │
│  │Balancer      │Manager      │Manager      │                  │
│  └──────────────┴─────────────┴─────────────┘                  │
│          │ ioctl + mmap rings                                  │
│      simckpt.ko                                                 │
└──────────┼─────────────────────────────────────────────────────┘
           │ BAR0/doorbell/completion
┌──────────▼────────────── gem5 ─────────────────────────────────┐
│  SimCkptDevice：一个PCI设备，内部多个并发引擎                   │
│                                                                │
│  GPU checkpoint source/HBM-pattern                             │
│       ├─ GPU→DRAM DMA queue ────────────────→ Host DRAM         │
│       └─ GPU→CXL  DMA queue → CXLMemCtrl ───→ CXL Type-3       │
│                                                                │
│  Storage DMA                                                   │
│       ├─ DRAM→Storage                                          │
│       ├─ CXL →Storage                                          │
│       ├─ Storage→DRAM                                          │
│       └─ Storage→CXL                                           │
│                                                                │
│  ParallelStorage                                               │
│       ├─ channel 0：带宽/延迟/队列/数据                         │
│       ├─ channel 1：带宽/延迟/队列/数据                         │
│       └─ channel N                                             │
└────────────────────────────────────────────────────────────────┘
建议第一版把 GPU DMA 和存储 DMA 做成一个多队列 PCI 设备，减少驱动开发量。模型稳定后再拆成 GPU PCI function 和存储 PCI function。
三、模块组成
模块	所在位置	作用
x86-cxl-ai-checkpoint.py	gem5配置	创建 VM、DRAM、CXL、checkpoint PCI设备、多通道存储
SimCkptDevice	gem5 C++	BAR、doorbell、描述符队列、中断、性能计数器
GpuDmaEngine	gem5 C++	模拟 GPU checkpoint payload，并发写DRAM/CXL及读回GPU
MemCopyEngine	gem5 C++	真正执行“read完成后携带数据发write”，替代两个独立TrafficGen
ParallelStorage	gem5 C++	模拟多个闪存通道、带宽、延迟、队列深度、持久数据
simckpt.ko	VM内核模块	pin页面、建立64位DMA映射、提交描述符、处理中断
libckpt	VM用户态库	分块、提交、等待、恢复、CRC校验
ckptd	VM用户态服务	分流、压力监控、pinned pool伸缩、热备管理
Checkpoint Manifest	VM用户态	记录chunk位置、版本、校验和、完成状态
ckptbench	VM测试程序	产生训练checkpoint、故障注入、性能/正确性验证


建议增加的代码目录：
src/dev/storage/SimCkpt.py
src/dev/storage/sim_ckpt_device.hh
src/dev/storage/sim_ckpt_device.cc
src/dev/storage/parallel_storage.hh
src/dev/storage/parallel_storage.cc

configs/example/gem5_library/x86-cxl-ai-checkpoint.py

tests/cxl_tests/simckpt/driver/simckpt.c
tests/cxl_tests/simckpt/include/simckpt_uapi.h
tests/cxl_tests/simckpt/engine/
tests/cxl_tests/simckpt/bench/
四、四条核心数据路径
1. Checkpoint进入DRAM+CXL
GPU data generator
 ├─ chunk 0,2,5... → GPU DMA queue 0 → DRAM pinned pool
 └─ chunk 1,3,4... → GPU DMA queue 1 → CXL pinned pool
每个描述符必须包含：
struct simckpt_desc {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint64_t storage_offset;
    uint32_t length;
    uint32_t checkpoint_id;
    uint32_t chunk_id;
    uint32_t flags;
    uint32_t crc32;
};
GPU模型需要生成真实 payload，不能只统计字节数。可使用：
payload = PRNG(checkpoint_id, chunk_id, byte_offset)
恢复时可以逐字节或CRC验证。
2. DRAM+CXL并发写共享存储
DRAM chunks ──DMA read──┐
                        ├─→ ParallelStorage logical namespace
CXL chunks  ──DMA read──┘
ParallelStorage 对外呈现一个逻辑 checkpoint 空间，内部条带化：
channel = chunk_id % channel_count
channel_offset = chunk_id / channel_count × chunk_size
因此数据可以位于不同物理通道，但恢复时通过 chunk_id + logical_offset 找到正确位置。
第一版建议使用“原始 checkpoint namespace”，不要立即实现完整 POSIX 块设备和文件系统。Manifest 就是这个逻辑文件的索引。后续如果必须让应用看到普通文件，再增加 blk-mq block frontend。
3. CXL热备
磁盘保存完整 checkpoint，CXL只保留最近 checkpoint 中最有价值的部分，例如：
- optimizer状态；
- 模型权重；
- 恢复关键元数据；
- 最近访问或恢复代价最高的tensor。
状态建议：
FREE → PINNED → IN_FLIGHT → DISK_COMMITTED → HOT
                                     └────→ EVICTABLE
CXL热备应该是磁盘副本的缓存，而不是唯一副本。否则 CXL 数据丢失会导致 checkpoint 不完整。
普通 CXL DRAM 只能防训练进程/GPU任务重启，不能保证防 VM 重启、主机断电或 CXL 设备掉电。
4. 并发恢复
热chunk：CXL ─────────────→ GPU restore queue

冷chunk：ParallelStorage
             ├─→ DRAM landing pool ─→ GPU
             └─→ CXL landing pool  ─→ GPU
chunk 可以乱序完成，但不能乱序安装到模型：
- 每个 chunk 带 tensor_id、offset、length、epoch；
- completion bitmap 标记完成；
- CRC正确后才提交；
- tensor所有chunk完成后才能标记 tensor ready；
- 所有必要tensor完成后再恢复训练。
五、分流算法
不要使用固定的50:50比例。每条路径维护：
- 当前排队字节数；
- 当前 outstanding descriptors；
- pool剩余空间；
- 最近完成带宽EWMA；
- P95完成延迟；
- DMA retry/backpressure；
- CXL请求队列满次数；
- 存储通道队列深度。
保存阶段，路径有效速率为：
Rdram = min(GPU→DRAM带宽, DRAM→storage带宽)
Rcxl  = min(GPU→CXL带宽,  CXL→storage带宽)
每来一个 chunk，计算：
predicted_finish_i =
    queued_bytes_i / R_i + chunk_size / R_i
选择预计完成时间更短、并且还有 pool headroom 的路径。
这比周期性硬算比例简单，而且比例会自然形成。例如最近100个chunk中60个去了DRAM、40个去了CXL，那么当前比例就是60:40。
理论保存带宽上限是：
Bsave ≤ min(
    Bgpu→dram + Bgpu→cxl,
    Bdram→store + Bcxl→store,
    Σ Bstorage_channel
)

【目标澄清：优化 GPU 效率，而不是单纯 save 吞吐】

checkpoint 分两个阶段，只有第一阶段暴露给 GPU：
- 阶段1（同步，GPU 可见）：GPU 把状态写入 DRAM/CXL（staging），写完 GPU 立刻回到训练；
- 阶段2（异步，后台）：DRAM/CXL → storage 慢慢落盘，不阻塞 GPU。

因此：
- 首要目标是「最小化 GPU stall time」（= 阶段1 staging 的时间）；「后台尽快持久化」是次要目标；
- 两路内存（DRAM + CXL）的价值在于：GPU 可以并行往两条路 staging，staging 带宽翻倍、GPU 更快返回；同时 DRAM+CXL 组成更大、更快的缓冲，能吸收更大的 burst；
- 即使磁盘只有 7 GB/s（storage 是瓶颈），DRAM+CXL 双路仍然显著降低 GPU stall——它把慢磁盘前面加了一个更大、更快的异步缓冲层，慢落盘被完全隔离到后台。

所以「双内存路径」的验收指标是 staging 带宽（GPU→DRAM/CXL 写入）与 GPU stall time，而不是 storage 落盘吞吐。
六、Pinned Pool控制
建议 VM 使用：
- DRAM：node 0匿名内存；
- CXL：dax_kmem转换后的CXL NUMA node匿名内存；
- mbind()绑定NUMA节点；
- simckpt.ko通过 pin_user_pages() 和 dma_map_sg()注册；
- PCI设备设置64位DMA mask。
MVP不要用 /dev/daxN.Y，因为它的 ZONE_DEVICE 页面、GUP和DMA映射处理更复杂。dax_kmem + NUMA System RAM更适合快速实现。
池管理状态：
NORMAL  ：允许复用，也允许扩大
FROZEN  ：只允许复用，禁止继续pin
SHRINK  ：释放引用计数为0的空闲buffer
STOPPED ：停止新checkpoint，等待已有I/O排空
压力策略可从以下初值开始：
- MemAvailable/MemTotal < 20%：进入 FROZEN；
- 低于10%、memory PSI full持续增加或allocstall快速增长：进入 SHRINK；
- 低于5%：停止新checkpoint并释放非热备空闲池；
- 连续若干采样恢复到25%以上后才重新允许增长，避免振荡。
注意：
- 不能 unpin IN_FLIGHT buffer；
- 只能等待DMA completion、引用计数归零后释放；
- 除系统指标外，必须记录本引擎自己的 pinned_bytes；
- DRAM和CXL节点要分别计算可用空间，不能只看全局 MemAvailable。
目前 CXLMemCtrl 已有请求队列满、重试、队列长度和延迟统计，[代码在这里 (line 46)](C:/Users/32575/Documents/paper/tmp/SimCXL-develop-plan/src/dev/x86/cxl_mem_ctrl.cc:46)，但这些是 gem5 统计，VM 内看不到。因此要把设备自己的：
outstanding
retry_count
queue_full_count
completed_bytes
EWMA bandwidth
average/P95 latency
映射到 PCI BAR 只读寄存器，让 ckptd 可以读取。


七、推荐实施步骤
说明：P0–P2 已完成并验证，P3–P10 为后续阶段。每个阶段给出关键接口、文件位置与验证方法。

P0：建立正确基线（已完成）
1. 从 Ruby 版 checkpoint 配置新建 configs/example/gem5_library/x86-cxl-ai-checkpoint.py（MESI Two Level）。
2. KVM 启动，第一次 m5 exit 后 processor.switch() 切 Timing CPU。
3. CXL 内存经 dax_kmem 全部交给 VM（本仓库内核 E820 type-20 自动 online 为 System RAM node 1），不再为 PyTrafficGen 保留顶部测试区。
4. 修复 classic 脚本 CXL 容量硬编码 1GB HBM、未接入 --cxl-size 的问题；并把 E820 override 的比较改为 int(e.addr)/int(e.range_type)（Addr/UInt64 对象不能直接 == int）。
5. PyTrafficGen 只用于校准 DRAM/CXL 读写带宽，不用于端到端数据正确性。
6. 基线（512MiB，Ruby+DDR5）：GPU→DRAM 写 ~30 GB/s、DRAM→GPU 读 ~29 GB/s、GPU→CXL 写 ~30 GB/s、CXL→GPU 读 ~20 GB/s。

P1：数据搬运引擎（已完成）
1. 新增 SimCkptDevice PCI 设备（src/dev/storage/SimCkpt.py、sim_ckpt_device.{hh,cc}）。
2. submission/completion ring + doorbell + queue depth + 中断（REG_INTR_EN + intrPost()）。
3. MemCopy 语义：DMA read 源内存 → 读响应返回 payload 放进 buffer → CRC32 → DMA write 目标 → 只有写响应到达才生成 completion + 中断。
4. descriptor/completion 都做成 64B cache-line 大小，避免 Ruby DMASequencer 的 cache-line aliasing（并发 DMA 命中同 line 会 clear 整个 request table）。
5. BAR0 用 32-bit（低于 4GB，避开 CXL 4–12GB 窗口）；guest 先写 PCI COMMAND 使能 memory space 才能访问 BAR。
6. SQ/CQ 用绝对索引 + %depth 取址，避免满环后重处理描述符的死循环。
7. 中断线设 0x11（IOAPIC TableSize=24，默认 0x1f 越界触发 assert）。
8. 每 chunk CRC32 写入 completion；测试用 chunk_id 匹配乱序完成。

P2：并行存储（已完成）
1. 新增 ParallelStorage（src/dev/storage/ParallelStorage.py、parallel_storage.{hh,cc}）。
2. 每通道独立：read/write bandwidth、base latency、queue depth、outstanding 限制、byte-addressable backing store。
3. chunk striping：channel = chunk_id % N；channel_offset = (chunk_id/N)*chunk_size + offset%chunk_size。
4. storage 端口用 whole-request（不做 64B cache-line 拆分），让 flash 延迟作用于整页（4KB）而不是 64B。
5. 描述符 flags 区分方向：FLAG_SAVE=内存读→storage 写；FLAG_RESTORE=storage 读→内存写。completion 只在“内存读 + storage 服务”都完成后才生成。
6. 每通道统计 chanBytesRead/chanBytesWritten/chanRejected。
7. 实测（execTicks 统计）：1 通道 ~7.2 GB/s、2 通道 ~11.5 GB/s（证明双通道带宽高于单通道）。

P3：VM 驱动 + 用户态软件
1. 目录 tests/cxl_tests/simckpt/{driver,include,engine,bench}。
2. include/simckpt_uapi.h：冻结 P1 的 simckpt_desc(64B)/simckpt_cpl(64B) ABI + ioctl 命令字（SIMCKPT_REGISTER/UNREGISTER/SUBMIT/WAIT/GET_COUNTERS）。
3. driver/simckpt.ko：64-bit DMA mask；pin_user_pages() + dma_map_sg()；doorbell/中断；把设备 BAR 只读计数器（outstanding/retry/queue_full/completed_bytes/EWMA/P95）映射到 ioctl。
4. engine/libckpt：save/restore 接口（分块、提交、等待、CRC）。
5. ckptd：DRAM(node0)/CXL(node1) 两个 pinned pool（mbind）；Manifest（chunk_meta + generation + bitmap + CRC）。
6. 将 .ko 与程序写入可写 disk image；启动顺序：modprobe dax_kmem → modprobe simckpt → ckptd --config /etc/simckpt.json → m5 exit（切 Timing）→ ckptbench。
7. 验证：驱动 save→restore 往返 CRC/SHA-256 一致；在飞 DMA 完成前禁止 unpin（验收 #7）。

P3 备选路径（若作者不公开 6.12 内核源码）：
simckpt.ko 编译被"启动内核 6.12.0+ 但 disk image 只有 4.15 头文件"卡住（vermagic `6.12.0+ SMP preempt mod_unload modversions`、CONFIG_MODVERSIONS=y、无 IKCONFIG）。主路径是向作者要 6.12 源码树 + .config + Module.symvers（issue 已发）。备选路径是**切换到作者已公开源码的内核 TianheMICALab/linux-5.4.49**（含 .config / cxl.config / release 里的 patch）：
- 改动清单：编译 5.4.49（用其 .config，CONFIG_CXL_MEM=y）→ gem5 配置脚本 KernelResource 路径改 5.4.49 vmlinux → guest 命令去掉 `modprobe dax_kmem`（5.4 无 dax_kmem，CXL 由 CONFIG_CXL_MEM 驱动 + NUMA 修改 online）→ simckpt.ko 把 `pin_user_pages_fast`(5.6+) 换成 `get_user_pages`(5.4) → 用户态 libckpt/ckptd/ckptbench 基本不动。
- 风险：① linux-5.4.49 是给 CXL-DMSim 的，其老 CONFIG_CXL_MEM 驱动与 SimCXL 的 gem5 CXLMemCtrl 模型**接口可能不匹配**，导致 CXL 无法 online 为 node 1；② gem5 x86 FS 的 boot 配置（串口/IDE/virtio）在 5.4 上需补齐；③ 版本回退（5.4 无主线 CXL 子系统、无 dax_kmem）。
- 验证（先花 ~1 小时试探再决定）：clone linux-5.4.49 → 用其 .config 编 vmlinux → 现有 gem5 配置 boot → 看 `numactl -H` 里 CXL 是否仍为 node 1。

P4：动态分流 + 压力控制
1. 每路径维护 queued_bytes/outstanding/pool剩余/EWMA bw/P95/retry/backpressure。
2. 选 lane：predicted_finish_i = queued_bytes_i/R_i + chunk_size/R_i，选预测最短且有 headroom 的 lane。
3. lane_cost 综合 GPU 队列等待 + 传输时间 + 池占用惩罚 + storage backlog 惩罚 + retry 惩罚（见“调度目标”）。
4. 全局 + 每路径 token bucket 防单路径无限积压。
5. pinned pool 状态机 NORMAL/FROZEN/SHRINK/STOPPED（借 CXLMemSim 的 DCD 动态容量思想）。
6. 验证：压力恶化后 pinned_bytes 不增长（验收 #6）。

P5：CXL 热备（hot standby）
1. CXL 只保留最近 checkpoint 中恢复代价最高的 tensor（optimizer state、权重、恢复关键元数据）。
2. CXL 是磁盘副本的缓存，不是唯一副本。
3. chunk 状态 HOT/EVICTABLE（借 CXLMemSim 一致性引擎的 sharer/dirty 思想）。
4. 验证：热备命中率 0/25/50/100% 下恢复时间（验收 #4）。

P6：故障注入
1. 崩溃矩阵：训练进程在 checkpoint 完成后崩溃；checkpoint 写到一半崩溃。
2. 只有 manifest 标记 COMMITTED 的 generation 可恢复，未完成版本回退到前一个 generation。
3. 并发恢复：storage→DRAM 与 storage→CXL 同时恢复。

P7：GPU payload 真实化 + 数据校验
1. GpuDmaEngine 生成可复现 payload：payload[i] = PRNG(checkpoint_id, chunk_id, i)。
2. 每 chunk CRC32 + 全 checkpoint SHA-256（调试模式逐字节比对）。
3. 验证：GPU→DRAM/CXL→storage→DRAM/CXL→GPU 数据 CRC 一致（验收 #1、#5）。

P8：分析模型 + 周期仿真协同（借 LLMServingSim）
1. Python 分析模型：用 P0 基线带宽预测 Bsave ≤ min(B_gpu→dram+B_gpu→cxl, B_dram→store+B_cxl→store, ΣB_channel)，快速扫描拓扑/分流策略。
2. ckptbench 生成参数化 checkpoint trace（frequency/size/chunk_size/hot-cold 分布），仿 LLMServingSim 的 request trace。
3. 周期仿真抽样验证分析模型的关键点。
4. 指标：checkpoint 保存时间、恢复时间（time-to-resume）、保存带宽、热备命中率、每 chunk P95 延迟。

P9：拓扑与一致性增强（借 CXLMemSim）
1. 显式拓扑对象：{GPU, DRAM pool, CXL pool, storage channels}，推导条带化与候选路径。
2. Manifest 版本/代际/状态机完整化（FREE→PINNED→IN_FLIGHT→DISK_COMMITTED→HOT/EVICTABLE）。
3. 多存储通道扩展（N 通道）、多 GPU DMA 队列。

P10：多队列/多主机 + 完整验收
1. 多 GPU DMA 队列；模型稳定后把 GPU DMA 与 storage DMA 拆成独立 PCI function。
2. 多主机 CXL fabric、交换机、跨主机一致性放到后续阶段（当前 X86Board 只有一个 x86 系统 + 一个 CXLMemCtrl）。
3. 跑完整验收矩阵 + 分链路统计（排队/带宽/延迟/重试）+ 论文级报告。
八、第一版的验收标准
第一版不用追求NVMe协议级精度，但应满足：
1. GPU→DRAM/CXL→storage→DRAM/CXL→GPU 数据CRC一致。
2. 双内存路径带宽高于最快单路径（指 GPU→DRAM/CXL 的 staging 写入带宽，用于降低 GPU stall；不是 storage 落盘吞吐）。
3. 两个存储通道带宽高于单通道。
4. CXL热备命中时恢复时间明显下降。
5. chunk乱序完成仍能正确恢复。
6. 压力触发后 pinned pool只复用、不扩大。
7. 所有在飞DMA完成前禁止unpin。
8. 每条链路的排队、带宽、延迟、重试都能分别统计。
如果图中的上下两部分表示两个独立主机/两个VM共同访问CXL和存储，这不属于当前 SimCXL 的快速改造范围。现在的 X86Board只有一个x86系统和一个 CXLMemCtrl。建议第一阶段把它实现成“一个VM、多个GPU DMA队列、多个共享存储通道”；多主机CXL fabric、交换机和跨主机一致性放到后续阶段。


错误检查：
对，但更准确地说：不是一定要对“全部恢复数据做一次总 Hash”，而是先让 GPU 模型生成可重复、可预测的真实数据，然后在恢复后验证这些数据有没有被正确搬回来。
例如生成每个字节：
payload[i] = PRNG(checkpoint_id, chunk_id, i);
只要输入相同，PRNG每次生成的内容就相同。因此恢复后可以采用三种验证方式。
1. 逐字节验证，最严格：
for (i = 0; i < chunk_size; i++)
    assert(restored[i] ==
           PRNG(checkpoint_id, chunk_id, i));
可以精确发现错误位置，但仿真开销最大。
2. 每个chunk做CRC，最推荐：
保存时：
生成payload
→ 计算CRC32
→ payload写入DRAM/CXL/存储
→ CRC写入manifest
恢复时：
恢复chunk
→ 计算CRC32
→ 与manifest中的CRC比较
Manifest例如：
struct chunk_meta {
    uint64_t checkpoint_id;
    uint32_t chunk_id;
    uint64_t logical_offset;
    uint32_t length;
    uint32_t crc32;
    uint8_t  source;       // DISK / CXL_HOT
    uint8_t  state;        // WRITING / COMMITTED
};
这样可以分别验证每个chunk，容易定位是：
- CXL路径出错；
- DRAM路径出错；
- 存储通道出错；
- 恢复顺序或offset出错；
- chunk丢失或重复。
3. 全checkpoint Hash：
所有chunk恢复并按逻辑offset组合完成后，再计算：
SHA-256(restored checkpoint)
和保存时的总Hash比较。
它适合作为最终一致性验证，但只能告诉你“整体错了”，不容易定位哪一个chunk或路径有问题。
建议组合使用：
每个chunk：CRC32
整个checkpoint：SHA-256（可选）
调试模式：错误chunk逐字节比较


调度目标：
调度目标应当改成两个层次
你的引擎主要目标如果是减少训练暂停，调度器首先应该优化：
目标1：最小化GPU exposed pause
目标2：在后台尽快完成持久化
建议增加这些直接指标：
gpu_stage_time
gpu_stall_time
gpu_stall_cycles
buffer_wait_time
durable_completion_time
storage_backlog_bytes
分流时不能只看内存带宽，还要检查池是否即将填满：
lane_cost =
    GPU队列等待时间
  + chunk传输时间
  + 内存池占用惩罚
  + storage backlog惩罚
  + retry/backpressure惩罚
因此：
- CXL带宽高、空闲空间多：增加CXL比例；
- CXL retry或P95升高：减少CXL比例；
- DRAM pool接近上限：转向CXL；
- 两个pool都接近上限：限制GPU发生速率；
- 磁盘积压持续上升：冻结pinned pool扩张，并降低新checkpoint速率。
最终应当分别比较：
单DRAM：GPU停顿时间、最终落盘时间
单CXL ：GPU停顿时间、最终落盘时间
DRAM+CXL：GPU停顿时间、最终落盘时间


九、参考资料
1. 三个参考库（层次对比见正文）
- SimCXL：https://github.com/TianheMICALab/SimCXL —— 全系统、周期级 CXL 模拟器（本仓库 base），支持 CXL.io/cache/mem 与 Type 1/2/3。
- CXLMemSim：https://github.com/SlugLab/CXLMemSim —— "Practical Performance Simulation and Characterization of CXL 3.0 Memory Systems"。借鉴点：拓扑树（Newick）、一致性引擎（owner/sharer/dirty/version）、DCD/GFAM、HDM decoder、QEMU 前端 + server 后端分离。
- LLMServingSim：https://github.com/casys-kaist/LLMServingSim —— "LLMServingSim 2.0: A Unified Simulator for Heterogeneous and Disaggregated LLM Serving Infrastructure"。借鉴点：工作负载级仿真 + 硬件参数化、KV cache 管理/命中率、TTFT/TPOT/吞吐指标、内存放置/offload 策略。

2. 论文（SimCXL 相关）
- Cohet: A CXL-Driven Coherent Heterogeneous Computing Framework with Hardware-Calibrated Full-System Simulation, HPCA 2026. DOI 10.1109/HPCA68181.2026.11408611
- CXL-DMSim: A Full-System CXL Disaggregated Memory Simulator With Comprehensive Silicon Validation, TCAD 2025. DOI 10.1109/TCAD.2025.3607145

3. 规范/文档
- CXL 3.x 规范（CXL.io/CXL.cache/CXL.mem 子协议、Type 1/2/3 设备、DCD）。
- NVMe Base Specification（submission/completion queue、doorbell、MSI-X 中断——SimCkptDevice 的 ring/doorbell 设计参考）。
- gem5 文档：https://www.gem5.org/documentation/（full system、Ruby、MemObject/DmaPort、PCI device 建模）。
- 仓库内 readme_cxl.md（GPU→CXL 带宽方法与结论）。
- 备选内核源码（P3 备选路径，若作者不公开 6.12 源码）：TianheMICALab/linux-5.4.49 —— https://github.com/TianheMICALab/linux-5.4.49（含 .config / cxl.config / release patch，但对应 CXL-DMSim 的 5.4.49，非 6.12）。

4. 本仓库内关键实现文件
- src/dev/storage/sim_ckpt_device.{hh,cc} / SimCkpt.py —— checkpoint DMA 设备（P1）。
- src/dev/storage/parallel_storage.{hh,cc} / ParallelStorage.py —— 多通道存储（P2）。
- src/dev/x86/cxl_mem_ctrl.{hh,cc} —— CXL Type-3 控制器（含 queue_full/retry/延迟统计）。
- src/mem/ruby/system/DMASequencer.{hh,cc} —— Ruby DMA 引擎（多 cache-line 并发拆分）。
- configs/example/gem5_library/x86-cxl-ai-checkpoint.py —— P0 基线脚本。
- configs/example/gem5_library/x86-cxl-simckpt-test.py —— P1/P2 功能测试脚本。
- tests/cxl_tests/simckpt/simckpt_test.cpp —— guest 侧功能测试程序。


十、测试方案（分层 + 分析/周期协同）
总策略：分析模型快速扫策略（借 LLMServingSim），周期仿真做 ground-truth 验证（SimCXL），一致性/拓扑抽象指导 ckptd 与 manifest（借 CXLMemSim）。

L0 单元层（gem5，无 VM）
- ParallelStorage：条带均分（chanBytesWritten）、带宽/延迟/queue depth/backpressure（numRejected）。
- SimCkptDevice：MemCopy 语义（read→carry→write→completion）、CRC、乱序完成（chunk_id 匹配）、SAVE/RESTORE 方向。
- 对照 CXLMemSim 一致性状态机逐条验证 chunk 状态迁移。

L1 集成层（VM 驱动 round-trip）
- simckpt.ko + libckpt 的 save→restore，CRC/SHA-256 一致（验收 #1）。
- 64 位 DMA 映射、doorbell/中断、pin/unpin 生命周期（在飞禁止 unpin，验收 #7）。

L2 系统层（分流 + 压力）
- 分析模型先行（借 LLMServingSim），预测拓扑/策略的 Bsave 上界；周期仿真抽样验证。
- 压力注入：MemAvailable <20% 进 FROZEN、<10% 进 SHRINK、<5% STOP；验证 pinned_bytes 不再增长（验收 #6）。
- 双内存路径带宽 vs 最快单路径（验收 #2）；双通道 vs 单通道（验收 #3，已测 11.45 vs 7.23 GB/s）。

L3 工作负载层（ckptbench 故障注入 + 热备）
- 参数化 trace 生成不同 frequency/size/hot-cold 分布。
- 热备命中率 0/25/50/100% 恢复时间（验收 #4）。
- 崩溃矩阵 + generation 回退；1 vs 2 存储通道；仅 DRAM/仅 CXL/DRAM+CXL；并发恢复。

验收标准映射（八条）：#1→L1/L3，#2→L2，#3→L0，#4→L3，#5→L0/L1，#6→L2，#7→L1，#8→L0（分链路统计）。