/*
 * Copyright (c) 2026
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

#include "dev/storage/pcie_link.hh"

#include "base/trace.hh"
#include "debug/PcieLink.hh"
#include "params/PcieLink.hh"

namespace gem5
{

namespace
{

// Serialization time per payload byte, in ticks (1 tick = 1 ps).
// Models the real PCIe physical/protocol overhead:
//   data rate = lineRateGtps * numLanes * 128/130        (128b/130b encoding)
//   each TLP carries maxPayloadBytes of payload plus tlpOverheadBytes of
//   header/LCRC/framing/DLLP, so the payload efficiency is
//   maxPayload / (maxPayload + overhead) and the per-payload-byte time is
//   scaled up by (1 + overhead/maxPayload).
double
computeTickPerByte(double lineRateGtps, unsigned int numLanes,
                   unsigned int tlpOverheadBytes,
                   unsigned int maxPayloadBytes)
{
    const double dataRateGbps = lineRateGtps * numLanes * 128.0 / 130.0;
    const double overheadRatio =
        (double)tlpOverheadBytes / (double)maxPayloadBytes;
    return (8000.0 / dataRateGbps) * (1.0 + overheadRatio); // ticks per byte
}

} // anonymous namespace

PcieLink::LinkResponsePort::LinkResponsePort(
    const std::string &name, PcieLink &link, LinkRequestPort &memPort,
    Tick delay, double tickPerByte, unsigned int respLimit,
    const std::vector<AddrRange> &_ranges)
    : ResponsePort(name), link(link), memPort(memPort), delay(delay),
      tickPerByte(tickPerByte), respLimit(respLimit),
      outstandingResponses(0), retryReq(false),
      sendEvent([this] { trySendTiming(); }, name),
      ranges(_ranges.begin(), _ranges.end())
{
}

PcieLink::LinkRequestPort::LinkRequestPort(
    const std::string &name, PcieLink &link, LinkResponsePort &cpuPort,
    Tick delay, double tickPerByte, unsigned int reqLimit)
    : RequestPort(name), link(link), cpuPort(cpuPort), delay(delay),
      tickPerByte(tickPerByte), reqLimit(reqLimit),
      sendEvent([this] { trySendTiming(); }, name)
{
}

PcieLink::PcieLink(const PcieLinkParams &p)
    : ClockedObject(p),
      cpuSidePort(p.name + ".cpu_side_port", *this, memSidePort, p.delay,
                  computeTickPerByte(p.line_rate_gtps, p.num_lanes,
                                     p.tlp_overhead_bytes,
                                     p.max_payload_bytes),
                  p.resp_size, p.ranges),
      memSidePort(p.name + ".mem_side_port", *this, cpuSidePort, p.delay,
                  computeTickPerByte(p.line_rate_gtps, p.num_lanes,
                                     p.tlp_overhead_bytes,
                                     p.max_payload_bytes),
                  p.req_size)
{
}

Port &
PcieLink::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side_port")
        return memSidePort;
    else if (if_name == "cpu_side_port")
        return cpuSidePort;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
PcieLink::init()
{
    if (!cpuSidePort.isConnected() || !memSidePort.isConnected())
        fatal("Both ports of a PcieLink must be connected.\n");
    cpuSidePort.sendRangeChange();
}

bool
PcieLink::LinkResponsePort::respQueueFull() const
{
    return outstandingResponses == respLimit;
}

bool
PcieLink::LinkRequestPort::reqQueueFull() const
{
    return transmitList.size() == reqLimit;
}

bool
PcieLink::LinkRequestPort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(PcieLink, "PcieLink recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    pkt->headerDelay = pkt->payloadDelay = 0;

    // Responses (write ACKs) are NOT bandwidth-throttled; they only incur the
    // fixed link latency, so the effective write bandwidth is exactly
    // bandwidth * efficiency.
    Tick t = curTick() + delay;
    cpuPort.schedTimingResp(pkt, t);

    return true;
}

bool
PcieLink::LinkResponsePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(PcieLink, "PcieLink recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    assert(!retryReq);

    if (memPort.reqQueueFull()) {
        DPRINTF(PcieLink, "PcieLink request queue full\n");
        retryReq = true;
    } else if (!retryReq) {
        bool expects_response = pkt->needsResponse() &&
            !pkt->cacheResponding();
        if (expects_response) {
            if (respQueueFull()) {
                DPRINTF(PcieLink, "PcieLink response queue full\n");
                retryReq = true;
            } else {
                assert(outstandingResponses != respLimit);
                ++outstandingResponses;
            }
        }

        if (!retryReq) {
            pkt->headerDelay = pkt->payloadDelay = 0;

            // Serialize the write data at `bandwidth * efficiency`.
            Tick serialization = (Tick)(pkt->getSize() * tickPerByte);
            Tick t = curTick() + delay + serialization;
            memPort.schedTimingReq(pkt, t);
        }
    }

    return !retryReq;
}

void
PcieLink::LinkRequestPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    if (transmitList.empty())
        link.schedule(sendEvent, when);

    assert(transmitList.size() != reqLimit);

    transmitList.emplace_back(pkt, when);
}

void
PcieLink::LinkResponsePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    if (transmitList.empty())
        link.schedule(sendEvent, when);

    transmitList.emplace_back(pkt, when);
}

void
PcieLink::LinkRequestPort::trySendTiming()
{
    assert(!transmitList.empty());

    auto req = transmitList.front();
    PacketPtr pkt = req.first;

    DPRINTF(PcieLink, "PcieLink trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        transmitList.pop_front();

        if (!transmitList.empty()) {
            // Space the next request by this packet's serialization time.
            Tick serialization = (Tick)(pkt->getSize() * tickPerByte);
            Tick t = curTick() + serialization;
            link.schedule(sendEvent, std::max(transmitList.front().second, t));
        }

        cpuPort.retryStalledReq();
    }
    // On send failure, wait for recvReqRetry().
}

void
PcieLink::LinkResponsePort::trySendTiming()
{
    assert(!transmitList.empty());

    auto resp = transmitList.front();
    PacketPtr pkt = resp.first;

    DPRINTF(PcieLink, "PcieLink trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        transmitList.pop_front();

        assert(outstandingResponses != 0);
        --outstandingResponses;

        if (!transmitList.empty())
            link.schedule(sendEvent, transmitList.front().second);

        if (!memPort.reqQueueFull() && retryReq) {
            retryReq = false;
            sendRetryReq();
        }
    }
    // On send failure, wait for recvRespRetry().
}

void
PcieLink::LinkResponsePort::retryStalledReq()
{
    if (retryReq) {
        retryReq = false;
        sendRetryReq();
    }
}

void
PcieLink::LinkRequestPort::recvReqRetry()
{
    trySendTiming();
}

void
PcieLink::LinkResponsePort::recvRespRetry()
{
    trySendTiming();
}

Tick
PcieLink::LinkResponsePort::recvAtomic(PacketPtr pkt)
{
    return delay + memPort.sendAtomic(pkt);
}

void
PcieLink::LinkResponsePort::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(link.name());

    for (auto &entry : transmitList) {
        if (pkt->trySatisfyFunctional(entry.first)) {
            pkt->makeResponse();
            return;
        }
    }

    if (memPort.trySatisfyFunctional(pkt))
        return;

    pkt->popLabel();

    memPort.sendFunctional(pkt);
}

bool
PcieLink::LinkRequestPort::trySatisfyFunctional(PacketPtr pkt)
{
    bool found = false;
    auto i = transmitList.begin();
    while (i != transmitList.end() && !found) {
        if (pkt->trySatisfyFunctional((*i).first)) {
            pkt->makeResponse();
            found = true;
        }
        ++i;
    }
    return found;
}

AddrRangeList
PcieLink::LinkResponsePort::getAddrRanges() const
{
    return ranges;
}

} // namespace gem5
