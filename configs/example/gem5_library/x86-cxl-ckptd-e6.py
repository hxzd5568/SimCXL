# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCkptDevice E6 config: target3 CXL-memory-budget sweep (write staging).

N GPUs (4 or 8) simultaneously write. DRAM-bound traffic fans into ONE shared
Gen5 x16 switch uplink (55.53 GB/s); CXL-bound traffic goes over M Gen5 x16
P2P lanes to a CXL expander whose *backend bandwidth* scales with the CXL
memory budget (channels x 17.6 GB/s). The benchmark (ckptbench_e6_budget)
splits the GPUs between DRAM and CXL at the optimal ratio and reports the
speedup over the DRAM-only single-path baseline.

Budget -> channels map (target3.md sec 2.1): n_ch = min(2 * GB, 8)
  1 GB -> 2 ch (35.2 GB/s), 2 GB -> 4 ch (70.4), 4/8 GB -> 8 ch (140.8).
"""

import argparse

import m5

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.memory.dram_interfaces.ddr5 import DDR5_4400_4x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

_parser = argparse.ArgumentParser(add_help=False)
_parser.add_argument(
    "--gpus",
    type=int,
    default=4,
    help="Number of GPUs writing simultaneously (DRAM lanes)",
)
_parser.add_argument(
    "--cxl-lanes",
    type=int,
    default=1,
    help="Number of CXL P2P lanes (Gen5 x16 each); 1 = single expander",
)
_parser.add_argument(
    "--cxl-mem-size",
    type=int,
    default=8,
    help="CXL memory budget in GB (backend channels = min(2*GB, 8))",
)
_parser.add_argument(
    "--dram-channels",
    type=int,
    default=8,
    help="Host DRAM channel count (8 subchannels = 140.8 GB/s)",
)
_parser.add_argument(
    "--ckpt-mib",
    type=int,
    default=32,
    help="Total checkpoint size in MiB (split evenly across the GPUs)",
)
_parser.add_argument("--chunk", type=int, default=4096)
_parser.add_argument(
    "--split-dram-frac",
    type=float,
    default=-1.0,
    help="Optimal DRAM fraction (default: computed from theoretical rates)",
)
_parser.add_argument(
    "--probe-cxl",
    action="store_true",
    help="Also measure the CXL-only single-path bandwidth",
)
_parser.add_argument(
    "--write-scale",
    type=float,
    default=1.0,
    help="PcieLink write-completion scale (1.0 = posted, ~1.5 = non-posted)",
)
_parser.add_argument(
    "--cxl-io-latency",
    type=str,
    default="1us",
    help="CXL.io one-time initial command latency on the CXL lanes",
)
_args, _ = _parser.parse_known_args()

# --- theoretical line rates (target3.md sec 2.1) --------------------------
PCIE_GEN5_X16 = 55.53  # GB/s (uplink and expander link)
DDR5_SUBCH = 17.6      # GB/s per 32-bit DDR5 subchannel

_cxl_ch = min(2 * _args.cxl_mem_size, 8)
_b_dram_line = min(PCIE_GEN5_X16, _args.dram_channels * DDR5_SUBCH)
_b_cxl_line = min(_args.cxl_lanes * PCIE_GEN5_X16, _cxl_ch * DDR5_SUBCH)
_split_frac = _args.split_dram_frac
if _split_frac < 0.0:
    _split_frac = _b_dram_line / (_b_dram_line + _b_cxl_line)

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=8,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    num_l2_banks=1,
)

memory = ChanneledMemory(DDR5_4400_4x8, _args.dram_channels, 64, size="3GiB")
cxl_dram = ChanneledMemory(
    DDR5_4400_4x8, _cxl_ch, 64, size=f"{_args.cxl_mem_size}GB"
)

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
    add_simckpt_device=True,
    simckpt_s2_topology=True,
    simckpt_num_queues=_args.gpus + _args.cxl_lanes,
    simckpt_num_dram_lanes=_args.gpus,
    simckpt_num_cxl_lanes=_args.cxl_lanes,
)
board.pc.south_bridge.simckpt_device.queue_depth = 32

for link in board._simckpt_pcie_links:
    link.write_scale = _args.write_scale
if board._simckpt_uplink is not None:
    board._simckpt_uplink.write_scale = _args.write_scale
for link in board._simckpt_cxl_links:
    link.init_cmd_latency = _args.cxl_io_latency

_probe = " --probe-cxl" if _args.probe_cxl else ""
command = (
    "m5 exit;"
    + "echo 1 > /sys/bus/pci/devices/0000:00:07.0/enable 2>/dev/null || "
    + "echo 'enable failed';"
    + f"/home/test_code/ckptbench_e6_budget --gpus {_args.gpus} "
    + f"--cxl-lanes {_args.cxl_lanes} --ckpt-mib {_args.ckpt_mib} "
    + f"--chunk {_args.chunk} --split-dram-frac {_split_frac:.6f}{_probe};"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path="/home/work/simcxl-resources/vmlinux"),
    disk_image=DiskImageResource(
        local_path="/home/work/simcxl-resources/parsec.img"
    ),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)


def handle_exit():
    processor.switch()
    m5.stats.reset()
    yield False
    m5.stats.dump()
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: handle_exit()},
)

print(
    f"Running SimCkptDevice E6 budget sweep: {_args.gpus} GPU + "
    f"{_args.cxl_lanes} CXL lane(s), CXL {_args.cxl_mem_size} GB "
    f"({_cxl_ch} ch = {_cxl_ch * DDR5_SUBCH:.1f} GB/s backend), "
    f"optimal split f_dram={_split_frac:.3f}"
)
m5.stats.reset()
simulator.run()
