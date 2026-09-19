#include "dev/storage/sim_ckpt_device.hh"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "debug/SimCkptDevice.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace
{

uint64_t
readU64(const uint8_t *p)
{
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return letoh(v);
}

uint32_t
readU32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return letoh(v);
}

void
writeU32(uint8_t *p, uint32_t v)
{
    v = htole(v);
    std::memcpy(p, &v, sizeof(v));
}

const std::array<uint32_t, 256> &
crcTable()
{
    static std::array<uint32_t, 256> table;
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    return table;
}

// Reproducible GPU payload generator (mirrors the guest-side gpu_dma_engine.c
// so a staged chunk can later be byte-compared against the reference PRNG).
uint32_t
mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t
payloadWord(uint32_t checkpoint_id, uint32_t chunk_id, uint32_t word_index)
{
    uint32_t h = mix32(checkpoint_id * 2654435761u);
    h = mix32(h ^ (chunk_id * 40503u));
    h = mix32(h ^ word_index);
    return h;
}

void
generatePayloadBytes(uint8_t *p, size_t len, uint32_t checkpoint_id,
                     uint32_t chunk_id)
{
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t w = payloadWord(checkpoint_id, chunk_id,
                                 static_cast<uint32_t>(i / 4));
        p[i]     = static_cast<uint8_t>(w);
        p[i + 1] = static_cast<uint8_t>(w >> 8);
        p[i + 2] = static_cast<uint8_t>(w >> 16);
        p[i + 3] = static_cast<uint8_t>(w >> 24);
    }
    for (; i < len; i++)
        p[i] = static_cast<uint8_t>(
            payloadWord(checkpoint_id, chunk_id, static_cast<uint32_t>(i)));
}

} // anonymous namespace

SimCkptDevice::CkptStats::CkptStats(SimCkptDevice &dev, uint32_t numQueues)
    : statistics::Group(&dev),
      ADD_STAT(numDescCompleted, statistics::units::Count::get(),
               "Number of completed descriptors"),
      ADD_STAT(numBytesRead, statistics::units::Byte::get(),
               "Bytes DMA-read from source"),
      ADD_STAT(numBytesWritten, statistics::units::Byte::get(),
               "Bytes DMA-written to destination"),
      ADD_STAT(numInterrupts, statistics::units::Count::get(),
               "Number of interrupts posted"),
      ADD_STAT(numErrors, statistics::units::Count::get(),
               "Number of descriptor errors"),
      ADD_STAT(numQueueFull, statistics::units::Count::get(),
               "Times all descriptor slots were busy"),
      ADD_STAT(firstIssueTick, statistics::units::Tick::get(),
               "Tick of first descriptor issue"),
      ADD_STAT(lastCompletionTick, statistics::units::Tick::get(),
               "Tick of last completion"),
      ADD_STAT(execTicks, statistics::units::Tick::get(),
               "Execution ticks (last completion - first issue)"),
      ADD_STAT(chunkLatency, statistics::units::Tick::get(),
               "Per-chunk completion latency (issue -> completion)"),
      ADD_STAT(qDescCompleted, statistics::units::Count::get(),
               "Descriptors completed per queue"),
      ADD_STAT(qBytesRead, statistics::units::Byte::get(),
               "Bytes DMA-read per queue"),
      ADD_STAT(qBytesWritten, statistics::units::Byte::get(),
               "Bytes DMA-written per queue"),
      ADD_STAT(qQueueFull, statistics::units::Count::get(),
               "Queue-full events per queue"),
      ADD_STAT(qRetry, statistics::units::Count::get(),
               "Storage/DMA retries per queue")
{
    execTicks = lastCompletionTick - firstIssueTick;
    chunkLatency.init(100);
    qDescCompleted.init(numQueues);
    qBytesRead.init(numQueues);
    qBytesWritten.init(numQueues);
    qQueueFull.init(numQueues);
    qRetry.init(numQueues);
}

SimCkptDevice::Slot::Slot(SimCkptDevice *dev, int queue, int idx)
    : dmaDoneEvent([dev, queue, idx] { dev->onDmaDone(queue, idx); },
                   dev->name() + ".q" + std::to_string(queue) +
                   ".slot" + std::to_string(idx) + ".dmaDone")
{
    std::memset(descBuf, 0, sizeof(descBuf));
    std::memset(cplBuf, 0, sizeof(cplBuf));
}

SimCkptDevice::SimCkptDevice(const Params &p)
    : PciDevice(p),
      queueDepth(p.queue_depth),
      numQueues(p.num_queues),
      maxChunkSize(p.max_chunk_size),
      procLat(p.proc_lat),
      storagePort(name() + ".storage_port", this),
      stats(*this, numQueues)
{
    for (uint32_t q = 0; q < numQueues; q++) {
        auto qq = std::make_unique<Queue>();
        for (uint32_t i = 0; i < queueDepth; i++) {
            qq->slots.emplace_back(std::make_unique<Slot>(this, q, i));
            qq->slots.back()->dataBuf.resize(static_cast<size_t>(maxChunkSize));
        }
        queues.emplace_back(std::move(qq));
        dmaPorts.emplace_back(std::make_unique<DmaPort>(this, sys,
                                                        p.sid, p.ssid));
    }
    queueSel = 0;
}

void
SimCkptDevice::init()
{
    for (uint32_t q = 0; q < numQueues; q++)
        panic_if(!dmaPorts[q]->isConnected(),
                 "DMA lane port %u of %s not connected to anything!",
                 q, name());
    // The inherited single dmaPort (DmaDevice) is unused; skip its
    // connectivity check and run the PIO check directly.
    PioDevice::init();
}

SimCkptDevice::StoragePort::StoragePort(const std::string &name,
                                        SimCkptDevice *dev)
    : RequestPort(name), device(dev),
      requestorId(dev->sys->getRequestorId(dev))
{
}

void
SimCkptDevice::StoragePort::sendStorage(Packet::Command cmd, Addr addr,
                                        int size, uint8_t *data, Event *event)
{
    RequestPtr req = std::make_shared<Request>(addr, size, 0, requestorId);
    PacketPtr pkt = new Packet(req, cmd);
    pkt->dataStatic(data);
    pkt->senderState = new StorageState{event};
    pending.push_back(pkt);
    trySend();
}

void
SimCkptDevice::StoragePort::trySend()
{
    if (blocked)
        return;
    while (!pending.empty()) {
        if (!sendTimingReq(pending.front())) {
            blocked = true;
            device->queues[device->queueSel]->retryCount++;
            device->stats.qRetry[device->queueSel]++;
            return;
        }
        pending.pop_front();
    }
}

bool
SimCkptDevice::StoragePort::recvTimingResp(PacketPtr pkt)
{
    StorageState *st = dynamic_cast<StorageState *>(pkt->senderState);
    assert(st);
    device->schedule(st->event, curTick());
    delete st;
    delete pkt;
    return true;
}

void
SimCkptDevice::StoragePort::recvReqRetry()
{
    assert(blocked);
    blocked = false;
    trySend();
}

Port &
SimCkptDevice::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "storage_port")
        return storagePort;
    if (if_name == "dma") {
        panic_if(idx >= static_cast<PortID>(dmaPorts.size()),
                 "SimCkptDevice: dma lane index %d out of range", idx);
        return *dmaPorts[idx];
    }
    return PciDevice::getPort(if_name, idx);
}

SimCkptDevice::Queue &
SimCkptDevice::selectedQueue()
{
    return *queues[queueSel];
}

const SimCkptDevice::Queue &
SimCkptDevice::selectedQueue() const
{
    return *queues[queueSel];
}

void
SimCkptDevice::resetQueue(Queue &q)
{
    q.sqBase = 0;
    q.sqDepth = 0;
    q.cqBase = 0;
    q.cqDepth = 0;
    q.sqHead = 0;
    q.sqTail = 0;
    q.cqHead = 0;
    q.cqTail = 0;
    q.completedCount = 0;
    q.intrEnable = false;
    q.intrPosted = 0;
    q.execBusy = false;
    q.numQueueFull = 0;
    q.retryCount = 0;
    q.completedBytes = 0;
    q.latencySum = 0;
    q.latencyCount = 0;
    q.latencySamples.clear();
    q.firstIssueTick = 0;
    q.lastCompletionTick = 0;
    for (auto &sp : q.slots) {
        sp->state = State::Free;
        sp->crc = 0;
    }
}

Addr
SimCkptDevice::barOffset(Addr addr) const
{
    assert(BARs[0]);
    return addr - BARs[0]->addr();
}

AddrRangeList
SimCkptDevice::getAddrRanges() const
{
    return PciDevice::getAddrRanges();
}

uint32_t
SimCkptDevice::crc32(const uint8_t *data, size_t len) const
{
    const auto &table = crcTable();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFU;
}

uint64_t
SimCkptDevice::queueLatencyP95(const Queue &q) const
{
    if (q.latencySamples.empty())
        return 0;
    auto samples = q.latencySamples;
    std::sort(samples.begin(), samples.end());
    size_t idx = static_cast<size_t>(0.95 * static_cast<double>(samples.size()));
    if (idx >= samples.size())
        idx = samples.size() - 1;
    return samples[idx];
}

void
SimCkptDevice::parseDescriptor(Slot &slot)
{
    const uint8_t *b = slot.descBuf;
    slot.desc.src_addr       = readU64(b + 0);
    slot.desc.dst_addr       = readU64(b + 8);
    slot.desc.storage_offset = readU64(b + 16);
    slot.desc.length         = readU32(b + 24);
    slot.desc.checkpoint_id  = readU32(b + 28);
    slot.desc.chunk_id       = readU32(b + 32);
    slot.desc.flags          = readU32(b + 36);
    slot.desc.crc32          = readU32(b + 40);

    slot.save = !(slot.desc.flags & FLAG_RESTORE);

    DPRINTF(SimCkptDevice,
            "desc src=%#lx dst=%#lx storage=%#lx len=%u ckpt=%u chunk=%u "
            "flags=%#x exp_crc=%#x %s\n",
            slot.desc.src_addr, slot.desc.dst_addr, slot.desc.storage_offset,
            slot.desc.length, slot.desc.checkpoint_id, slot.desc.chunk_id,
            slot.desc.flags, slot.desc.crc32,
            slot.save ? "SAVE" : "RESTORE");
}

void
SimCkptDevice::generatePayload(Slot &slot)
{
    generatePayloadBytes(slot.dataBuf.data(), slot.desc.length,
                         slot.desc.checkpoint_id, slot.desc.chunk_id);
    slot.crc = crc32(slot.dataBuf.data(), slot.desc.length);
}

void
SimCkptDevice::postCompletion(int qIdx, Queue &q, Slot &slot)
{
    uint8_t *b = slot.cplBuf;
    writeU32(b + 0, slot.desc.checkpoint_id);
    writeU32(b + 4, slot.desc.chunk_id);

    uint32_t cplStatus = 0;
    if (slot.desc.crc32 != 0 && slot.desc.crc32 != slot.crc) {
        cplStatus = 1;  // CRC mismatch
    }
    writeU32(b + 8, cplStatus);
    writeU32(b + 12, slot.crc);

    Addr cplAddr = q.cqBase + static_cast<uint64_t>(q.cqTail % q.cqDepth) *
                                CPL_SIZE;
    slot.state = State::WritingCpl;
    dmaPorts[qIdx]->dmaAction(MemCmd::WriteReq, cplAddr, CPL_SIZE,
                              &slot.dmaDoneEvent, slot.cplBuf, 0);
    q.cqTail++;
}

void
SimCkptDevice::tryFillSlots(int qIdx, Queue &q)
{
    if (q.sqDepth == 0)
        return;

    if (!q.execBusy && q.sqHead != q.sqTail) {
        q.execBusy = true;
        q.firstIssueTick = curTick();
        stats.firstIssueTick = curTick();
    }

    while (q.sqHead != q.sqTail) {
        Slot *free = nullptr;
        for (auto &sp : q.slots) {
            if (sp->state == State::Free) {
                free = sp.get();
                break;
            }
        }
        if (!free) {
            q.numQueueFull++;
            stats.numQueueFull++;
            stats.qQueueFull[qIdx]++;
            break;
        }

        free->state = State::FetchDesc;
        free->issueTick = curTick();
        Addr descAddr = q.sqBase +
            static_cast<uint64_t>(q.sqHead % q.sqDepth) * DESC_SIZE;
        dmaPorts[qIdx]->dmaAction(MemCmd::ReadReq, descAddr, DESC_SIZE,
                                  &free->dmaDoneEvent, free->descBuf, procLat);
        q.sqHead++;
    }
}

void
SimCkptDevice::onDmaDone(int queue, int idx)
{
    Queue &q = *queues[queue];
    Slot &slot = *q.slots[idx];

    switch (slot.state) {
      case State::FetchDesc: {
        parseDescriptor(slot);
        if (slot.desc.length == 0 ||
            slot.desc.length > static_cast<uint32_t>(maxChunkSize)) {
            DPRINTF(SimCkptDevice, "invalid length %u\n", slot.desc.length);
            errorCount++;
            stats.numErrors++;
            slot.state = State::Free;
            tryFillSlots(queue, q);
            break;
        }
        if (slot.desc.flags & FLAG_STAGE) {
            // GPU staging: synthesize the reproducible payload directly in the
            // device and DMA-write it to the DRAM/CXL destination (no source
            // read, no storage access).
            generatePayload(slot);
            DPRINTF(SimCkptDevice, "stage ckpt=%u chunk=%u -> dst=%#lx\n",
                    slot.desc.checkpoint_id, slot.desc.chunk_id,
                    slot.desc.dst_addr);
            slot.state = State::Writing;
            dmaPorts[queue]->dmaAction(MemCmd::WriteReq, slot.desc.dst_addr,
                                       slot.desc.length, &slot.dmaDoneEvent,
                                       slot.dataBuf.data(), 0);
            break;
        }
        slot.state = State::Reading;
        if (slot.save) {
            dmaPorts[queue]->dmaAction(MemCmd::ReadReq, slot.desc.src_addr,
                                       slot.desc.length, &slot.dmaDoneEvent,
                                       slot.dataBuf.data(), 0);
        } else {
            storagePort.sendStorage(MemCmd::ReadReq, slot.desc.storage_offset,
                                    slot.desc.length, slot.dataBuf.data(),
                                    &slot.dmaDoneEvent);
        }
        break;
      }
      case State::Reading: {
        slot.crc = crc32(slot.dataBuf.data(), slot.desc.length);
        stats.numBytesRead += slot.desc.length;
        stats.qBytesRead[queue] += slot.desc.length;
        slot.state = State::Writing;
        if (slot.save) {
            storagePort.sendStorage(MemCmd::WriteReq, slot.desc.storage_offset,
                                    slot.desc.length, slot.dataBuf.data(),
                                    &slot.dmaDoneEvent);
        } else {
            dmaPorts[queue]->dmaAction(MemCmd::WriteReq, slot.desc.dst_addr,
                                       slot.desc.length, &slot.dmaDoneEvent,
                                       slot.dataBuf.data(), 0);
        }
        break;
      }
      case State::Writing: {
        postCompletion(queue, q, slot);
        break;
      }
      case State::WritingCpl: {
        stats.numBytesWritten += slot.desc.length;
        stats.qBytesWritten[queue] += slot.desc.length;
        q.completedBytes += slot.desc.length;
        q.completedCount++;
        stats.numDescCompleted++;
        stats.qDescCompleted[queue]++;
        Tick lat = curTick() - slot.issueTick;
        stats.chunkLatency.sample(lat);
        q.latencySum += lat;
        q.latencyCount++;
        if (q.latencySamples.size() < (1 << 20))
            q.latencySamples.push_back(lat);
        if (q.intrEnable) {
            intrPost();
            q.intrPosted++;
            stats.numInterrupts++;
        }
        DPRINTF(SimCkptDevice,
                "queue %d chunk %u (ckpt %u) done, crc=%#x\n",
                queue, slot.desc.chunk_id, slot.desc.checkpoint_id, slot.crc);
        slot.state = State::Free;
        tryFillSlots(queue, q);

        if (q.execBusy) {
            bool idle = (q.sqHead == q.sqTail);
            for (auto &sp : q.slots) {
                if (sp->state != State::Free) {
                    idle = false;
                    break;
                }
            }
            if (idle) {
                q.execBusy = false;
                q.lastCompletionTick = curTick();
                stats.lastCompletionTick = curTick();
            }
        }
        break;
      }
      default:
        panic("SimCkptDevice: queue %d slot %d in bad state", queue, idx);
    }
}

Tick
SimCkptDevice::read(PacketPtr pkt)
{
    Addr offset = barOffset(pkt->getAddr());
    uint64_t value = 0;

    const Queue &q = selectedQueue();

    switch (offset) {
      case REG_CTRL:        value = ctrl; break;
      case REG_STATUS:      value = status; break;
      case REG_SQ_BASE:     value = q.sqBase; break;
      case REG_SQ_DEPTH:    value = q.sqDepth; break;
      case REG_CQ_BASE:     value = q.cqBase; break;
      case REG_CQ_DEPTH:    value = q.cqDepth; break;
      case REG_COMPLETED:   value = q.completedCount; break;
      case REG_INTR_EN:     value = q.intrEnable ? 1 : 0; break;
      case REG_INTR_POSTED: value = q.intrPosted; break;
      case REG_SQ_HEAD:     value = q.sqHead; break;
      case REG_CQ_TAIL:     value = q.cqTail; break;
      case REG_ERRORS:      value = errorCount; break;
      case REG_NUM_QUEUES:  value = numQueues; break;
      case REG_QUEUE_SEL:   value = queueSel; break;
      case REG_Q_OUTSTANDING: {
        for (auto &sp : q.slots)
            if (sp->state != State::Free)
                value++;
        break;
      }
      case REG_Q_RETRY:     value = q.retryCount; break;
      case REG_Q_QUEUE_FULL: value = q.numQueueFull; break;
      case REG_Q_COMPLETED_BYTES: value = q.completedBytes; break;
      case REG_Q_LATENCY_AVG:
        value = q.latencyCount ? q.latencySum / q.latencyCount : 0;
        break;
      case REG_Q_LATENCY_P95: value = queueLatencyP95(q); break;
      case REG_Q_ISSUE_TICK: value = q.firstIssueTick; break;
      case REG_Q_DONE_TICK: value = q.lastCompletionTick; break;
      default:              value = 0; break;
    }

    pkt->setUintX(value, ByteOrder::little);
    pkt->makeResponse();
    return pioDelay;
}

Tick
SimCkptDevice::write(PacketPtr pkt)
{
    Addr offset = barOffset(pkt->getAddr());
    uint64_t value = pkt->getUintX(ByteOrder::little);

    switch (offset) {
      case REG_CTRL:
        ctrl = value;
        if (value & CTRL_RESET) {
            resetQueue(selectedQueue());
        }
        if (value & CTRL_START) {
            tryFillSlots(queueSel, selectedQueue());
        }
        break;
      case REG_QUEUE_SEL:
        if (value < numQueues)
            queueSel = value;
        break;
      case REG_SQ_BASE:
        selectedQueue().sqBase = value;
        break;
      case REG_SQ_DEPTH:
        selectedQueue().sqDepth = value;
        break;
      case REG_CQ_BASE:
        selectedQueue().cqBase = value;
        break;
      case REG_CQ_DEPTH:
        selectedQueue().cqDepth = value;
        break;
      case REG_SQ_DOORBELL:
        selectedQueue().sqTail = value;
        DPRINTF(SimCkptDevice, "doorbell queue=%u sqTail=%u\n",
                queueSel, selectedQueue().sqTail);
        tryFillSlots(queueSel, selectedQueue());
        break;
      case REG_CQ_DOORBELL:
        selectedQueue().cqHead = value;
        break;
      case REG_INTR_EN:
        selectedQueue().intrEnable = (value & 1) != 0;
        break;
      default:
        break;
    }

    pkt->makeResponse();
    return pioDelay;
}

} // namespace gem5
