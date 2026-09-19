#ifndef __DEV_STORAGE_SIM_CKPT_DEVICE_HH__
#define __DEV_STORAGE_SIM_CKPT_DEVICE_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "dev/dma_device.hh"
#include "dev/pci/device.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/SimCkptDevice.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * AI-checkpoint DMA engine (SimCkptDevice) with multiple independent queues.
 *
 * Models the host-side "real data mover" that replaces two independent
 * PyTrafficGen streams. For each descriptor submitted through a submission
 * queue the device:
 *
 *   1. DMA-reads the descriptor itself from the submission queue (SQ);
 *   2. DMA-reads the source payload into an internal buffer;
 *   3. computes a CRC32 over the returned payload;
 *   4. DMA-writes the payload (the *same* bytes returned by the read) to the
 *      destination address;
 *   5. only after the write response is received, DMA-writes a completion
 *      entry into the completion queue (CQ) and optionally raises an
 *      interrupt.
 *
 * Since P10 the device exposes `num_queues` independent SQ/CQ engines (one per
 * GPU DMA lane, e.g. DRAM lane -> queue 0, CXL lane -> queue 1). Each queue has
 * its own head/tail indices, its own descriptor slot pool and its own
 * completion/interrupt accounting, so the lanes neither serialize nor starve
 * each other. The active queue is selected through the REG_QUEUE_SEL register;
 * all SQ/CQ programming, doorbell and counter registers then operate on the
 * selected queue. Per-queue (per-link) queueing/bandwidth/latency/retry
 * statistics are exposed as read-only BAR registers so ckptd can observe them
 * from the guest (target.md section 6).
 */
class SimCkptDevice : public PciDevice
{
  public:
    // Descriptor/completion entries are cache-line sized (64 B) so that
    // concurrent DMA transactions never alias on a cache line (the Ruby
    // DMASequencer tracks sub-requests per cache line).
    static constexpr size_t DESC_SIZE = 64;
    static constexpr size_t CPL_SIZE = 64;

    enum DescFlags : uint32_t
    {
        FLAG_SAVE    = 1 << 0,  // memory (src_addr) -> storage (storage_offset)
        FLAG_RESTORE = 1 << 1,  // storage (storage_offset) -> memory (dst_addr)
        FLAG_STAGE   = 1 << 2,  // GPU staging: generate PRNG payload into
                                //   dst_addr (DRAM/CXL), no source read/storage
    };

    /** Descriptor layout in host memory (little-endian, packed to 64 B). */
    struct Descriptor
    {
        uint64_t src_addr;
        uint64_t dst_addr;
        uint64_t storage_offset;
        uint32_t length;
        uint32_t checkpoint_id;
        uint32_t chunk_id;
        uint32_t flags;
        uint32_t crc32;
        uint32_t reserved[5];  // pad to 64 B
    };

    /** Completion layout in host memory (little-endian, packed to 64 B). */
    struct Completion
    {
        uint32_t checkpoint_id;
        uint32_t chunk_id;
        uint32_t status;   // 0 == success
        uint32_t crc32;
        uint32_t reserved[12];  // pad to 64 B
    };

  private:
    enum RegisterOffset : Addr
    {
        REG_CTRL           = 0x00,
        REG_STATUS         = 0x08,
        REG_SQ_BASE        = 0x10,
        REG_SQ_DEPTH       = 0x18,
        REG_CQ_BASE        = 0x20,
        REG_CQ_DEPTH       = 0x28,
        REG_SQ_DOORBELL    = 0x30,
        REG_CQ_DOORBELL    = 0x38,
        REG_COMPLETED      = 0x40,
        REG_INTR_EN        = 0x48,
        REG_INTR_POSTED    = 0x50,
        REG_SQ_HEAD        = 0x58,
        REG_CQ_TAIL        = 0x60,
        REG_ERRORS         = 0x68,
        // P10 multi-queue control + per-link statistics.
        REG_QUEUE_SEL      = 0x70,
        REG_NUM_QUEUES     = 0x78,
        REG_Q_OUTSTANDING  = 0x80,
        REG_Q_RETRY        = 0x88,
        REG_Q_QUEUE_FULL   = 0x90,
        REG_Q_COMPLETED_BYTES = 0x98,
        REG_Q_LATENCY_AVG  = 0xA0,
        REG_Q_LATENCY_P95  = 0xA8,
        REG_Q_ISSUE_TICK   = 0xB0,
        REG_Q_DONE_TICK    = 0xB8,
    };

    enum CtrlBits : uint64_t
    {
        CTRL_START = 1ULL << 0,
        CTRL_RESET = 1ULL << 1,
    };

    enum StatusBits : uint64_t
    {
        STATUS_BUSY  = 1ULL << 0,
        STATUS_DONE  = 1ULL << 1,
        STATUS_ERROR = 1ULL << 2,
    };

    enum class State : uint8_t
    {
        Free = 0,
        FetchDesc,
        Reading,
        Writing,
        WritingCpl,
    };

    /** One in-flight descriptor slot. */
    struct Slot
    {
        EventFunctionWrapper dmaDoneEvent;
        State state = State::Free;
        uint8_t descBuf[DESC_SIZE];
        uint8_t cplBuf[CPL_SIZE];
        std::vector<uint8_t> dataBuf;
        Descriptor desc;
        uint32_t crc = 0;
        bool save = true;   // save: memory->storage; restore: storage->memory
        Tick issueTick = 0;

        Slot(SimCkptDevice *dev, int queue, int idx);
    };

    /**
     * One independent submission/completion engine (a GPU DMA lane). Owns its
     * SQ/CQ head/tail, a private descriptor-slot pool and per-link statistics.
     */
    struct Queue
    {
        Addr sqBase = 0;
        uint32_t sqDepth = 0;
        Addr cqBase = 0;
        uint32_t cqDepth = 0;
        uint32_t sqHead = 0;   // device-owned read index
        uint32_t sqTail = 0;   // driver-owned write index
        uint32_t cqHead = 0;   // driver-owned read index
        uint32_t cqTail = 0;   // device-owned write index
        uint64_t completedCount = 0;
        bool intrEnable = false;
        uint64_t intrPosted = 0;
        bool execBusy = false;
        Tick firstIssueTick = 0;
        Tick lastCompletionTick = 0;

        std::vector<std::unique_ptr<Slot>> slots;

        // Per-link (per-queue) statistics, also mapped to read-only BAR regs.
        uint64_t numQueueFull = 0;
        uint64_t retryCount = 0;
        uint64_t completedBytes = 0;
        uint64_t latencySum = 0;
        uint64_t latencyCount = 0;
        std::vector<Tick> latencySamples;
    };

    /**
     * Point-to-point port to the ParallelStorage backend. Unlike the DMA port
     * it does not split transfers into cache lines: the storage models
     * bandwidth/latency per request (e.g. a 4 KiB flash page), so the whole
     * payload is issued as a single request. The storage backend is shared by
     * all queues (the storage side is the shared bottleneck, not the lanes).
     */
    class StoragePort : public RequestPort
    {
      private:
        SimCkptDevice *device;
        RequestorID requestorId;

        struct StorageState : public Packet::SenderState
        {
            Event *event;
            StorageState(Event *e) : event(e) {}
        };

        std::deque<PacketPtr> pending;
        bool blocked = false;

        void trySend();

      public:
        StoragePort(const std::string &name, SimCkptDevice *dev);

        void sendStorage(Packet::Command cmd, Addr addr, int size,
                         uint8_t *data, Event *event);

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    const uint32_t queueDepth;
    const uint32_t numQueues;
    const Addr maxChunkSize;
    const Tick procLat;

    // Port to the ParallelStorage backend (save/restore), shared across lanes.
    StoragePort storagePort;

    // Per-lane DMA ports (one per queue): each lane gets its own Ruby
    // DMASequencer so the GPU DMA lanes neither serialize nor contend on a
    // single DMA engine.
    std::vector<std::unique_ptr<DmaPort>> dmaPorts;

    // P10 multi-queue state.
    uint32_t queueSel = 0;
    std::vector<std::unique_ptr<Queue>> queues;

    // Device-global status (mirrored to REG_STATUS).
    uint64_t ctrl = 0;
    uint64_t status = 0;
    uint64_t errorCount = 0;

    // Statistics.
    struct CkptStats : public statistics::Group
    {
        explicit CkptStats(SimCkptDevice &dev, uint32_t numQueues);

        statistics::Scalar numDescCompleted;
        statistics::Scalar numBytesRead;
        statistics::Scalar numBytesWritten;
        statistics::Scalar numInterrupts;
        statistics::Scalar numErrors;
        statistics::Scalar numQueueFull;
        statistics::Scalar firstIssueTick;
        statistics::Scalar lastCompletionTick;
        statistics::Formula execTicks;
        statistics::Histogram chunkLatency;      // per-chunk completion latency
        // Per-link (per-queue) statistics (acceptance #8: per-link stats).
        statistics::Vector qDescCompleted;
        statistics::Vector qBytesRead;
        statistics::Vector qBytesWritten;
        statistics::Vector qQueueFull;
        statistics::Vector qRetry;
    } stats;

    Addr barOffset(Addr addr) const;
    Queue &selectedQueue();
    const Queue &selectedQueue() const;

    void resetQueue(Queue &q);
    void tryFillSlots(int qIdx, Queue &q);
    void onDmaDone(int queue, int idx);
    void postCompletion(int qIdx, Queue &q, Slot &slot);
    void parseDescriptor(Slot &slot);
    void generatePayload(Slot &slot);
    uint32_t crc32(const uint8_t *data, size_t len) const;
    uint64_t queueLatencyP95(const Queue &q) const;

  public:
    using Params = SimCkptDeviceParams;
    explicit SimCkptDevice(const Params &p);

    void init() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    AddrRangeList getAddrRanges() const override;
};

} // namespace gem5

#endif // __DEV_STORAGE_SIM_CKPT_DEVICE_HH__
