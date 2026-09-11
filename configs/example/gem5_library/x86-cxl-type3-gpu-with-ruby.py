# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
GPU -> CXL Type-3 bandwidth test with the Ruby MESI Two Level protocol.

This mirrors the Classic GPU DMA test, but uses the Ruby memory system
(the CXL memory is reached through the Ruby DMA machine instead of the
Classic CXLBridge/CXLMemCtrl path).
"""

import argparse
import m5
from gem5.utils.requires import requires
from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource
from m5.objects import PyTrafficGen
from m5.ticks import fromSeconds
from m5.util.convert import toMemoryBandwidth, toMemorySize

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

parser = argparse.ArgumentParser(description='CXL Type-3 GPU DMA (Ruby).')
parser.add_argument('--gpu-copy-mib', type=int, default=512)
parser.add_argument('--gpu-offered-bw', type=str, default="32GB/s")
parser.add_argument('--gpu-request-size', type=int, choices=[64, 128, 256], default=64)
parser.add_argument('--gpu-op', choices=['write', 'read'], default='write')
parser.add_argument('--cxl-test-reserve', type=str, default=None)
parser.add_argument('--cxl-rsp-size', type=int, default=48)
parser.add_argument('--cxl-proto-lat', type=str, default="15ns")
parser.add_argument('--cxl-addr-map', type=str, default="RoRaBaCoCh")
parser.add_argument('--cxl-tcl', type=str, default=None)
args = parser.parse_args()

read_percent = 100 if args.gpu_op == 'read' else 0

if args.cxl_test_reserve is None:
    args.cxl_test_reserve = f"{max(64, args.gpu_copy_mib)}MiB"

cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=8,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    num_l2_banks=1,
)

memory = DIMM_DDR5_4400(size="3GiB")
cxl_dram = DIMM_DDR5_4400(size="8GB")
for _mc in cxl_dram.get_memory_controllers():
    _mc.dram.addr_mapping = args.cxl_addr_map
    if args.cxl_tcl:
        _mc.dram.tCL = args.cxl_tcl
        _mc.dram.tRCD = args.cxl_tcl
        _mc.dram.tRP = args.cxl_tcl
        _mc.dram.tRTP = args.cxl_tcl
        _mc.dram.tCCD_L = "4ns"
        _mc.dram.tRRD = args.cxl_tcl
        _mc.dram.tXAW = args.cxl_tcl

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)
for proc in processor.start:
    proc.core.usePerf = False

board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=True,
)
board.pc.south_bridge.cxl_device.rsp_size = args.cxl_rsp_size
board.pc.south_bridge.cxl_device.req_size = args.cxl_rsp_size
board.pc.south_bridge.cxl_device.proto_proc_lat = args.cxl_proto_lat

# GPU DMA 合成流量源，接到 IO 总线（在 Ruby 下经 DMA machine 访问 CXL）
# max_outstanding_reqs 限制在飞请求数，配合下面的 completion 等待实现端到端计时。
board.gpu_dma = PyTrafficGen(progress_check="1000000s",
                             max_outstanding_reqs=args.cxl_rsp_size * 4)
board.gpu_dma.port = board.iobus.cpu_side_ports

m5.ticks.fixGlobalFrequency()

copy_bytes = args.gpu_copy_mib * 1024 * 1024
block_size = args.gpu_request_size

CXL_BASE = 0x100000000
reserve_size = toMemorySize(args.cxl_test_reserve)
cxl_size = cxl_dram.get_size()
if copy_bytes > reserve_size:
    raise ValueError("gpu-copy-mib exceeds reserve")
if reserve_size >= cxl_size:
    raise ValueError("cxl-test-reserve must be smaller than cxl-size")
cxl_test_start = CXL_BASE + cxl_size - reserve_size
cxl_test_end = cxl_test_start + copy_bytes

# x86_board.py 里 CXL 的 E820 默认只预留 64MiB，这里按 --cxl-test-reserve
# 覆盖成实际预留大小，让 Linux 只看到 cxl_size - reserve_size，顶部留给测试。
cxl_linux_visible = cxl_size - reserve_size
for e in board.workload.e820_table.entries:
    if e.addr == CXL_BASE and e.range_type == 20:
        e.size = f"{cxl_linux_visible}B"
        break

offered_rate = toMemoryBandwidth(args.gpu_offered_bw)
period = max(1, fromSeconds(block_size / offered_rate))

command = (
    "m5 exit;"
    + "numactl -H;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/root/simcxl-resources/vmlinux'),
    disk_image=DiskImageResource(local_path='/root/simcxl-resources/parsec.img'),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)

TICK_PER_SEC = 1e12

POLL_TICKS = int(TICK_PER_SEC * 1e-6)  # poll every 1us

# 写完成在 bytesWritten 累加，读完成在 bytesRead 累加（都在 recvTimingResp 里）。
_completion_stat = "bytesWritten" if read_percent == 0 else "bytesRead"


def _bytes_done():
    return int(board.gpu_dma.resolveStat(_completion_stat).value)


def handle_exit():
    processor.switch()
    m5.stats.reset()
    t_start = m5.curTick()

    gpu_copy_state = board.gpu_dma.createLinear(
        m5.MaxTick,
        cxl_test_start,
        cxl_test_end,
        block_size,
        period,
        period,
        read_percent,   # 100=读, 0=写
        copy_bytes,
    )
    gpu_exit_state = board.gpu_dma.createExit(0)
    board.gpu_dma.start([gpu_copy_state, gpu_exit_state])
    # 第一次退出：注入完成（LinearGen 到达 data_limit -> ExitGen）。此时还有
    # 在飞请求未返回，不能立刻计时。
    yield False

    # 等待所有写响应返回（bytesWritten/bytesRead 在 recvTimingResp 里累加，代表
    # 真正落库完成）。用 tick exit 反复轮询。
    while _bytes_done() < copy_bytes:
        m5.scheduleTickExitFromCurrent(POLL_TICKS)
        yield False

    done = _bytes_done()
    assert done == copy_bytes, \
        f"{_completion_stat} {done} != copy_bytes {copy_bytes}"

    elapsed = (m5.curTick() - t_start) / TICK_PER_SEC
    op = 'read' if read_percent == 100 else 'write'
    print(f"[GPU->CXL (Ruby)] {copy_bytes} bytes in {elapsed:.6f}s "
          f"= {copy_bytes / elapsed / 1e9:.2f} GB/s ({op})")
    m5.stats.dump()
    yield True


_handler = handle_exit()
simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: _handler,
        ExitEvent.SCHEDULED_TICK: _handler,
    },
)

print("Running GPU->CXL (Ruby MESI Two Level)...")
m5.stats.reset()
simulator.run()
