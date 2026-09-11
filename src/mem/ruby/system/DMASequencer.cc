/*
 * Copyright (c) 2021 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2008 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/system/DMASequencer.hh"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "debug/RubyDma.hh"
#include "debug/RubyStats.hh"
#include "mem/ruby/protocol/SequencerMsg.hh"
#include "mem/ruby/protocol/SequencerRequestType.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

DMARequest::DMARequest(uint64_t start_paddr, int len, bool write,
                       int bytes_completed, int bytes_issued, uint8_t *data,
                       PacketPtr pkt)
    : start_paddr(start_paddr), len(len), write(write),
      bytes_completed(bytes_completed), bytes_issued(bytes_issued), data(data),
      pkt(pkt)
{
}

DMASequencer::DMASequencer(const Params &p)
    : RubyPort(p), m_outstanding_count(0),
      m_max_outstanding_requests(p.max_outstanding_requests)
{
}

void
DMASequencer::init()
{
    RubyPort::init();
    m_data_block_mask = mask(RubySystem::getBlockSizeBits());
    m_data_block_size = RubySystem::getBlockSizeBytes();
}

RequestStatus
DMASequencer::makeRequest(PacketPtr pkt)
{
    if (m_outstanding_count == m_max_outstanding_requests) {
        return RequestStatus_BufferFull;
    }

    Addr paddr = pkt->getAddr();
    uint8_t* data =  pkt->getPtr<uint8_t>();
    int len = pkt->getSize();
    bool write = pkt->isWrite();

    // Should DMA be allowed to generate this ?
    assert(!pkt->isMaskedWrite());

    assert(m_outstanding_count < m_max_outstanding_requests);

    // A single DMA request may span several cache lines (e.g. a 256B PCIe MRd
    // that the Home Agent turns into 4x64B CXL.mem MemRd requests). Issue all
    // the cache-line sub-requests up-front so they can proceed in parallel,
    // and track them individually in m_RequestTable. The owning DMARequest is
    // shared across all the sub-requests; it completes once every cache line
    // has reported back.
    auto parent = std::make_shared<DMARequest>(paddr, len, write, 0, 0, data,
                                               pkt);

    // Atomic requests are only supported within a single cache line and are
    // not subject to the parallel multi-cache-line split used for LD/ST.
    if (pkt->req->isAtomic()) {
        assert(len <= m_data_block_size);
        Addr line_addr = makeLineAddress(paddr);
        int atomic_offset = paddr & m_data_block_mask;

        std::shared_ptr<SequencerMsg> msg =
            std::make_shared<SequencerMsg>(clockEdge());
        msg->getPhysicalAddress() = paddr;
        msg->getLineAddress() = line_addr;
        msg->setType(SequencerRequestType_ATOMIC);
        msg->getLen() = len;

        std::vector<bool> access_mask(m_data_block_size, false);
        for (int idx = 0; idx < len; ++idx) {
            access_mask[atomic_offset + idx] = true;
        }
        std::vector<std::pair<int, AtomicOpFunctor*>> atomic_ops;
        atomic_ops.emplace_back(atomic_offset, pkt->getAtomicOp());
        msg->getwriteMask().setAtomicOps(atomic_ops);

        m_RequestTable.emplace(line_addr,
                               DMALine{parent, 0, atomic_offset, len});
        m_mandatory_q_ptr->enqueue(msg, clockEdge(), cyclesToTicks(Cycles(1)));
        parent->bytes_issued = len;
        m_outstanding_count++;
        return RequestStatus_Issued;
    }

    int issued = 0;
    int block_offset = paddr & m_data_block_mask;
    Addr cur_paddr = paddr;
    while (issued < len) {
        int sub_len = std::min<int>(len - issued,
                                    m_data_block_size - block_offset);
        Addr line_addr = makeLineAddress(cur_paddr);

        // Conservative: only one outstanding sub-request per cache line.
        if (m_RequestTable.find(line_addr) != m_RequestTable.end()) {
            DPRINTF(RubyDma, "DMA aliased: addr %p, len %d\n",
                    line_addr, sub_len);
            m_RequestTable.clear();
            return RequestStatus_Aliased;
        }

        std::shared_ptr<SequencerMsg> msg =
            std::make_shared<SequencerMsg>(clockEdge());
        msg->getPhysicalAddress() = cur_paddr;
        msg->getLineAddress() = line_addr;
        msg->setType(write ? SequencerRequestType_ST : SequencerRequestType_LD);
        msg->getLen() = sub_len;

        if (write && (data != NULL)) {
            msg->getDataBlk().setData(&data[issued], block_offset, sub_len);
        }

        m_RequestTable.emplace(line_addr,
                               DMALine{parent, issued, block_offset, sub_len});

        DPRINTF(RubyDma, "DMA req created: addr %p, len %d\n",
                line_addr, sub_len);

        m_mandatory_q_ptr->enqueue(msg, clockEdge(), cyclesToTicks(Cycles(1)));

        issued += sub_len;
        parent->bytes_issued += sub_len;
        cur_paddr += sub_len;
        block_offset = 0;   // only the first cache line may be unaligned
    }

    m_outstanding_count++;
    return RequestStatus_Issued;
}

void
DMASequencer::dataCallback(const DataBlock & dblk, const Addr& address)
{
    RequestTable::iterator i = m_RequestTable.find(address);
    assert(i != m_RequestTable.end());

    DMALine line = i->second;
    std::shared_ptr<DMARequest> active_request = line.parent;
    m_RequestTable.erase(i);

    assert(!active_request->write);
    if (active_request->data != NULL) {
        memcpy(&active_request->data[line.data_offset],
               dblk.getData(line.block_offset, line.len), line.len);
    }

    active_request->bytes_completed += line.len;
    if (active_request->bytes_completed == active_request->len) {
        DPRINTF(RubyDma, "DMA request completed: addr %p, size %d\n",
                address, active_request->len);
        m_outstanding_count--;
        PacketPtr pkt = active_request->pkt;
        ruby_hit_callback(pkt);
    }
}

void
DMASequencer::ackCallback(const Addr& address)
{
    RequestTable::iterator i = m_RequestTable.find(address);
    assert(i != m_RequestTable.end());

    DMALine line = i->second;
    std::shared_ptr<DMARequest> active_request = line.parent;
    m_RequestTable.erase(i);

    assert(active_request->write);
    active_request->bytes_completed += line.len;
    if (active_request->bytes_completed == active_request->len) {
        DPRINTF(RubyDma, "DMA write completed: addr %p, size %d\n",
                address, active_request->len);
        m_outstanding_count--;
        PacketPtr pkt = active_request->pkt;
        ruby_hit_callback(pkt);
    }
}

void
DMASequencer::atomicCallback(const DataBlock& dblk, const Addr& address)
{
    RequestTable::iterator i = m_RequestTable.find(address);
    assert(i != m_RequestTable.end());

    DMALine line = i->second;
    std::shared_ptr<DMARequest> active_request = line.parent;
    m_RequestTable.erase(i);

    PacketPtr pkt = active_request->pkt;

    int offset = active_request->start_paddr & m_data_block_mask;
    memcpy(pkt->getPtr<uint8_t>(), dblk.getData(offset, pkt->getSize()),
           pkt->getSize());

    ruby_hit_callback(pkt);

    m_outstanding_count--;
}

void
DMASequencer::recordRequestType(DMASequencerRequestType requestType)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            DMASequencerRequestType_to_string(requestType));
}

} // namespace ruby
} // namespace gem5
