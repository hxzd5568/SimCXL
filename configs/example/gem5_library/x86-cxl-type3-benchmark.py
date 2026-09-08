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
Clean CXL Type-3 baseline benchmark (Step 0).

Runs the CXL memory expander with the *default* CXL device model
(CXLMemCtrl proto_proc_lat 15ns/60ns, queue 48/36) and measures the
DRAM vs CXL latency/bandwidth using the benchmarks in the disk image,
bound via numactl (membind=0 => DRAM, membind=1 => CXL node).

This is the paper-validated path, without the GPU/disk synthetic
harness added for the checkpoint experiments.
"""

import argparse
import m5
from gem5.utils.requires import requires
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import DIMM_DDR5_4400, SingleChannelDDR4_3200
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource

requires(isa_required=ISA.X86)

parser = argparse.ArgumentParser(description='CXL Type-3 baseline.')
parser.add_argument('--is_asic', type=str, nargs='?', choices=['True', 'False'],
                    default='True', help='CXL ASIC (True) or FPGA (False)')
test_choices = [
    'lmbench_cxl.sh', 'lmbench_dram.sh',
    'merci_dram.sh', 'merci_cxl.sh', 'merci_dram+cxl.sh',
    'stream_dram.sh', 'stream_cxl.sh'
]
parser.add_argument('--test_cmd', type=str, choices=test_choices,
                    default='stream_cxl.sh', help='Benchmark to run')
parser.add_argument('--num_cpus', type=int, default=1)
parser.add_argument('--cpu_type', type=str, choices=['TIMING', 'O3'], default='TIMING')
parser.add_argument('--cxl_mem_type', type=str, choices=['Simple', 'DRAM'], default='DRAM')

args = parser.parse_args()

cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB", l1d_assoc=6, l1i_size="32kB", l1i_assoc=8,
    l2_size="2MB", l2_assoc=16, l3_size="96MB", l3_assoc=48,
)

memory = DIMM_DDR5_4400(size="3GB")
cxl_dram = DIMM_DDR5_4400(size="8GB")
if args.is_asic == 'False':
    cxl_dram = SingleChannelDDR4_3200(size="8GB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.O3 if args.cpu_type == 'O3' else CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=args.num_cpus,
)
for proc in processor.start:
    proc.core.usePerf = False

board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=(args.is_asic == 'True'),
)

command = (
    "m5 exit;"                 # first exit: switch KVM -> TIMING
    + "numactl -H;"            # print NUMA topology (CXL should be node 1)
    + "echo '===== DRAM memcpy (membind=0) =====';"
    + "numactl --membind=0 --cpunodebind=0 /home/test_code/memcpy_test 64 1;"
    + "echo '===== CXL memcpy (membind=1) =====';"
    + "numactl --membind=1 --cpunodebind=0 /home/test_code/memcpy_test 64 1;"
    + "m5 exit;"                # second exit: dump + end
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/root/simcxl-resources/vmlinux'),
    disk_image=DiskImageResource(local_path='/root/simcxl-resources/parsec.img'),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)


def handle_exit():
    processor.switch()
    m5.stats.reset()
    yield False   # continue simulation (run benchmark)
    m5.stats.dump()
    yield True    # benchmark done, end


simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: handle_exit()},
)

print("Running CXL Type-3 baseline benchmark...")
print("Using KVM cpu for boot")

m5.stats.reset()
simulator.run()
