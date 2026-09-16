# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.

"""
GPU checkpoint save -> restore workflow (Ruby MESI Two Level).

Application-level checkpoint/restore over a CXL Type-3 memory expander:

  save    (checkpoint): GPU writes its state into CXL memory
                       (CXL.mem write direction).
  restore (recovery) : GPU reads its state back from CXL memory
                       (CXL.mem read direction).

Both phases run back-to-back in a single simulation and are timed
independently. Each phase waits for all responses to drain
(bytesWritten / bytesRead == copy_bytes) before measuring, so the
reported bandwidth is end-to-end throughput, not injection throughput.

This models only the host-side path after the PCIe TLP has been
terminated and converted to cache-line (64 B) memory transactions by
the Root Complex / CXL Host Bridge. The GPU PCIe packetizer, PCIe link,
and TLP termination/translation stages are NOT modelled here; see
readme_cxl.md for the protocol-path notes.
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

parser = argparse.ArgumentParser(
    description='CXL checkpoint save/restore (Ruby).')
parser.add_argument('--gpu-copy-mib', type=int, default=512)
parser.add_argument('--gpu-offered-bw', type=str, default="32GB/s")
parser.add_argument('--gpu-request-size', type=int, choices=[64, 128, 256],
                    default=64)
parser.add_argument('--cxl-test-reserve', type=str, default=None)
parser.add_argument('--cxl-rsp-size', type=int, default=48)
parser.add_argument('--cxl-proto-lat', type=str, default="15ns")
parser.add_argument('--cxl-addr-map', type=str, default="RoRaBaCoCh")
parser.add_argument('--cxl-tcl', type=str, default=None)
args = parser.parse_args()

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

# Two synthetic GPU traffic sources: one for checkpoint save (write) and one
# for restore (read). max_outstanding_reqs bounds in-flight requests so the
# completion-poll below gives true end-to-end timing.
board.gpu_save = PyTrafficGen(progress_check="1000000s",
                              max_outstanding_reqs=args.cxl_rsp_size * 4)
board.gpu_save.port = board.iobus.cpu_side_ports
board.gpu_restore = PyTrafficGen(progress_check="1000000s",
                                 max_outstanding_reqs=args.cxl_rsp_size * 4)
board.gpu_restore.port = board.iobus.cpu_side_ports

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

# x86_board.py's CXL E820 entry reserves only the top 64 MiB by default;
# widen it to the actual reserve so Linux does not use the test region.
cxl_linux_visible = cxl_size - reserve_size
for e in board.workload.e820_table.entries:
    if int(e.addr) == CXL_BASE and int(e.range_type) == 20:
        e.size = f"{cxl_linux_visible}B"
        break

offered_rate = toMemoryBandwidth(args.gpu_offered_bw)
period = max(1, fromSeconds(block_size / offered_rate))

command = "m5 exit;"

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/root/simcxl-resources/vmlinux'),
    disk_image=DiskImageResource(local_path='/root/simcxl-resources/parsec.img'),
    readfile_contents=command,
    kernel_args=board.get_default_kernel_args() + ["idle=nomwait"],
)

TICK_PER_SEC = 1e12
POLL_TICKS = int(TICK_PER_SEC * 1e-6)  # poll every 1 us

_phase = [0]
_t_start = [0]


def _wait_done(gen, stat, target):
    """Yield False (continue simulating) until `stat` reaches `target`."""
    while int(gen.resolveStat(stat).value) < target:
        m5.scheduleTickExitFromCurrent(POLL_TICKS)
        yield False


def handle_exit():
    if _phase[0] == 0:
        # boot exit: switch CPU, reset stats, start the checkpoint save (write)
        processor.switch()
        m5.stats.reset()
        _t_start[0] = m5.curTick()

        save_state = board.gpu_save.createLinear(
            m5.MaxTick, cxl_test_start, cxl_test_end, block_size,
            period, period, 0, copy_bytes)   # 0% read = all write
        save_exit = board.gpu_save.createExit(0)
        board.gpu_save.start([save_state, save_exit])
        _phase[0] = 1
        yield False

    if _phase[0] == 1:
        # save injection finished; wait for all writes to land in CXL
        yield from _wait_done(board.gpu_save, "bytesWritten", copy_bytes)
        assert int(board.gpu_save.resolveStat("bytesWritten").value) == copy_bytes

        elapsed = (m5.curTick() - _t_start[0]) / TICK_PER_SEC
        print(f"[checkpoint save  GPU->CXL] {copy_bytes} bytes in "
              f"{elapsed:.6f}s = {copy_bytes / elapsed / 1e9:.2f} GB/s (write)")

        # start the restore (read)
        m5.stats.reset()
        _t_start[0] = m5.curTick()
        restore_state = board.gpu_restore.createLinear(
            m5.MaxTick, cxl_test_start, cxl_test_end, block_size,
            period, period, 100, copy_bytes)  # 100% read
        restore_exit = board.gpu_restore.createExit(0)
        board.gpu_restore.start([restore_state, restore_exit])
        _phase[0] = 2
        yield False

    if _phase[0] == 2:
        # restore injection finished; wait for all reads to return
        yield from _wait_done(board.gpu_restore, "bytesRead", copy_bytes)
        assert int(board.gpu_restore.resolveStat("bytesRead").value) == copy_bytes

        elapsed = (m5.curTick() - _t_start[0]) / TICK_PER_SEC
        print(f"[checkpoint restore CXL->GPU] {copy_bytes} bytes in "
              f"{elapsed:.6f}s = {copy_bytes / elapsed / 1e9:.2f} GB/s (read)")

        m5.stats.dump()
        _phase[0] = 3
        yield True


_handler = handle_exit()
simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: _handler,
        ExitEvent.SCHEDULED_TICK: _handler,
    },
)

print("Running CXL checkpoint save/restore (Ruby MESI Two Level)...")
m5.stats.reset()
simulator.run()
