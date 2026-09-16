# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
SimCXL AI checkpoint P0 baseline (Ruby MESI Two Level).

This is the "correct baseline" configuration for the AI checkpoint project.
It boots a single x86 VM with KVM, switches to Timing CPU, and hands the
*entire* CXL Type-3 memory to the guest via ``dax_kmem`` (the CXL memory
becomes a System RAM NUMA node owned by Linux, ready for the pinned pool in
later phases). No CXL address range is reserved for external PyTrafficGen.

The synthetic GPU (PyTrafficGen) is kept only to *calibrate* the raw DRAM and
CXL bandwidth of the DMA data path (GPU->DRAM and GPU->CXL), not for
end-to-end data-correctness tests:

  phase 0  GPU -> DRAM  (write)
  phase 1  DRAM -> GPU  (read)
  phase 2  GPU -> CXL   (write)
  phase 3  CXL  -> GPU  (read)

Each phase waits for every in-flight request to drain (``bytesWritten`` /
``bytesRead`` reaches ``copy_bytes``) before timing, so the reported bandwidth
is end-to-end throughput, not injection throughput.
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
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource
from m5.objects import PyTrafficGen
from m5.ticks import fromSeconds
from m5.util.convert import toMemoryBandwidth

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

parser = argparse.ArgumentParser(
    description="SimCXL AI checkpoint P0 baseline (Ruby)."
)
parser.add_argument("--gpu-copy-mib", type=int, default=512)
parser.add_argument("--gpu-offered-bw", type=str, default="32GB/s")
parser.add_argument(
    "--gpu-request-size", type=int, choices=[64, 128, 256], default=64
)
parser.add_argument("--cxl-size", type=str, default="8GB")
parser.add_argument("--cxl-rsp-size", type=int, default=48)
parser.add_argument("--cxl-proto-lat", type=str, default="15ns")
parser.add_argument("--cxl-addr-map", type=str, default="RoRaBaCoCh")
parser.add_argument("--cxl-tcl", type=str, default=None)
parser.add_argument("--skip-dram", action="store_true",
                    help="skip DRAM calibration phases")
parser.add_argument("--skip-cxl", action="store_true",
                    help="skip CXL calibration phases")
args = parser.parse_args()

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
cxl_dram = DIMM_DDR5_4400(size=args.cxl_size)
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

# Widen the IO bus so it is not the bottleneck for the DMA copy (see
# x86-cxl-checkpoint.py).
board.iobus.width = 32

# One synthetic GPU DMA source per calibration phase; separate sources keep
# the bytesWritten/bytesRead completion counters independent across phases.
max_out = args.cxl_rsp_size * 4
board.dram_write = PyTrafficGen(progress_check="1000000s",
                                max_outstanding_reqs=max_out)
board.dram_write.port = board.iobus.cpu_side_ports
board.dram_read = PyTrafficGen(progress_check="1000000s",
                               max_outstanding_reqs=max_out)
board.dram_read.port = board.iobus.cpu_side_ports
board.cxl_write = PyTrafficGen(progress_check="1000000s",
                               max_outstanding_reqs=max_out)
board.cxl_write.port = board.iobus.cpu_side_ports
board.cxl_read = PyTrafficGen(progress_check="1000000s",
                              max_outstanding_reqs=max_out)
board.cxl_read.port = board.iobus.cpu_side_ports

m5.ticks.fixGlobalFrequency()

copy_bytes = args.gpu_copy_mib * 1024 * 1024
block_size = args.gpu_request_size

CXL_BASE = 0x100000000
cxl_size = cxl_dram.get_size()
if copy_bytes > cxl_size:
    raise ValueError("gpu-copy-mib exceeds CXL size")

# DRAM calibration target: 2 GiB..2 GiB+copy, within the 3 GiB main memory.
dram_start = 0x80000000
dram_end = dram_start + copy_bytes
if dram_end > 0xC0000000:
    raise ValueError("gpu-copy-mib exceeds main-memory capacity")

# CXL calibration target: top of the CXL range.
cxl_start = CXL_BASE + cxl_size - copy_bytes
cxl_end = cxl_start + copy_bytes

# Hand the *entire* CXL memory to Linux (no test reserve). The E820 type-20
# entry for CXL is widened to the full CXL size so that the kernel onlines all
# of it as a System RAM NUMA node (this kernel auto-enables type-20 CXL memory;
# the ``dax_kmem`` module path is equivalent on upstream kernels).
for e in board.workload.e820_table.entries:
    if int(e.addr) == CXL_BASE and int(e.range_type) == 20:
        e.size = f"{cxl_size}B"
        break

offered_rate = toMemoryBandwidth(args.gpu_offered_bw)
period = max(1, fromSeconds(block_size / offered_rate))

command = (
    "modprobe dax_kmem 2>/dev/null;"  # online CXL as System RAM (auto-enabled
                                      # by this kernel if module is absent)
    + "numactl -H;"                   # show NUMA topology (CXL = node 1)
    + "m5 exit;"                      # switch KVM -> Timing
    + "while :; do :; done;"          # keep the guest alive during calibration
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path="/root/simcxl-resources/vmlinux"),
    disk_image=DiskImageResource(local_path="/root/simcxl-resources/parsec.img"),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)

TICK_PER_SEC = 1e12
POLL_TICKS = int(TICK_PER_SEC * 1e-6)  # poll every 1 us

phases = []
if not args.skip_dram:
    phases.append(("GPU->DRAM write", board.dram_write,
                   dram_start, dram_end, 0, "bytesWritten"))
    phases.append(("DRAM->GPU read", board.dram_read,
                   dram_start, dram_end, 100, "bytesRead"))
if not args.skip_cxl:
    phases.append(("GPU->CXL write", board.cxl_write,
                   cxl_start, cxl_end, 0, "bytesWritten"))
    phases.append(("CXL->GPU read", board.cxl_read,
                   cxl_start, cxl_end, 100, "bytesRead"))

if not phases:
    raise ValueError("both --skip-dram and --skip-cxl given: nothing to run")

_t_start = [0]


def handle_exit():
    processor.switch()
    m5.stats.reset()

    results = []
    for name, gen, start, end, read_percent, stat in phases:
        _t_start[0] = m5.curTick()

        state = gen.createLinear(
            m5.MaxTick, start, end, block_size,
            period, period, read_percent, copy_bytes)
        exit_state = gen.createExit(0)
        gen.start([state, exit_state])
        yield False  # wait for injection to finish (ExitGen)

        while int(gen.resolveStat(stat).value) < copy_bytes:
            m5.scheduleTickExitFromCurrent(POLL_TICKS)
            yield False
        assert int(gen.resolveStat(stat).value) == copy_bytes

        elapsed = (m5.curTick() - _t_start[0]) / TICK_PER_SEC
        bw = copy_bytes / elapsed / 1e9
        op = "write" if read_percent == 0 else "read"
        print(f"[{name}] {copy_bytes} bytes in {elapsed:.6f}s = {bw:.2f} GB/s "
              f"({op})")
        results.append((name, bw))

    print("---- P0 baseline summary ----")
    for name, bw in results:
        print(f"  {name:20s} {bw:.2f} GB/s")

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

print("Running SimCXL AI checkpoint P0 baseline (Ruby MESI Two Level)...")
print(f"  copy = {args.gpu_copy_mib} MiB, request size = {block_size} B, "
      f"CXL = {args.cxl_size} (all handed to guest)")
m5.stats.reset()
simulator.run()
