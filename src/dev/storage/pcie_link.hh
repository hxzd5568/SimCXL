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

#ifndef __DEV_STORAGE_PCIE_LINK_HH__
#define __DEV_STORAGE_PCIE_LINK_HH__

#include <deque>
#include <utility>
#include <vector>

#include "base/addr_range.hh"
#include "mem/port.hh"
#include "params/PcieLink.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

/**
 * PcieLink: a unidirectional bandwidth-limited link modeling a single
 * GPU/device PCIe/CXL uplink. Only the request (write-data) direction is
 * throttled to `bandwidth * efficiency`; responses pass through with a fixed
 * latency. See PcieLink.py.
 */
class PcieLink : public ClockedObject
{
  protected:
    class LinkRequestPort;

    /** Port on the device side: receives requests, sends responses. */
    class LinkResponsePort : public ResponsePort
    {
      private:
        PcieLink &link;
        LinkRequestPort &memPort;
        const Tick delay;
        const double tickPerByte;
        const unsigned int respLimit;

        std::deque<std::pair<PacketPtr, Tick>> transmitList;
        unsigned int outstandingResponses;
        bool retryReq;
        EventFunctionWrapper sendEvent;
        AddrRangeList ranges;

        bool respQueueFull() const;
        void trySendTiming();

      public:
        LinkResponsePort(const std::string &name, PcieLink &link,
                         LinkRequestPort &memPort, Tick delay,
                         double tickPerByte, unsigned int respLimit,
                         const std::vector<AddrRange> &_ranges);

        void schedTimingResp(PacketPtr pkt, Tick when);
        void retryStalledReq();

        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
    };

    /** Port on the memory side: sends requests, receives responses. */
    class LinkRequestPort : public RequestPort
    {
      private:
        PcieLink &link;
        LinkResponsePort &cpuPort;
        const Tick delay;
        const double tickPerByte;
        const unsigned int reqLimit;

        std::deque<std::pair<PacketPtr, Tick>> transmitList;
        EventFunctionWrapper sendEvent;

        void trySendTiming();

      public:
        LinkRequestPort(const std::string &name, PcieLink &link,
                        LinkResponsePort &cpuPort, Tick delay,
                        double tickPerByte, unsigned int reqLimit);

        void schedTimingReq(PacketPtr pkt, Tick when);

        bool reqQueueFull() const;

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        bool trySatisfyFunctional(PacketPtr pkt);
    };

    LinkResponsePort cpuSidePort;
    LinkRequestPort memSidePort;

  public:
    PcieLink(const PcieLinkParams &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void init() override;
};

} // namespace gem5

#endif // __DEV_STORAGE_PCIE_LINK_HH__
