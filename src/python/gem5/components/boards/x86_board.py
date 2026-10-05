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


from typing import (
    List,
    Sequence,
)

from m5.objects import (
    Addr,
    AddrRange,
    BaseXBar,
    Bridge,
    CowDiskImage,
    CXLBridge,
    CXLMemCtrl,
    IdeDisk,
    IOXBar,
    NoncoherentXBar,
    ParallelStorage,
    Pc,
    PcieLink,
    Port,
    RawDiskImage,
    SimCkptDevice,
    X86E820Entry,
    X86FsLinux,
    X86IntelMPBus,
    X86IntelMPBusHierarchy,
    X86IntelMPIOAPIC,
    X86IntelMPIOIntAssignment,
    X86IntelMPProcessor,
    X86SMBiosBiosInformation,
)
from m5.params import Latency
from m5.util.convert import toMemorySize

from ...isas import ISA
from ...resources.resource import AbstractResource
from ...utils.override import overrides
from ..cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy
from ..memory.abstract_memory_system import AbstractMemorySystem
from ..processors.abstract_processor import AbstractProcessor
from .abstract_system_board import AbstractSystemBoard
from .kernel_disk_workload import KernelDiskWorkload


class X86Board(AbstractSystemBoard, KernelDiskWorkload):
    """
    A board capable of full system simulation for X86.

    **Limitations**
    * Currently, this board's memory is hardcoded to 3GB.
    * Much of the I/O subsystem is hard coded.
    """

    def __init__(
        self,
        clk_freq: str,
        processor: AbstractProcessor,
        memory: AbstractMemorySystem,
        cache_hierarchy: AbstractCacheHierarchy,
        cxl_memory: AbstractMemorySystem,
        is_asic: bool = True,
        add_simckpt_device: bool = False,
        simckpt_s2_topology: bool = False,
        simckpt_num_queues: int = 2,
        simckpt_num_dram_lanes: int = 0,
        simckpt_num_cxl_lanes: int = 0,
    ) -> None:
        self._cxl_memory_ptr = cxl_memory
        self._is_asic = is_asic
        self._add_simckpt_device = add_simckpt_device
        self._simckpt_s2 = simckpt_s2_topology
        self._simckpt_num_queues = simckpt_num_queues
        self._simckpt_num_dram_lanes = simckpt_num_dram_lanes
        self._simckpt_num_cxl_lanes = simckpt_num_cxl_lanes

        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )
        self.cxl_memory = self._cxl_memory_ptr

        if self.get_processor().get_isa() != ISA.X86:
            raise Exception(
                "The X86Board requires a processor using the X86 "
                f"ISA. Current processor ISA: '{processor.get_isa().name}'."
            )

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        self.pc = Pc()
        # cxl_device is dynamically initialized and attached
        self.pc.south_bridge.cxl_device = CXLMemCtrl(
            pci_func=0, pci_dev=6, pci_bus=0
        )
        if self._add_simckpt_device:
            self.pc.south_bridge.simckpt_device = SimCkptDevice(
                pci_func=0, pci_dev=7, pci_bus=0
            )
            self.pc.south_bridge.simckpt_device.num_queues = (
                self._simckpt_num_queues
            )
            self.simckpt_storage = ParallelStorage(
                size="64MiB", num_channels=2, chunk_size="4KiB", queue_depth=64
            )
            self.pc.south_bridge.simckpt_device.storage_port = (
                self.simckpt_storage.port
            )
            # Two independent PCIe lines (one per DMA lane): the DRAM lane (q0)
            # and the CXL lane (q1) each get their own bandwidth-limited
            # PcieLink, so they no longer contend on a single switch uplink
            # (target2 E0). Each link models the real PCIe physical layer:
            # 128b/130b encoding + TLP header/LCRC/framing/DLLP overhead on the
            # write-data direction (NO CXL.mem 68B flit accounting: the CXL line
            # is pure TLP, same formula as the DRAM line); responses/ACKs pass.
            #
            # Effective Gen5 x16 = 32 GT/s x16 x128/130 x 256/(256+35)
            #                  = 63.02 GB/s x 0.8797 ~= 55.5 GB/s  (target 55.53)
            num_queues = self._simckpt_num_queues
            self._simckpt_pcie_links = []
            self._simckpt_cxl_links = []
            self._simckpt_uplink = None
            self._simckpt_uplink_xbar = None

            def _mk_link(line_rate_gtps, init_cmd_latency="0ns"):
                return PcieLink(
                    req_size=1024,
                    resp_size=1024,
                    line_rate_gtps=line_rate_gtps,  # Gen5=32, Gen4=16
                    num_lanes=16,  # x16
                    max_payload_bytes=256,  # device MaxPayloadSize
                    tlp_overhead_bytes=35,  # header(16)+LCRC(4)+framing/DLLP
                    delay="0ns",
                    init_cmd_latency=init_cmd_latency,
                )

            if self._simckpt_s2:
                # --- S2 topology (target2 E5): 4 GPU + shared switch uplink ---
                # 4 GPU lanes each get their own Gen4 x16 link (27.77 GB/s,
                # the per-GPU egress limit). Their DRAM-bound traffic fans in
                # through a NoncoherentXBar into a single shared Gen5 x16
                # switch uplink (55.53 GB/s) -> this is the multi-GPU shared
                # uplink that becomes the bottleneck for DRAM-only staging.
                # 2 CXL lanes each get a direct Gen5 x16 expander link (P2P,
                # does NOT traverse the shared uplink) -> the on-device DMA
                # data plane, which releases the uplink in dual-path mode.
                n_dram = self._simckpt_num_dram_lanes or 4
                n_cxl = self._simckpt_num_cxl_lanes or 2
                assert n_dram + n_cxl == num_queues, (
                    "S2 topology: num_dram_lanes + num_cxl_lanes must equal "
                    "SimCkptDevice.num_queues"
                )
                dram_links = []
                for i in range(n_dram):
                    link = _mk_link(16)  # Gen4 x16 per-GPU link
                    dram_links.append(link)
                    self._simckpt_pcie_links.append(link)
                    setattr(self, f"simckpt_pcie_link{i}", link)
                    self.pc.south_bridge.simckpt_device.dma[
                        i
                    ] = link.cpu_side_port
                # Shared Gen5 x16 switch uplink with a NoncoherentXBar fan-in
                # of the 4 GPU DRAM lanes.
                uplink = _mk_link(32)  # Gen5 x16 switch uplink
                self._simckpt_uplink = uplink
                setattr(self, "simckpt_uplink", uplink)
                xbar = NoncoherentXBar(
                    width=64,
                    frontend_latency=1,
                    forward_latency=0,
                    response_latency=1,
                    header_latency=1,
                )
                self._simckpt_uplink_xbar = xbar
                setattr(self, "simckpt_uplink_xbar", xbar)
                for link in dram_links:
                    xbar.cpu_side_ports = link.mem_side_port
                xbar.mem_side_ports = uplink.cpu_side_port
                # 2 CXL P2P lanes (direct Gen5 x16, on-device DMA, no uplink).
                for i in range(n_cxl):
                    # The CXL lane carries a one-time CXL.io initial-command
                    # latency (CPU->expander discovery/config/descriptor/
                    # doorbell) before the data plane starts.
                    link = _mk_link(32, init_cmd_latency="1us")
                    self._simckpt_pcie_links.append(link)
                    self._simckpt_cxl_links.append(link)
                    qi = n_dram + i
                    setattr(self, f"simckpt_pcie_link{qi}", link)
                    self.pc.south_bridge.simckpt_device.dma[
                        qi
                    ] = link.cpu_side_port
            else:
                for i in range(num_queues):
                    link = _mk_link(32)  # Gen5 x16 per-lane link (v2.1)
                    self._simckpt_pcie_links.append(link)
                    setattr(self, f"simckpt_pcie_link{i}", link)
                    self.pc.south_bridge.simckpt_device.dma[
                        i
                    ] = link.cpu_side_port

        self.workload = X86FsLinux()

        # North Bridge
        self.iobus = IOXBar()

        # Set up all of the I/O.
        self._setup_io_devices()

        self.m5ops_base = 0xFFFF0000

    def _simckpt_ruby_dma_ports(self):
        """Mem-side ports of the PcieLink(s) that enter the Ruby memory system.

        In the default "independent" topology every per-lane link connects
        directly to a Ruby DMASequencer. In the S2 topology the 4 GPU DRAM
        lanes fan into the shared uplink first, so only the uplink's mem-side
        port (plus the 2 CXL P2P lanes) reach Ruby.
        """
        if self._simckpt_s2:
            return [self._simckpt_uplink.mem_side_port] + [
                link.mem_side_port for link in self._simckpt_cxl_links
            ]
        return [link.mem_side_port for link in self._simckpt_pcie_links]

    def _setup_io_devices(self):
        """Sets up the x86 IO devices.

        .. note::

            This is mostly copy-paste from prior X86 FS setups. Some of it
            may not be documented and there may be bugs.
        """

        # Constants similar to x86_traits.hh
        IO_address_space_base = 0x8000000000000000
        pci_config_address_space_base = 0xC000000000000000
        interrupts_address_space_base = 0xA000000000000000
        APIC_range_size = 1 << 12

        # Configure CXL Device
        cxl_dram = self._cxl_memory_ptr
        cxl_mem_range = AddrRange(Addr(0x100000000), size=cxl_dram.get_size())
        cxl_dram.set_memory_range([cxl_mem_range])
        cxl_mem_ctrl = self.pc.south_bridge.cxl_device
        cxl_mem_ctrl.connectMemory(cxl_mem_range, cxl_dram)
        cxl_abstract_mems = []
        for mc in cxl_dram.get_memory_controllers():
            cxl_abstract_mems.append(mc.dram)
        self.memories.extend(cxl_abstract_mems)

        if self._is_asic:
            cxl_mem_ctrl.configCXL(Latency("15ns"), 48)
        else:
            cxl_mem_ctrl.configCXL(Latency("60ns"), 36)

        # Setup memory system specific settings.
        if self.get_cache_hierarchy().is_ruby():
            dma_ports = [self.pc.south_bridge.ide.dma, cxl_mem_ctrl.dma]
            if hasattr(self.pc.south_bridge, "simckpt_device"):
                # Each lane's independent PCIe uplink (PcieLink) is the DMA
                # source for the Ruby system, not the per-queue device DMA ports.
                # In the S2 topology the DRAM lanes fan into the shared uplink
                # first, so only the uplink + CXL lanes reach Ruby.
                for port in self._simckpt_ruby_dma_ports():
                    dma_ports.append(port)
            self.pc.attachIO(self.get_io_bus(), dma_ports)
        else:
            # # Constants similar to x86_traits.hh
            IO_address_space_base = 0x8000000000000000
            pci_config_address_space_base = 0xC000000000000000
            interrupts_address_space_base = 0xA000000000000000
            APIC_range_size = 1 << 12

            # Configure CXLBridge
            self.bridge = CXLBridge(
                bridge_lat="50ns",
                proto_proc_lat="12ns",
                req_fifo_depth=128,
                resp_fifo_depth=128,
            )
            self.bridge.mem_side_port = self.get_io_bus().cpu_side_ports
            self.bridge.cpu_side_port = (
                self.get_cache_hierarchy().get_mem_side_port()
            )

            self.bridge.ranges = [
                AddrRange(0xC0000000, 0xFFFF0000),
                AddrRange(
                    IO_address_space_base, interrupts_address_space_base - 1
                ),
                AddrRange(pci_config_address_space_base, Addr.max),
                cxl_mem_range,
            ]

            self.apicbridge = Bridge(delay="50ns")
            self.apicbridge.cpu_side_port = self.get_io_bus().mem_side_ports
            self.apicbridge.mem_side_port = (
                self.get_cache_hierarchy().get_cpu_side_port()
            )
            self.apicbridge.ranges = [
                AddrRange(
                    interrupts_address_space_base,
                    interrupts_address_space_base
                    + self.get_processor().get_num_cores() * APIC_range_size
                    - 1,
                )
            ]
            self.pc.attachIO(self.get_io_bus())

        # Add in a Bios information structure.
        self.workload.smbios_table.structures = [X86SMBiosBiosInformation()]

        # Set up the Intel MP table
        base_entries = []
        ext_entries = []
        for i in range(self.get_processor().get_num_cores()):
            bp = X86IntelMPProcessor(
                local_apic_id=i,
                local_apic_version=0x14,
                enable=True,
                bootstrap=(i == 0),
            )
            base_entries.append(bp)
        io_apic = X86IntelMPIOAPIC(
            id=self.get_processor().get_num_cores(),
            version=0x11,
            enable=True,
            address=0xFEC00000,
        )

        self.pc.south_bridge.io_apic.apic_id = io_apic.id
        base_entries.append(io_apic)
        pci_bus = X86IntelMPBus(bus_id=0, bus_type="PCI   ")
        base_entries.append(pci_bus)
        isa_bus = X86IntelMPBus(bus_id=1, bus_type="ISA   ")
        base_entries.append(isa_bus)
        connect_busses = X86IntelMPBusHierarchy(
            bus_id=1, subtractive_decode=True, parent_bus=0
        )
        ext_entries.append(connect_busses)

        pci_dev4_inta = X86IntelMPIOIntAssignment(
            interrupt_type="INT",
            polarity="ConformPolarity",
            trigger="ConformTrigger",
            source_bus_id=0,
            source_bus_irq=0 + (4 << 2),
            dest_io_apic_id=io_apic.id,
            dest_io_apic_intin=16,
        )

        base_entries.append(pci_dev4_inta)

        def assignISAInt(irq, apicPin):
            assign_8259_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="ExtInt",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=0,
            )
            base_entries.append(assign_8259_to_apic)

            assign_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=apicPin,
            )
            base_entries.append(assign_to_apic)

        assignISAInt(0, 2)
        assignISAInt(1, 1)

        for i in range(3, 15):
            assignISAInt(i, i)

        self.workload.intel_mp_table.base_entries = base_entries
        self.workload.intel_mp_table.ext_entries = ext_entries

        entries = [
            # Mark the first megabyte of memory as reserved
            X86E820Entry(addr=0, size="639kB", range_type=1),
            X86E820Entry(addr=0x9FC00, size="385kB", range_type=2),
            # Mark the rest of physical memory as available
            X86E820Entry(
                addr=0x100000,
                size=f"{self.mem_ranges[0].size() - 0x100000:d}B",
                range_type=1,
            ),
        ]

        # Reserve the last 16kB of the 32-bit address space for m5ops
        entries.append(
            X86E820Entry(addr=0xFFFF0000, size="64kB", range_type=2)
        )

        gpu_test_reserve = toMemorySize("64MiB")

        cxl_linux_visible_size = cxl_mem_range.size() - gpu_test_reserve

        entries.append(
            X86E820Entry(
                addr=0x100000000,
                size=f"{cxl_linux_visible_size}B",
                range_type=20,
            )
        )

        self.workload.e820_table.entries = entries

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self) -> BaseXBar:
        return self.iobus

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> Sequence[Port]:
        ports = [
            self.pc.south_bridge.ide.dma,
            self.iobus.mem_side_ports,
            self.pc.south_bridge.cxl_device.dma,
        ]
        if hasattr(self.pc.south_bridge, "simckpt_device"):
            for port in self._simckpt_ruby_dma_ports():
                ports.append(port)
        return ports

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        return self.iobus.mem_side_ports

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self):
        memory = self.get_memory()

        if memory.get_size() > toMemorySize("3GB"):
            raise Exception(
                "X86Board currently only supports memory sizes up "
                "to 3GB because of the I/O hole."
            )
        data_range = AddrRange(memory.get_size())
        memory.set_memory_range([data_range])
        cpu_abstract_mems = []
        for mc in memory.get_memory_controllers():
            cpu_abstract_mems.append(mc.dram)
        self.memories = cpu_abstract_mems

        # Add the address range for the IO
        self.mem_ranges = [
            data_range,  # All data
            AddrRange(0xC0000000, size=0x100000),  # For I/0
        ]

    @overrides(KernelDiskWorkload)
    def get_disk_device(self):
        return "/dev/sda1"

    @overrides(KernelDiskWorkload)
    def _add_disk_to_board(self, disk_image: AbstractResource):
        ide_disk = IdeDisk()
        ide_disk.driveID = "device0"
        ide_disk.image = CowDiskImage(
            child=RawDiskImage(read_only=True), read_only=False
        )
        ide_disk.image.child.image_file = disk_image.get_local_path()

        # Attach the SimObject to the system.
        self.pc.south_bridge.ide.disks = [ide_disk]

    @overrides(KernelDiskWorkload)
    def get_default_kernel_args(self) -> List[str]:
        return [
            "earlyprintk=ttyS0",
            "console=ttyS0",
            "lpj=7999923",
            "root={root_value}",
            "disk_device={disk_device}",
        ]
