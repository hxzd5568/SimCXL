/* SPDX-License-Identifier: MIT */
/*
 * gpu_dma_engine - user-space emulation of the GPU DMA engine.
 *
 * SimCXL has no silicon GPU model, so the "GPU" side of a checkpoint is
 * emulated as a deterministic DMA engine that stages checkpoint payload into
 * DRAM/CXL and, on resume, regenerates the expected payload to verify what came
 * back through storage. The payload is reproducible:
 *
 *     payload[i] = PRNG(checkpoint_id, chunk_id, i)
 *
 * (byte i is the high byte of a 32-bit word, i.e. an fp32 tensor element),
 * so the verifier never needs to keep a reference copy in memory: it simply
 * regenerates the bytes and compares (acceptance #1 / #5).
 */
#ifndef GPU_DMA_ENGINE_H
#define GPU_DMA_ENGINE_H

#include <stddef.h>
#include <stdint.h>

struct gpu_dma_engine {
    uint32_t seed;             /* per-run seed                          */
    uint64_t bytes_generated;  /* bytes staged (GPU -> DRAM/CXL)        */
    uint64_t bytes_verified;   /* bytes rechecked (DRAM/CXL -> GPU)     */
    uint64_t mismatches;       /* bytes that failed verification        */
};

void gpu_dma_engine_init(struct gpu_dma_engine *e, uint32_t seed);

/* Generate the reproducible payload for one chunk into `buf`. */
void gpu_payload_gen(struct gpu_dma_engine *e, void *buf, size_t len,
                     uint32_t checkpoint_id, uint32_t chunk_id);

/* Regenerate the expected payload and compare `buf` against it byte-for-byte.
 * Returns the number of mismatching bytes (0 = match). */
uint64_t gpu_payload_check(struct gpu_dma_engine *e, const void *buf, size_t len,
                           uint32_t checkpoint_id, uint32_t chunk_id);

/* The raw 32-bit payload word for byte index i/4 (fp32 tensor element). */
uint32_t gpu_payload_word(uint32_t checkpoint_id, uint32_t chunk_id,
                          uint32_t word_index);

#endif /* GPU_DMA_ENGINE_H */
