# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCkptDevice E5 config: target2 S2 4-GPU dual-path staging with a shared
switch uplink (E5) + Gen4 GPU links (E3) + CXL P2P / CXL.io (E2).

Topology (target2.md section 1.2):
  4 GPU lanes, each with its own Gen4 x16 link (27.77 GB/s). Their DRAM-bound
  traffic fans into ONE shared Gen5 x16 switch uplink (55.53 GB/s) through a
  NoncoherentXBar -> the multi-GPU shared uplink that becomes the bottleneck
  for DRAM-only staging. 2 CXL lanes each get a direct Gen5 x16 expander link
  (P2P, on-device DMA, no uplink) with a one-time CXL.io initial-command
  latency.

The S2 benchmark (ckptbench_e5_s2) measures:
  DRAM-only : all 4 GPU lanes stage to DRAM through the shared uplink
              (expect ~55.53 GB/s, uplink-limited)
  dual      : 2 GPU lanes -> DRAM (uplink) + 2 GPU lanes -> CXL (P2P)
              (expect ~111 GB/s = 55.53 + 55.53, i.e. ~2.0x speedup)
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
    "--ckpt-mib",
    type=int,
    default=64,
    help="Total checkpoint size in MiB (split across the active lanes)",
)
_parser.add_argument(
    "--dram-channels",
    type=int,
    default=8,
    help="Host DRAM channel count (8 subchannels = 140.8 GB/s, non-bottleneck)",
)
_parser.add_argument(
    "--cxl-channels",
    type=int,
    default=8,
    help="CXL DRAM channel count (8 subchannels = 140.8 GB/s, non-bottleneck)",
)
_parser.add_argument(
    "--gpu-lanes",
    type=int,
    default=4,
    help="Number of GPU DRAM lanes (Gen4 x16 each)",
)
_parser.add_argument(
    "--cxl-lanes",
    type=int,
    default=2,
    help="Number of CXL P2P lanes (Gen5 x16 each)",
)
_parser.add_argument(
    "--write-scale",
    type=float,
    default=1.0,
    help="PcieLink write-completion scale (1.0 = posted, ~1.5 = non-posted "
    "write completion -> write BW = read / scale)",
)
_parser.add_argument(
    "--cxl-io-latency",
    type=str,
    default="1us",
    help="CXL.io one-time initial command latency on the CXL lanes",
)
_args, _ = _parser.parse_known_args()

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
cxl_dram = ChanneledMemory(DDR5_4400_4x8, _args.cxl_channels, 64, size="8GB")

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
    simckpt_num_queues=_args.gpu_lanes + _args.cxl_lanes,
    simckpt_num_dram_lanes=_args.gpu_lanes,
    simckpt_num_cxl_lanes=_args.cxl_lanes,
)
board.pc.south_bridge.simckpt_device.queue_depth = 32

# Apply the write-completion scale to every link (GPU/uplink/CXL); this is the
# E2 write-completion modelling knob (default 1.0 keeps the v2.1 numbers).
for link in board._simckpt_pcie_links:
    link.write_scale = _args.write_scale
if board._simckpt_uplink is not None:
    board._simckpt_uplink.write_scale = _args.write_scale

# CXL P2P lanes carry the CXL.io one-time initial command latency.
for link in board._simckpt_cxl_links:
    link.init_cmd_latency = _args.cxl_io_latency

command = (
    "m5 exit;"
    + "echo 1 > /sys/bus/pci/devices/0000:00:07.0/enable 2>/dev/null || "
    + "echo 'enable failed';"
    + f"/home/test_code/ckptbench_e5_s2 --ckpt-mib {_args.ckpt_mib};"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path="/root/simcxl-resources/vmlinux"),
    disk_image=DiskImageResource(
        local_path="/root/simcxl-resources/parsec.img"
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

print("Running SimCkptDevice E5 (S2 4-GPU shared-uplink staging) test...")
m5.stats.reset()
simulator.run()
