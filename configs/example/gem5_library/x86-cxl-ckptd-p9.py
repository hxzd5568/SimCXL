# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCkptDevice P9 test: explicit topology + manifest state machine (Ruby).

Boots a single x86 VM, switches KVM -> Timing, and runs ckptbench_p9, which
builds the CXLMemSim-style topology object {GPU, DRAM pool, CXL pool, N storage
channels}, derives striping and candidate paths, and drives a save/restore
through the full chunk lifecycle (FREE -> PINNED -> IN_FLIGHT -> DISK_COMMITTED
-> HOT -> EVICTABLE), reporting per-channel balance, manifest version and the
final state histogram. The storage channel count is configurable via
--storage-channels.
"""

import m5
from gem5.utils.requires import requires
from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource

import argparse
_parser = argparse.ArgumentParser(add_help=False)
_parser.add_argument("--storage-channels", type=int, default=4,
                     help="Number of ParallelStorage channels (P9 N-channel)")
_args, _ = _parser.parse_known_args()

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1d_size="48kB", l1d_assoc=8, l1i_size="32kB", l1i_assoc=8,
    l2_size="2MB", l2_assoc=16, num_l2_banks=1,
)

memory = DIMM_DDR5_4400(size="3GiB")
cxl_dram = DIMM_DDR5_4400(size="8GB")

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
board.simckpt_storage.num_channels = _args.storage_channels

command = (
    "m5 exit;"
    + "echo 1 > /sys/bus/pci/devices/0000:00:07.0/enable 2>/dev/null || "
    + "echo 'enable failed';"
    + f"/home/test_code/ckptbench_p9 --channels {_args.storage_channels};"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path="/root/simcxl-resources/vmlinux"),
    disk_image=DiskImageResource(local_path="/root/simcxl-resources/parsec.img"),
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

print("Running SimCkptDevice P9 (topology + state machine) test...")
m5.stats.reset()
simulator.run()
