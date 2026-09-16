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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class ParallelStorage(ClockedObject):
    """Multi-channel flash-style backing store for AI checkpoints.

    Presents a single logical checkpoint namespace, striped across several
    independent channels. Each channel has its own byte-addressable backing
    store, read/write bandwidth, base latency, queue depth (outstanding
    limit) and is serviced independently, so aggregate bandwidth scales with
    the number of channels.

    Stripping maps a logical byte offset to a channel:

        chunk_id      = offset // chunk_size
        channel       = chunk_id % num_channels
        channel_off   = (chunk_id // num_channels) * chunk_size + offset % chunk_size
    """

    type = "ParallelStorage"
    cxx_header = "dev/storage/parallel_storage.hh"
    cxx_class = "gem5::ParallelStorage"

    port = ResponsePort("This port sends responses and receives requests")

    size = Param.MemorySize("64MiB", "Total logical checkpoint namespace size")
    num_channels = Param.Unsigned(2, "Number of independent storage channels")
    chunk_size = Param.MemorySize("4KiB", "Striping chunk size")
    read_bw = Param.MemoryBandwidth("7GB/s", "Per-channel read bandwidth")
    write_bw = Param.MemoryBandwidth("7GB/s", "Per-channel write bandwidth")
    latency = Param.Latency("10us", "Per-channel base access latency")
    queue_depth = Param.Unsigned(
        32, "Per-channel maximum number of outstanding requests"
    )
