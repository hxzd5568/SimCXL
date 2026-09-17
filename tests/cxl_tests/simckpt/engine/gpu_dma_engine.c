/* SPDX-License-Identifier: MIT */
#include "gpu_dma_engine.h"

#include <string.h>

/* A 32-bit integer hash avalanche (deterministic, good spread). */
static uint32_t
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
gpu_payload_word(uint32_t checkpoint_id, uint32_t chunk_id, uint32_t word_index)
{
    uint32_t h = mix32(checkpoint_id * 2654435761u);
    h = mix32(h ^ (chunk_id * 40503u));
    h = mix32(h ^ word_index);
    return h;
}

void
gpu_dma_engine_init(struct gpu_dma_engine *e, uint32_t seed)
{
    memset(e, 0, sizeof(*e));
    e->seed = seed;
}

void
gpu_payload_gen(struct gpu_dma_engine *e, void *buf, size_t len,
                uint32_t checkpoint_id, uint32_t chunk_id)
{
    uint8_t *p = (uint8_t *)buf;
    size_t i = 0;

    for (; i + 4 <= len; i += 4) {
        uint32_t w = gpu_payload_word(checkpoint_id, chunk_id,
                                      (uint32_t)(i / 4));
        p[i]     = (uint8_t)(w);
        p[i + 1] = (uint8_t)(w >> 8);
        p[i + 2] = (uint8_t)(w >> 16);
        p[i + 3] = (uint8_t)(w >> 24);
    }
    for (; i < len; i++)
        p[i] = (uint8_t)gpu_payload_word(checkpoint_id, chunk_id,
                                         (uint32_t)i);
    e->bytes_generated += len;
}

uint64_t
gpu_payload_check(struct gpu_dma_engine *e, const void *buf, size_t len,
                  uint32_t checkpoint_id, uint32_t chunk_id)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t bad = 0;
    size_t i = 0;

    for (; i + 4 <= len; i += 4) {
        uint32_t w = gpu_payload_word(checkpoint_id, chunk_id,
                                      (uint32_t)(i / 4));
        if (p[i] != (uint8_t)(w) || p[i + 1] != (uint8_t)(w >> 8) ||
            p[i + 2] != (uint8_t)(w >> 16) || p[i + 3] != (uint8_t)(w >> 24))
            bad++;
    }
    for (; i < len; i++)
        if (p[i] != (uint8_t)gpu_payload_word(checkpoint_id, chunk_id,
                                              (uint32_t)i))
            bad++;

    e->bytes_verified += len;
    e->mismatches += bad;
    return bad;
}
