# Copyright (c) 2026
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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *


# PcieLink: a unidirectional bandwidth-limited link modeling a single
# GPU/device PCIe/CXL uplink to the root complex.
#
# Models the real PCIe physical/protocol overheads directly (no hand-waved
# "efficiency" factor):
#
#   data rate (Gb/s) = line_rate_gtps * num_lanes * 128/130   # 128b/130b
#   serialization time per request = (tlp_overhead_bytes + payload) / data_rate
#
# `tlp_overhead_bytes` folds in the TLP header (16 B for 64-bit addressing),
# LCRC (4 B), framing (STP/END ~2 B) and an amortised DLLP/ACK share.
#
# Only the request (write-data) direction is serialized; responses pass
# through with a fixed `delay` (they do not consume payload bandwidth). The
# `req_size`/`resp_size` fields are the credit-based flow-control depths
# (queue depth), which bound outstanding TLPs and thus the achievable
# throughput via Little's Law.
class PcieLink(ClockedObject):
    type = "PcieLink"
    cxx_header = "dev/storage/pcie_link.hh"
    cxx_class = "gem5::PcieLink"

    mem_side_port = RequestPort(
        "This port sends requests to and receives responses from memory"
    )
    master = DeprecatedParam(
        mem_side_port, "`master` is now called `mem_side_port`"
    )
    cpu_side_port = ResponsePort(
        "This port receives requests from and sends responses to the device"
    )
    slave = DeprecatedParam(
        cpu_side_port, "`slave` is now called `cpu_side_port`"
    )

    req_size = Param.Unsigned(1024, "Request queue depth (credits)")
    resp_size = Param.Unsigned(1024, "Response queue depth (credits)")
    delay = Param.Latency("0ns", "Fixed latency of the link")
    ranges = VectorParam.AddrRange(
        [AllMemory], "Address ranges to pass through the link"
    )

    line_rate_gtps = Param.Float(
        32, "PCIe per-lane line rate in GT/s (Gen5=32, Gen4=16, Gen3=8)"
    )
    num_lanes = Param.Unsigned(16, "Number of lanes (x16)")
    max_payload_bytes = Param.Unsigned(
        256, "PCIe MaxPayloadSize in bytes (device MaxPayload, e.g. 256)"
    )
    tlp_overhead_bytes = Param.Unsigned(
        24,
        "Per-TLP overhead (header + LCRC + framing + DLLP), amortised "
        "over max_payload_bytes",
    )
