P2 is implemented and verified. Summary:
What I built (P2: parallel storage)
New ParallelStorage (gem5 C++):
- src/dev/storage/ParallelStorage.py, parallel_storage.hh/.cc — a multi-channel flash-style backing store.
- Each channel is independent with its own byte-addressable backing store, read/write bandwidth (7 GB/s), base latency (10 µs), and queue depth / outstanding limit.
- Chunk striping: channel = chunk_id % N, channel_offset = (chunk_id / N) * chunk_size + offset % chunk_size — verified: both channels got exactly 16384 bytes each.
- Per-channel stats: chanBytesRead/chanBytesWritten/chanRejected.
SimCkptDevice extended to a real save/restore engine:
- Added a point-to-point StoragePort (whole-request, no cache-line split so flash latency applies to pages, not 64 B).
- Descriptor flags now encode direction: FLAG_SAVE (memory→storage) and FLAG_RESTORE (storage→memory).
- Save = DMA-read memory → CRC → write storage → completion only after both the memory read and storage write complete. Restore = storage read → DMA-write DRAM/CXL.
Fixed two real bugs found along the way:
1. SQ/CQ ring wrap bug — indices were % depth, so a full ring made the device re-process descriptors forever. Now absolute indices + % depth only for addressing.
2. Completion-write aliasing from a too-small CQ ring.
Results
[SAVE DRAM->stor ] completed=4  [RESTORE stor->DRAM] completed=4
[SAVE CXL->stor  ] completed=4  [RESTORE stor->CXL ] completed=4
DRAM round-trip OK  CXL round-trip OK  PASS
Bandwidth scaling (acceptance criterion #3) — measured via the device's own execTicks (doorbell→last completion, not the guest's coarse clock):
channels
1
2
Two channels ≈ 1.58× one channel, confirming the parallel-channel bandwidth gain.
Files: ParallelStorage.{py,hh,cc} (new), sim_ckpt_device.{hh,cc}, SimCkpt.py, x86_board.py, SConscript, x86-cxl-simckpt-test.py, tests/cxl_tests/simckpt/simckpt_test.cpp.
▣  Build · DeepSeek V4 Pro