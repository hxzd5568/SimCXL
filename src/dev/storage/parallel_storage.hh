#ifndef __DEV_STORAGE_PARALLEL_STORAGE_HH__
#define __DEV_STORAGE_PARALLEL_STORAGE_HH__

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/ParallelStorage.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * Multi-channel flash-style backing store for AI checkpoints.
 *
 * Presents a single logical checkpoint namespace (byte offset [0, size)),
 * striped across `numChannels` independent channels. Each channel owns its
 * own byte-addressable backing store and processes requests independently
 * with its own read/write bandwidth, base latency, and outstanding-request
 * (queue depth) limit. Aggregate bandwidth therefore scales with the number
 * of channels.
 */
class ParallelStorage : public ClockedObject
{
  private:
    class StoragePort : public ResponsePort
    {
      private:
        ParallelStorage &storage;

      public:
        StoragePort(const std::string &name, ParallelStorage &s)
            : ResponsePort(name), storage(s)
        {}

      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        void recvFunctional(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
    };

    struct DeferredPacket
    {
        Tick tick;
        PacketPtr pkt;
        DeferredPacket(PacketPtr p, Tick t) : tick(t), pkt(p) {}
    };

    struct Channel
    {
        ParallelStorage *parent;
        int idx;
        std::vector<uint8_t> pmem;   // byte-addressable backing store
        Tick latency;
        double bwRead;               // ticks per byte
        double bwWrite;              // ticks per byte
        uint32_t maxOutstanding;
        uint32_t outstanding;
        Tick nextFree;               // channel busy-until tick (bandwidth)
        bool retryReq;
        bool retryResp;
        std::list<DeferredPacket> packetQueue;
        EventFunctionWrapper dequeueEvent;

        Channel(ParallelStorage *p, int i);
    };

    std::vector<std::unique_ptr<Channel>> channels;
    const uint32_t numChannels;
    const Addr chunkSize;
    const Addr logicalSize;

    StoragePort port;

    // Map a logical byte offset to a channel index and intra-channel offset.
    void mapAddr(Addr logical, uint32_t &chan, Addr &off) const;

    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
    void recvFunctional(PacketPtr pkt);
    Tick recvAtomic(PacketPtr pkt);
    void dequeue(int idx);
    AddrRangeList getAddrRanges() const;

    struct PSStats : public statistics::Group
    {
        PSStats(ParallelStorage &s, uint32_t numChannels);

        statistics::Scalar numReads;
        statistics::Scalar numWrites;
        statistics::Scalar bytesRead;
        statistics::Scalar bytesWritten;
        statistics::Scalar numRejected;
        statistics::Vector chanBytesRead;
        statistics::Vector chanBytesWritten;
        statistics::Vector chanRejected;
    } stats;

  public:
    using Params = ParallelStorageParams;
    explicit ParallelStorage(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void init() override;
    DrainState drain() override;
};

} // namespace gem5

#endif // __DEV_STORAGE_PARALLEL_STORAGE_HH__
