# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
CXL checkpoint workflow benchmark.

Steps (each measurable independently):
  1. mount CXL      -- guest onlines the e820 type-20 CXL memory as a NUMA node
                       (dax_kmem), then numactl --membind can target it.
  2. GPU HBM->CXL   -- PyTrafficGen writes the CXL reserve (0x2FC000000), the
                       direct CXL.mem path.
  2'. GPU HBM->HBM   -- PyTrafficGen writes main memory, the fallback source.
  3. HBM->CXL copy   -- a guest memcpy benchmark reads main memory and writes
                       the CXL NUMA node.

Modes:
  --run-memcpy              run the CPU memcpy benchmark instead of GPU traffic
  --gpu-target cxl|hbm      GPU traffic destination (cxl = CXL reserve,
                            hbm = main memory)
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

parser = argparse.ArgumentParser(description='CXL checkpoint benchmark (Ruby).')
parser.add_argument('--gpu-copy-mib', type=int, default=64)
parser.add_argument('--gpu-offered-bw', type=str, default="32GB/s")
parser.add_argument('--gpu-request-size', type=int, choices=[64, 128, 256], default=64)
parser.add_argument('--gpu-op', choices=['write', 'read'], default='write')
parser.add_argument('--gpu-target', choices=['cxl', 'hbm'], default='cxl')
parser.add_argument('--run-memcpy', action='store_true')
parser.add_argument('--dma-copy', action='store_true',
                    help='DRAM->CXL DMA copy: read DRAM and write CXL '
                         'simultaneously with two traffic generators')
parser.add_argument('--memcpy-size-mib', type=int, default=64)
parser.add_argument('--memcpy-iters', type=int, default=10)
parser.add_argument('--memcpy-src-node', type=int, default=0)
parser.add_argument('--memcpy-dst-node', type=int, default=1)
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

# Widen the IO bus so it is not the bottleneck for the DMA copy: the 128-bit
# default caps responses at ~30 GB/s, which is below the ~35 GB/s DRAM read
# throughput and would overflow the DMA sequencer's response queue.
board.iobus.width = 32

board.gpu_dma = PyTrafficGen(progress_check="1000000s")
board.gpu_dma.port = board.iobus.cpu_side_ports
board.copy_read = PyTrafficGen(progress_check="1000000s")
board.copy_read.port = board.iobus.cpu_side_ports
board.copy_write = PyTrafficGen(progress_check="1000000s")
board.copy_write.port = board.iobus.cpu_side_ports

m5.ticks.fixGlobalFrequency()

copy_bytes = args.gpu_copy_mib * 1024 * 1024
block_size = args.gpu_request_size

CXL_BASE = 0x100000000
reserve_size = toMemorySize(args.cxl_test_reserve)
cxl_size = cxl_dram.get_size()
if copy_bytes > reserve_size:
    raise ValueError("gpu-copy-mib exceeds reserve")
cxl_test_start = CXL_BASE + cxl_size - reserve_size
cxl_test_end = cxl_test_start + copy_bytes

# main memory (HBM stand-in) target: above the 1MB boot region, below 3GB
hbm_test_start = 0x100000
hbm_test_end = hbm_test_start + copy_bytes
if hbm_test_end > 0xC0000000:
    raise ValueError("gpu-copy-mib exceeds main-memory capacity")

target_start = cxl_test_start if args.gpu_target == 'cxl' else hbm_test_start
target_end = cxl_test_end if args.gpu_target == 'cxl' else hbm_test_end
target_name = 'CXL' if args.gpu_target == 'cxl' else 'HBM'

offered_rate = toMemoryBandwidth(args.gpu_offered_bw)
period = max(1, fromSeconds(block_size / offered_rate))

if args.run_memcpy:
    command = (
        "modprobe dax_kmem;"
        + "numactl -H;"
        + "m5 exit;"  # switch to TIMING, then run the copy in timing mode
        + f"./memcpy_test {args.memcpy_size_mib * 1024 * 1024} "
        + f"{args.memcpy_src_node} {args.memcpy_dst_node} {args.memcpy_iters};"
        + "m5 exit;"
    )
else:
    command = "m5 exit; numactl -H;"

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/root/simcxl-resources/vmlinux'),
    disk_image=DiskImageResource(local_path='/root/simcxl-resources/parsec.img'),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)

TICK_PER_SEC = 1e12


_phase = [0]
_t_start = [0]


def handle_exit():
    if _phase[0] == 0:
        # first exit (m5 exit): switch CPU, reset stats, kick off the test
        processor.switch()
        m5.stats.reset()
        _t_start[0] = m5.curTick()

        if args.dma_copy:
            # DMA copy: read DRAM and write CXL simultaneously
            read_state = board.copy_read.createLinear(
                m5.MaxTick, hbm_test_start, hbm_test_end, block_size,
                period, period, 100, copy_bytes)
            read_exit = board.copy_read.createExit(0)
            board.copy_read.start([read_state, read_exit])
            write_state = board.copy_write.createLinear(
                m5.MaxTick, cxl_test_start, cxl_test_end, block_size,
                period, period, 0, copy_bytes)
            write_exit = board.copy_write.createExit(0)
            board.copy_write.start([write_state, write_exit])
            _phase[0] = 1
            yield False
        elif args.run_memcpy:
            # let the guest run memcpy_test (its own clock_gettime)
            _phase[0] = 2
            yield False
        else:
            gpu_copy_state = board.gpu_dma.createLinear(
                m5.MaxTick, target_start, target_end, block_size,
                period, period, read_percent, copy_bytes)
            gpu_exit_state = board.gpu_dma.createExit(0)
            board.gpu_dma.start([gpu_copy_state, gpu_exit_state])
            _phase[0] = 2
            yield False

    if _phase[0] == 1:
        # first of the two DMA-copy generators finished; wait for the second
        _phase[0] = 2
        yield False

    # done: measure and dump
    elapsed = (m5.curTick() - _t_start[0]) / TICK_PER_SEC
    if args.dma_copy:
        print(f"[DMA copy DRAM->CXL] {copy_bytes} bytes in {elapsed:.6f}s "
              f"= {copy_bytes / elapsed / 1e9:.2f} GB/s")
    elif args.run_memcpy:
        print(f"[HBM->CXL memcpy] guest ran in {elapsed:.6f}s "
              f"(see guest output for GB/s)")
    else:
        op = 'write' if read_percent == 0 else 'read'
        print(f"[GPU->{target_name} ({op})] {copy_bytes} bytes in "
              f"{elapsed:.6f}s = {copy_bytes / elapsed / 1e9:.2f} GB/s")

    m5.stats.dump()
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: handle_exit()},
)

print("Running CXL checkpoint benchmark (Ruby MESI Two Level)...")
m5.stats.reset()
simulator.run()
