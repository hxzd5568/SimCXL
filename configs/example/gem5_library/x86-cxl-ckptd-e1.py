# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCkptDevice E1 test: pure-write staging bandwidth (single DRAM / single CXL
/ dual-lane), isolating the metadata DMA (descriptor fetch + completion write)
from the payload write path.

Boots a single x86 VM, switches KVM -> Timing, and runs ckptbench_e1, which
drives the SimCkptDevice pure-stage mode (skip descriptor fetch, skip
completion write, only sequential 4 KiB payload writes).
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
    default=2,
    help="Checkpoint size in MiB (<=2 keeps CXL buffer in one "
    "contiguous extent)",
)
_parser.add_argument(
    "--pcie-gtps",
    type=int,
    default=32,
    help="PCIe per-lane line rate in GT/s (Gen5=32, Gen4=16, "
    "Gen3=8); 128b/130b + TLP overhead applied in model",
)
_parser.add_argument(
    "--dram-channels",
    type=int,
    default=4,
    help="DRAM channel count (each = one 32-bit DDR5 "
    "subchannel = 17.6 GB/s)",
)
_parser.add_argument(
    "--cxl-channels",
    type=int,
    default=2,
    help="CXL channel count (each = one 32-bit DDR5 "
    "subchannel = 17.6 GB/s)",
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
)
board.pc.south_bridge.simckpt_device.num_queues = 2
board.pc.south_bridge.simckpt_device.queue_depth = 32
for link in board._simckpt_pcie_links:
    link.line_rate_gtps = _args.pcie_gtps

command = (
    "m5 exit;"
    + "echo 1 > /sys/bus/pci/devices/0000:00:07.0/enable 2>/dev/null || "
    + "echo 'enable failed';"
    + f"/home/test_code/ckptbench_e1 --ckpt-mib {_args.ckpt_mib};"
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

print("Running SimCkptDevice E1 (pure-write staging bandwidth) test...")
m5.stats.reset()
simulator.run()
