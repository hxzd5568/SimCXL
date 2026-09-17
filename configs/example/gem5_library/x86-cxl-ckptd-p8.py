# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCkptDevice P8 test: analysis model + cycle-simulation co-design (Ruby).

Boots a single x86 VM, switches KVM -> Timing, and runs ckptbench_p8, which
generates a parameterized checkpoint trace (LLMServingSim request-trace style)
and reports the P8 metrics (save time, save bandwidth, time-to-resume, hot
standby hit rate, per-chunk P95 latency) alongside the analytical model's
prediction (trace_gen.c). Two samples are taken in one boot to validate the
hot-cold key point: hot_frac 0.50 and 0.00.
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

# Two sampling points in one boot: hot_frac 0.50 and 0.00.
command = (
    "m5 exit;"
    + "echo 1 > /sys/bus/pci/devices/0000:00:07.0/enable 2>/dev/null || "
    + "echo 'enable failed';"
    + "/home/test_code/ckptbench_p8 --hot-frac 0.50;"
    + "/home/test_code/ckptbench_p8 --hot-frac 0.00;"
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

print("Running SimCkptDevice P8 (analysis model + cycle sim) test...")
m5.stats.reset()
simulator.run()
