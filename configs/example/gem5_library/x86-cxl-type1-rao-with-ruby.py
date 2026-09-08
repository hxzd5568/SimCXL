"""
Run a CXL Type-1 RAO accelerator with the Ruby CXL MESI protocol.
"""

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board_cxl_type1_rao import X86BoardCXLType1RAO
from gem5.components.cachehierarchies.ruby.cxl_mesi_two_level_cache_hierarchy import (
    CXLMESITwoLevelCacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import DiskImageResource, KernelResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.CXL_MESI_TWO_LEVEL,
    kvm_required=True,
)

cache_hierarchy = CXLMESITwoLevelCacheHierarchy(
    l1d_size="128KiB",
    l1d_assoc=8,
    l1i_size="32KiB",
    l1i_assoc=8,
    l2_size="512KiB",
    l2_assoc=16,
    num_l2_banks=1,
)

memory = DIMM_DDR5_4400(size="3GiB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

for proc in processor.start:
    proc.core.usePerf = False

board = X86BoardCXLType1RAO(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

command = (
    "m5 exit;"
    + "echo 'This is running on Timing CPU cores.';"
    + "/home/cxl_benchmark/rao/type1_rao_test /home/cxl_benchmark/rao/trace/RAM_ADD_100.csv;"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path="/root/simcxl-resources/vmlinux"),
    disk_image=DiskImageResource(local_path="/root/simcxl-resources/parsec.img"),
    readfile_contents=command,
)

simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: (func() for func in [processor.switch])},
)

simulator.run()
