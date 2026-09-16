# Copyright (c) 2021 The Regents of The University of California
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

from m5.params import *
from m5.objects.PciDevice import PciDevice, PciMemBar


class SimCkptDevice(PciDevice):
    """AI-checkpoint DMA engine.

    A PCI device exposing an NVMe-like submission/completion ring pair in host
    memory plus a doorbell interface in BAR0. The device reads a source buffer,
    computes a CRC32 over the payload, writes the payload to a destination
    buffer, and only then posts a completion (optionally raising an interrupt).
    """

    type = "SimCkptDevice"
    cxx_header = "dev/storage/sim_ckpt_device.hh"
    cxx_class = "gem5::SimCkptDevice"

    queue_depth = Param.Unsigned(
        8, "Maximum number of descriptors processed concurrently"
    )
    max_chunk_size = Param.MemorySize(
        "4KiB", "Maximum payload size of a single descriptor"
    )
    proc_lat = Param.Latency(
        "15ns", "Per-descriptor processing latency before DMA issue"
    )

    VendorID = 0x8086
    DeviceID = 0x9090
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x11
    InterruptPin = 0x01

    BAR0 = PciMemBar(size="64KiB")
