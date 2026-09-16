# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""

This script shows an example of running a CXL type 3 memory expander simulation
using the gem5 library with the Classic memory system. It defaults to simulating
a CXL ASIC Device.
This simulation boots Ubuntu 18.04 using KVM CPU cores (switching from Atomic/KVM).
The simulation then switches to TIMING/O3 CPU core to run the benchmark.

Usage
-----

```
scons build/X86/gem5.opt -j21
./build/X86/gem5.opt configs/example/gem5_library/x86-cxl-run.py
```
"""
import argparse
import m5
from gem5.utils.requires import requires
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import DIMM_DDR5_4400, SingleChannelDDR4_3200
from gem5.components.memory.dram_interfaces.hbm import HBM_1000_4H_1x128
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource
from m5.objects import Addr, AddrRange, NoncoherentXBar, PyTrafficGen, SimpleMemory, X86E820Entry
from m5.ticks import fromSeconds
from m5.util.convert import toMemoryBandwidth, toMemorySize

# Check ensures the gem5 binary is compiled to X86.
requires(isa_required=ISA.X86)

# Argument Parsing
parser = argparse.ArgumentParser(description='CXL system parameters.')
parser.add_argument('--is_asic', action='store', type=str, nargs='?', 
                    choices=['True', 'False'], default='True', 
                    help='Choose to simulate CXL ASIC Device or FPGA Device.')
test_choices = [
    'lmbench_cxl.sh', 'lmbench_dram.sh', 
    'merci_dram.sh', 'merci_cxl.sh', 'merci_dram+cxl.sh',
    'stream_dram.sh', 'stream_cxl.sh'
]
parser.add_argument('--test_cmd', type=str, choices=test_choices, 
                    default='lmbench_cxl.sh', help='Choose a test to run.')

parser.add_argument('--num_cpus', type=int, default=1, help='Number of CPUs')
parser.add_argument('--cpu_type', type=str, choices=['TIMING', 'O3'], 
                    default='TIMING', help='CPU type')
parser.add_argument('--cxl_mem_type', type=str, choices=['Simple', 'DRAM'], 
                    default='DRAM', help='CXL memory type')
parser.add_argument(
    "--gpu-direction",
    choices=["gpu-to-cxl", "cxl-to-gpu"],
    default="gpu-to-cxl",
)

parser.add_argument(
    "--gpu-target",
    choices=["cxl", "ddr"],
    default="cxl",
    help="Write/read target: CXL expander memory or main DDR",
)

parser.add_argument(
    "--gpu-copy-mib",
    type=int,
    default=16,
)

parser.add_argument(
    "--gpu-offered-bw",
    type=str,
    default="32GB/s",
)

parser.add_argument(
    "--gpu-request-size",
    type=int,
    choices=[64, 128, 256, 4096],
    default=64,
)

parser.add_argument(
    "--disk-bw",
    type=str,
    default="7GB/s",
    help="Disk write bandwidth (NVMe ~7GB/s, SATA SSD ~0.55GB/s, HDD ~0.2GB/s)",
)

parser.add_argument(
    "--disk-latency",
    type=str,
    default="10us",
    help="Disk access latency",
)

parser.add_argument(
    "--disk-size",
    type=str,
    default=None,
    help="Disk (SimpleMemory) size. Defaults to gpu-copy-mib.",
)

parser.add_argument(
    "--cxl-size",
    type=str,
    default="1GB",
    help="CXL Type-3 memory size (8-channel HBM_1000_4H_1x128 is 1GiB)",
)

parser.add_argument(
    "--cxl-test-reserve",
    type=str,
    default=None,
    help="Reserved region at top of CXL for the test. Defaults to gpu-copy-mib.",
)

parser.add_argument(
    "--cxl-queue-size",
    type=int,
    default=256,
    help="CXL bridge/controller queue depth. Higher => higher CXL bandwidth "
         "(Little's Law: BW ~= depth*64B/latency). 48 => ~5GB/s, 256 => ~30GB/s.",
)

parser.add_argument(
    "--cxl-bridge-lat",
    type=str,
    default="50ns",
    help="CXLBridge bridge latency (per packet). Lower => higher CXL bandwidth.",
)

parser.add_argument(
    "--cxl-proc-lat",
    type=str,
    default="12ns",
    help="CXLBridge/CXLMemCtrl protocol processing latency (per packet).",
)


args = parser.parse_args()

# 让预留区/磁盘大小默认跟随拷贝数据量，避免大块测试时忘记手动加大。
# 至少 64MiB：与 x86_board.py 里硬编码的预留一致，避免小数据时改变 CXL 地址
# 布局（否则会触发 CoherentXBar snoop filter 的断言）。
if args.cxl_test_reserve is None:
    args.cxl_test_reserve = f"{max(64, args.gpu_copy_mib)}MiB"
if args.disk_size is None:
    args.disk_size = f"{max(64, args.gpu_copy_mib)}MiB"

# Setup Classic MESI Three Level Cache Hierarchy
cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)

# Setup system memory and CXL memory
memory = DIMM_DDR5_4400(size="3GB")
# CXL 背板内存换成 HBM（128bit 宽通道），验证写带宽能否摆脱 DDR5-4400 的 ~5GB/s
# 容量由 --cxl-size 控制（8 通道 HBM_1000_4H_1x128 物理容量为 1GiB）。
cxl_dram = ChanneledMemory(HBM_1000_4H_1x128, 8, 64, size=args.cxl_size)

# Setup Processor
# Using KVM for fast boot, then switching to Timing/O3
# If KVM cannot be used, you can boot from ATOMIC.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.O3 if args.cpu_type == 'O3' else CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=args.num_cpus,
)

# Here we tell the KVM CPU (the starting CPU) not to use perf.
for proc in processor.start:
    proc.core.usePerf = False

# Here we setup the board and CXL device memory size. The X86Board allows for Full-System X86 simulations.
board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=(args.is_asic == 'True'),
)

# 加深 CXL 链路队列，让 GPU<->CXL 带宽更贴近真实 PCIe/CXL。
# 默认 x86_board.py 里 CXLBridge req/resp_fifo_depth=128、CXLMemCtrl req/rsp_size=48，
# 配合 ~几百 ns 延迟只能撑 ~5GB/s；加深到 256 可逼近 ~30GB/s。
board.bridge.req_fifo_depth = args.cxl_queue_size
board.bridge.resp_fifo_depth = args.cxl_queue_size
board.pc.south_bridge.cxl_device.req_size = args.cxl_queue_size
board.pc.south_bridge.cxl_device.rsp_size = args.cxl_queue_size
board.bridge.bridge_lat = args.cxl_bridge_lat
board.bridge.proto_proc_lat = args.cxl_proc_lat
board.pc.south_bridge.cxl_device.proto_proc_lat = args.cxl_proc_lat

# CXL 内存的 DDR 控制器写缓冲深度（默认 DDR5-4400 是 64）
for mc in cxl_dram.get_memory_controllers():
    mc.dram.write_buffer_size = args.cxl_queue_size
# GPU DMA synthetic request source
# GPU 连接方式取决于目标：
#  - cxl：作为 CXL.mem 非一致性请求者，接 iobus（绕过一致性 membus snoop）
#  - ddr：接 membus（一致），直接访问主存 DDR，用于对比 DDR 写带宽
board.gpu_dma = PyTrafficGen(progress_check="1000000s")
if args.gpu_target == "cxl":
    board.gpu_dma.port = board.iobus.cpu_side_ports
else:
    board.gpu_dma.port = cache_hierarchy.get_cpu_side_port()

m5.ticks.fixGlobalFrequency()

copy_bytes = args.gpu_copy_mib * 1024 * 1024
block_size = args.gpu_request_size

# x86_board.py中Type-3地址从4GiB开始
CXL_BASE = 0x100000000

# 预留 CXL 顶部一段区间给测试，不让 Linux 使用
reserve_size = toMemorySize(args.cxl_test_reserve)
cxl_size = cxl_dram.get_size()

if copy_bytes > reserve_size:
    raise ValueError(
        f"gpu-copy-mib ({args.gpu_copy_mib} MiB) must not exceed the "
        f"reserved test range ({args.cxl_test_reserve})"
    )
if reserve_size >= cxl_size:
    raise ValueError(
        f"cxl-test-reserve ({args.cxl_test_reserve}) must be smaller than "
        f"cxl-size ({args.cxl_size}), otherwise Linux sees no CXL memory"
    )

cxl_test_start = CXL_BASE + cxl_size - reserve_size
cxl_test_end = cxl_test_start + copy_bytes

# GPU 读写目标地址：默认 CXL 顶部预留区；--gpu-target ddr 则写主存 DDR 高地址
if args.gpu_target == "ddr":
    gpu_test_start = 0x80000000  # 2GB，主存 DDR 的高地址区间
    gpu_test_end = gpu_test_start + copy_bytes
else:
    gpu_test_start = cxl_test_start
    gpu_test_end = cxl_test_end

# x86_board.py 里 CXL 的 E820 默认只预留 64MiB，这里按 --cxl-test-reserve
# 覆盖成实际预留大小，让 Linux 只看到 cxl_size - reserve_size，顶部留给测试。
cxl_linux_visible = cxl_size - reserve_size
for e in board.workload.e820_table.entries:
    if int(e.addr) == CXL_BASE and int(e.range_type) == 20:
        e.size = f"{cxl_linux_visible}B"
        break

offered_rate = toMemoryBandwidth(args.gpu_offered_bw)

# 每发送一个block的时间间隔
period = max(
    1,
    fromSeconds(block_size / offered_rate),
)

# gpu-to-cxl：GPU DMA写Type-3内存
# cxl-to-gpu：GPU DMA读Type-3内存
read_percent = (
    0 if args.gpu_direction == "gpu-to-cxl" else 100
)

# ---- 磁盘（持久化后端）建模 ----
# checkpoint 最终要落到持久化存储。真实磁盘写带宽远低于内存/CXL
# (NVMe ~7GB/s, SATA SSD ~0.55GB/s, HDD ~0.2GB/s)，用 SimpleMemory 的
# bandwidth 参数来模拟这个写速率上限，作为 CXL->disk 拷贝的瓶颈。
DISK_BASE = 0x4000000000  # 256 GiB，远离 DDR(0-3G)/CXL(4-12G) 区间

disk_size = toMemorySize(args.disk_size)
if disk_size < copy_bytes:
    raise ValueError("disk-size must be >= gpu-copy-mib")

board.disk = SimpleMemory(
    range=AddrRange(DISK_BASE, size=disk_size),
    latency=args.disk_latency,
    bandwidth=args.disk_bw,
    kvm_map=False,           # 不让 KVM 映射这个高地址
    conf_table_reported=False,
)
# 磁盘是 I/O 设备，挂在独立非一致性总线（NoncoherentXBar）上，
# 磁盘 DMA 引擎直接连这个总线，避免经过一致性 membus 的 snoop 路径。
board.disk_xbar = NoncoherentXBar(
    width=16, frontend_latency=2, forward_latency=1, response_latency=2,
)
board.disk_xbar.mem_side_ports = board.disk.port
# 显式加入系统的 memories 列表，保证 isMemAddr() 命中、system() 回指正确
board.memories.append(board.disk)

# 磁盘控制器 DMA 引擎（合成流量源），代表把数据刷到磁盘
board.disk_dma = PyTrafficGen(progress_check="1000000s")
board.disk_dma.port = board.disk_xbar.cpu_side_ports

disk_start = DISK_BASE
disk_end = DISK_BASE + copy_bytes

# 磁盘写注入速率设得远高于磁盘带宽，让磁盘 bandwidth 成为真正瓶颈
# 注意 TrafficGen 的 blocksize 必须 <= cache line(64B)，因此磁盘写也用 64B 请求
disk_offered_rate = toMemoryBandwidth("64GB/s")
disk_period = max(1, fromSeconds(block_size / disk_offered_rate))

gpu_copy_state = None
gpu_exit_state = None
# Here we set the Full System workload.
# The `set_kernel_disk_workload` function for the X86Board takes a kernel, a
# disk image, and, optionally, a command to run.

# This is the command to run after the system has booted. The first `m5 exit`
# will stop the simulation so we can switch the CPU cores from KVM/ATOMIC to 
# TIMING/O3 and continue the simulation to run the command. After simulation
# has ended you may inspect `m5out/board.pc.com_1.device` to see the echo
# output.
# command = (
#     "m5 exit;"
#     + "numactl -H;"
#     + "m5 resetstats;"
#     # + "/home/cxl_benchmark/" + args.test_cmd + ";"
#     + "numactl -N 0 -m 1 /home/test_code/simple_test;"
# )

# command = (
#     "m5 exit;"
#     + "numactl -H;"
#     + "m5 resetstats;"
#     + "/home/cxl_benchmark/" + args.test_cmd + ";"
#     + "m5 dumpstats;"
#     + "m5 exit;"
# )

command = (
    "m5 exit;"
)

# command = """
# m5 exit
# numactl -H

# echo "===== DDR only: node 0 ====="
# m5 resetstats
# numactl --cpunodebind=0 --membind=0 /home/test_code/simple_test
# m5 dumpstats

# echo "===== CXL only: node 1 ====="
# m5 resetstats
# numactl --cpunodebind=0 --membind=1 /home/test_code/simple_test
# m5 dumpstats

# echo "===== DDR+CXL page interleave ====="
# m5 resetstats
# numactl --cpunodebind=0 --interleave=0,1 /home/test_code/simple_test
# m5 dumpstats

# m5 exit
# """

# Please modify the paths of kernel and disk_image according to the location of your files.
board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/root/simcxl-resources/vmlinux'),
    disk_image=DiskImageResource(local_path='/root/simcxl-resources/parsec.img'),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)

# simulator = Simulator(
#     board=board,
#     on_exit_event={
#         ExitEvent.EXIT: (func() for func in [processor.switch])
#     },
# )

# 1 tick == 1 ps (全局频率默认 1 THz，未调用 setGlobalFrequency)
TICK_PER_SEC = 1e12


def bw_str(nbytes, elapsed_s):
    return f"{nbytes / elapsed_s / 1e9:.2f} GB/s"


def handle_exit():
    # ---- 第一次退出：启动 GPU -> CXL 拷贝 ----
    processor.switch()
    m5.stats.reset()
    t_gpu_start = m5.curTick()

    # createLinear/createExit为C++方法，需在instantiate之后才能调用，
    # 因此在此(第一次exit时，系统已实例化)创建generator
    gpu_copy_state = board.gpu_dma.createLinear(
        m5.MaxTick,        # 无时间限制，由 data_limit 结束
        gpu_test_start,
        gpu_test_end,
        block_size,
        period,
        period,
        read_percent,
        copy_bytes,
    )
    gpu_exit_state = board.gpu_dma.createExit(0)
    board.gpu_dma.start([gpu_copy_state, gpu_exit_state])

    yield False

    # ---- 第二次退出：GPU -> CXL 完成，启动 CXL -> disk 拷贝 ----
    gpu_elapsed = (m5.curTick() - t_gpu_start) / TICK_PER_SEC
    print(f"[GPU<->CXL] {copy_bytes} bytes in {gpu_elapsed:.6f}s "
          f"= {bw_str(copy_bytes, gpu_elapsed)} "
          f"({'write' if read_percent == 0 else 'read'})")

    m5.stats.reset()
    t_disk_start = m5.curTick()

    # CXL -> disk：数据源在 CXL（读侧很快，非瓶颈），瓶颈在磁盘写。
    # 这里磁盘 DMA 引擎把 copy_bytes 写到磁盘（SimpleMemory 按 --disk-bw 节流）。
    disk_write_state = board.disk_dma.createLinear(
        m5.MaxTick,        # 无时间限制，由 data_limit 结束
        disk_start,
        disk_end,
        block_size,        # 64B cache line（TrafficGen 限制）
        disk_period,
        disk_period,
        0,                # 全部写（落盘）
        copy_bytes,
    )
    disk_exit_state = board.disk_dma.createExit(0)
    board.disk_dma.start([disk_write_state, disk_exit_state])

    yield False

    # ---- 第三次退出：CXL -> disk 完成 ----
    disk_elapsed = (m5.curTick() - t_disk_start) / TICK_PER_SEC
    print(f"[CXL->disk] {copy_bytes} bytes flushed to disk in {disk_elapsed:.6f}s "
          f"(disk write bandwidth = {args.disk_bw}; "
          f"see board.disk.bwWrite in stats.txt for the measured value)")

    m5.stats.dump()
    yield True

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: handle_exit()
    },
)
print("Running the simulation Classic MESI Three Level protocol...")
print("Using KVM cpu for boot")

m5.stats.reset()

simulator.run()
