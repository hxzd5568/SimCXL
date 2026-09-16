#include "dev/storage/sim_ckpt_device.hh"

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

} // anonymous namespace

SimCkptDevice::CkptStats::CkptStats(SimCkptDevice &dev)
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
               "Times all descriptor slots were busy")
{
}

SimCkptDevice::Slot::Slot(SimCkptDevice *dev, int idx)
    : dmaDoneEvent([dev, idx] { dev->onDmaDone(idx); },
                   dev->name() + ".slot" + std::to_string(idx) + ".dmaDone")
{
    std::memset(descBuf, 0, sizeof(descBuf));
    std::memset(cplBuf, 0, sizeof(cplBuf));
}

SimCkptDevice::SimCkptDevice(const Params &p)
    : PciDevice(p),
      queueDepth(p.queue_depth),
      maxChunkSize(p.max_chunk_size),
      procLat(p.proc_lat),
      stats(*this)
{
    for (uint32_t i = 0; i < queueDepth; i++) {
        slots.emplace_back(std::make_unique<Slot>(this, i));
        slots.back()->dataBuf.resize(static_cast<size_t>(maxChunkSize));
    }
    resetState();
}

void
SimCkptDevice::resetState()
{
    ctrl = 0;
    status = 0;
    sqBase = 0;
    sqDepth = 0;
    cqBase = 0;
    cqDepth = 0;
    sqHead = 0;
    sqTail = 0;
    cqHead = 0;
    cqTail = 0;
    completedCount = 0;
    intrEnable = false;
    intrPosted = 0;
    errorCount = 0;
    for (auto &sp : slots) {
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

    DPRINTF(SimCkptDevice,
            "desc src=%#lx dst=%#lx len=%u ckpt=%u chunk=%u flags=%#x "
            "exp_crc=%#x\n",
            slot.desc.src_addr, slot.desc.dst_addr, slot.desc.length,
            slot.desc.checkpoint_id, slot.desc.chunk_id, slot.desc.flags,
            slot.desc.crc32);
}

void
SimCkptDevice::postCompletion(Slot &slot)
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

    Addr cplAddr = cqBase + static_cast<uint64_t>(cqTail) * CPL_SIZE;
    slot.state = State::WritingCpl;
    dmaPort.dmaAction(MemCmd::WriteReq, cplAddr, CPL_SIZE,
                      &slot.dmaDoneEvent, slot.cplBuf, 0);
    cqTail = (cqTail + 1) % cqDepth;
}

void
SimCkptDevice::tryFillSlots()
{
    while (sqHead != sqTail) {
        Slot *free = nullptr;
        for (auto &sp : slots) {
            if (sp->state == State::Free) {
                free = sp.get();
                break;
            }
        }
        if (!free) {
            stats.numQueueFull++;
            break;
        }

        free->state = State::FetchDesc;
        free->issueTick = curTick();
        Addr descAddr = sqBase + static_cast<uint64_t>(sqHead) * DESC_SIZE;
        dmaPort.dmaAction(MemCmd::ReadReq, descAddr, DESC_SIZE,
                          &free->dmaDoneEvent, free->descBuf, procLat);
        sqHead = (sqHead + 1) % sqDepth;
    }
}

void
SimCkptDevice::onDmaDone(int idx)
{
    Slot &slot = *slots[idx];

    switch (slot.state) {
      case State::FetchDesc: {
        parseDescriptor(slot);
        if (slot.desc.length == 0 ||
            slot.desc.length > static_cast<uint32_t>(maxChunkSize)) {
            DPRINTF(SimCkptDevice, "invalid length %u\n", slot.desc.length);
            errorCount++;
            stats.numErrors++;
            slot.state = State::Free;
            tryFillSlots();
            break;
        }
        slot.state = State::Reading;
        dmaPort.dmaAction(MemCmd::ReadReq, slot.desc.src_addr,
                          slot.desc.length, &slot.dmaDoneEvent,
                          slot.dataBuf.data(), 0);
        break;
      }
      case State::Reading: {
        slot.crc = crc32(slot.dataBuf.data(), slot.desc.length);
        stats.numBytesRead += slot.desc.length;
        slot.state = State::Writing;
        dmaPort.dmaAction(MemCmd::WriteReq, slot.desc.dst_addr,
                          slot.desc.length, &slot.dmaDoneEvent,
                          slot.dataBuf.data(), 0);
        break;
      }
      case State::Writing: {
        postCompletion(slot);
        break;
      }
      case State::WritingCpl: {
        stats.numBytesWritten += slot.desc.length;
        completedCount++;
        stats.numDescCompleted++;
        if (intrEnable) {
            intrPost();
            intrPosted++;
            stats.numInterrupts++;
        }
        DPRINTF(SimCkptDevice,
                "chunk %u (ckpt %u) done, crc=%#x\n",
                slot.desc.chunk_id, slot.desc.checkpoint_id, slot.crc);
        slot.state = State::Free;
        tryFillSlots();
        break;
      }
      default:
        panic("SimCkptDevice: slot %d in bad state", idx);
    }
}

Tick
SimCkptDevice::read(PacketPtr pkt)
{
    Addr offset = barOffset(pkt->getAddr());
    uint64_t value = 0;

    switch (offset) {
      case REG_CTRL:        value = ctrl; break;
      case REG_STATUS:      value = status; break;
      case REG_SQ_BASE:     value = sqBase; break;
      case REG_SQ_DEPTH:    value = sqDepth; break;
      case REG_CQ_BASE:     value = cqBase; break;
      case REG_CQ_DEPTH:    value = cqDepth; break;
      case REG_COMPLETED:   value = completedCount; break;
      case REG_INTR_EN:     value = intrEnable ? 1 : 0; break;
      case REG_INTR_POSTED: value = intrPosted; break;
      case REG_SQ_HEAD:     value = sqHead; break;
      case REG_CQ_TAIL:     value = cqTail; break;
      case REG_ERRORS:      value = errorCount; break;
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
            resetState();
        }
        if (value & CTRL_START) {
            tryFillSlots();
        }
        break;
      case REG_SQ_BASE:
        sqBase = value;
        break;
      case REG_SQ_DEPTH:
        sqDepth = value;
        break;
      case REG_CQ_BASE:
        cqBase = value;
        break;
      case REG_CQ_DEPTH:
        cqDepth = value;
        break;
      case REG_SQ_DOORBELL:
        sqTail = value;
        DPRINTF(SimCkptDevice, "doorbell sqTail=%u\n", sqTail);
        tryFillSlots();
        break;
      case REG_CQ_DOORBELL:
        cqHead = value;
        break;
      case REG_INTR_EN:
        intrEnable = (value & 1) != 0;
        break;
      default:
        break;
    }

    pkt->makeResponse();
    return pioDelay;
}

} // namespace gem5
