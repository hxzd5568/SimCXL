#include "dev/storage/parallel_storage.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>

#include "base/trace.hh"
#include "debug/ParallelStorage.hh"

namespace gem5
{

ParallelStorage::PSStats::PSStats(ParallelStorage &s, uint32_t numChannels)
    : statistics::Group(&s),
      ADD_STAT(numReads, statistics::units::Count::get(),
               "Number of storage reads"),
      ADD_STAT(numWrites, statistics::units::Count::get(),
               "Number of storage writes"),
      ADD_STAT(bytesRead, statistics::units::Byte::get(),
               "Bytes read from storage"),
      ADD_STAT(bytesWritten, statistics::units::Byte::get(),
               "Bytes written to storage"),
      ADD_STAT(numRejected, statistics::units::Count::get(),
               "Times a request was rejected (queue full)"),
      ADD_STAT(chanBytesRead, statistics::units::Byte::get(),
               "Bytes read per channel"),
      ADD_STAT(chanBytesWritten, statistics::units::Byte::get(),
               "Bytes written per channel"),
      ADD_STAT(chanRejected, statistics::units::Count::get(),
               "Rejections per channel")
{
    chanBytesRead.init(numChannels);
    chanBytesWritten.init(numChannels);
    chanRejected.init(numChannels);
}

ParallelStorage::Channel::Channel(ParallelStorage *p, int i)
    : parent(p),
      idx(i),
      pmem(0),
      latency(0),
      bwRead(0),
      bwWrite(0),
      maxOutstanding(1),
      outstanding(0),
      nextFree(0),
      retryReq(false),
      retryResp(false),
      dequeueEvent([p, i] { p->dequeue(i); },
                   p->name() + ".chan" + std::to_string(i) + ".dequeue")
{
}

ParallelStorage::ParallelStorage(const Params &p)
    : ClockedObject(p),
      numChannels(p.num_channels),
      chunkSize(p.chunk_size),
      logicalSize(p.size),
      port(name() + ".port", *this),
      stats(*this, numChannels)
{
    const Addr channelCapacity = logicalSize / numChannels;
    panic_if(channelCapacity == 0, "ParallelStorage: per-channel capacity 0");
    panic_if(chunkSize == 0, "ParallelStorage: chunk_size must be non-zero");

    for (uint32_t i = 0; i < numChannels; i++) {
        auto ch = std::make_unique<Channel>(this, i);
        ch->pmem.resize(channelCapacity, 0);
        ch->latency = p.latency;
        ch->bwRead = p.read_bw;
        ch->bwWrite = p.write_bw;
        ch->maxOutstanding = p.queue_depth;
        channels.emplace_back(std::move(ch));
    }
}

void
ParallelStorage::mapAddr(Addr logical, uint32_t &chan, Addr &off) const
{
    Addr chunkId = logical / chunkSize;
    Addr within = logical % chunkSize;
    chan = chunkId % numChannels;
    off = (chunkId / numChannels) * chunkSize + within;
}

Port &
ParallelStorage::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    return ClockedObject::getPort(if_name, idx);
}

void
ParallelStorage::init()
{
    if (port.isConnected())
        port.sendRangeChange();
}

AddrRangeList
ParallelStorage::getAddrRanges() const
{
    return AddrRangeList({AddrRange(0, logicalSize)});
}

bool
ParallelStorage::recvTimingReq(PacketPtr pkt)
{
    uint32_t chanIdx;
    Addr off;
    mapAddr(pkt->getAddr(), chanIdx, off);
    Channel &ch = *channels[chanIdx];

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "ParallelStorage only handles read/write, got %s",
             pkt->cmdString());

    if (ch.retryReq)
        return false;

    if (ch.outstanding >= ch.maxOutstanding) {
        ch.retryReq = true;
        stats.numRejected++;
        stats.chanRejected[chanIdx]++;
        DPRINTF(ParallelStorage, "chan %d queue full, retry\n", chanIdx);
        return false;
    }

    ch.outstanding++;

    Tick receiveDelay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    double bw = pkt->isRead() ? ch.bwRead : ch.bwWrite;
    Tick duration = static_cast<Tick>(pkt->getSize() * bw);
    Tick start = std::max(curTick(), ch.nextFree);
    ch.nextFree = start + duration;
    Tick respTick = start + duration + ch.latency + receiveDelay;

    ch.packetQueue.emplace_back(pkt, respTick);
    if (!ch.dequeueEvent.scheduled())
        schedule(ch.dequeueEvent, respTick);

    return true;
}

void
ParallelStorage::dequeue(int idx)
{
    Channel &ch = *channels[idx];
    assert(!ch.packetQueue.empty());
    DeferredPacket dp = ch.packetQueue.front();

    uint32_t chan;
    Addr off;
    mapAddr(dp.pkt->getAddr(), chan, off);
    assert(chan == static_cast<uint32_t>(idx));

    // Serialized per channel, so a read observes any prior write.
    if (dp.pkt->isRead()) {
        dp.pkt->setData(ch.pmem.data() + off);
        stats.numReads++;
        stats.bytesRead += dp.pkt->getSize();
        stats.chanBytesRead[idx] += dp.pkt->getSize();
    } else {
        dp.pkt->writeData(ch.pmem.data() + off);
        stats.numWrites++;
        stats.bytesWritten += dp.pkt->getSize();
        stats.chanBytesWritten[idx] += dp.pkt->getSize();
    }
    if (dp.pkt->needsResponse())
        dp.pkt->makeResponse();

    ch.retryResp = !port.sendTimingResp(dp.pkt);
    if (ch.retryResp)
        return;

    ch.packetQueue.pop_front();
    ch.outstanding--;

    if (ch.retryReq && ch.outstanding < ch.maxOutstanding) {
        ch.retryReq = false;
        port.sendRetryReq();
    }

    if (!ch.packetQueue.empty()) {
        reschedule(ch.dequeueEvent,
                   std::max(ch.packetQueue.front().tick, curTick()), true);
    } else if (drainState() == DrainState::Draining) {
        signalDrainDone();
    }
}

void
ParallelStorage::recvRespRetry()
{
    for (auto &ch : channels) {
        if (ch->retryResp) {
            ch->retryResp = false;
            ch->dequeueEvent.process();
            return;
        }
    }
}

void
ParallelStorage::recvFunctional(PacketPtr pkt)
{
    uint32_t chan;
    Addr off;
    mapAddr(pkt->getAddr(), chan, off);
    Channel &ch = *channels[chan];
    if (pkt->isRead()) {
        pkt->setData(ch.pmem.data() + off);
    } else if (pkt->isWrite()) {
        pkt->writeData(ch.pmem.data() + off);
    }
    pkt->makeResponse();
}

Tick
ParallelStorage::recvAtomic(PacketPtr pkt)
{
    uint32_t chan;
    Addr off;
    mapAddr(pkt->getAddr(), chan, off);
    Channel &ch = *channels[chan];
    if (pkt->isRead()) {
        pkt->setData(ch.pmem.data() + off);
        stats.numReads++;
        stats.bytesRead += pkt->getSize();
        stats.chanBytesRead[chan] += pkt->getSize();
    } else {
        pkt->writeData(ch.pmem.data() + off);
        stats.numWrites++;
        stats.bytesWritten += pkt->getSize();
        stats.chanBytesWritten[chan] += pkt->getSize();
    }
    if (pkt->needsResponse())
        pkt->makeResponse();
    return ch.latency;
}

DrainState
ParallelStorage::drain()
{
    for (auto &ch : channels) {
        if (!ch->packetQueue.empty() || ch->outstanding > 0)
            return DrainState::Draining;
    }
    return DrainState::Drained;
}

bool
ParallelStorage::StoragePort::recvTimingReq(PacketPtr pkt)
{
    return storage.recvTimingReq(pkt);
}

void
ParallelStorage::StoragePort::recvRespRetry()
{
    storage.recvRespRetry();
}

void
ParallelStorage::StoragePort::recvFunctional(PacketPtr pkt)
{
    storage.recvFunctional(pkt);
}

Tick
ParallelStorage::StoragePort::recvAtomic(PacketPtr pkt)
{
    return storage.recvAtomic(pkt);
}

AddrRangeList
ParallelStorage::StoragePort::getAddrRanges() const
{
    return storage.getAddrRanges();
}

} // namespace gem5
