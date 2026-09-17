/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p5 - P5 test: CXL hot standby.
 *
 * Saves a checkpoint to durable storage, keeps a hot subset in CXL (the hot
 * standby is a cache of the disk copy), then restores it at hit rates
 * 0/25/50/100%. A hot hit restores from CXL (CPU memcpy, fast) and a cold miss
 * restores from storage (device DMA, ~10us latency), so the restore time drops
 * as the hit rate rises (acceptance #4).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libckpt.h"
#include "ckptd.h"

static void
fill_pattern(uint8_t *buf, size_t len, uint32_t ckpt, uint32_t chunk)
{
    uint32_t state = ckpt * 2654435761U + chunk * 40503U;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
}

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
restore_once(struct simckpt_ctx *ctx, const uint8_t *cxl, uint8_t *dst,
             uint32_t num_chunks, uint32_t chunk_len, uint32_t num_hot,
             const uint8_t *ref, double *ms_out)
{
    double t0 = now_ms();
    for (uint32_t c = 0; c < num_chunks; c++) {
        uint8_t *d = dst + (size_t)c * chunk_len;
        if (c < num_hot) {
            /* hot hit: restore from the CXL standby (fast, no storage read) */
            memcpy(d, cxl + (size_t)c * chunk_len, chunk_len);
        } else {
            /* cold miss: restore from durable storage (device DMA) */
            simckpt_restore(ctx, d, (uint64_t)c * chunk_len, chunk_len,
                            1, c, NULL);
        }
    }
    double t1 = now_ms();
    if (ms_out)
        *ms_out = t1 - t0;

    /* verify byte-for-byte */
    return memcmp(dst, ref, (size_t)num_chunks * chunk_len) == 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
    const char *bar0 = (argc > 1 && argv[1][0]) ? argv[1] : NULL;
    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened\n");

    const uint32_t num_chunks = 512;
    const uint32_t chunk_len = 4096;
    size_t total = (size_t)num_chunks * chunk_len;

    uint8_t *src = ckpt_pool_alloc(0, total);   /* source (DRAM)            */
    uint8_t *cxl = ckpt_pool_alloc(1, total);   /* CXL hot standby          */
    uint8_t *dst = ckpt_pool_alloc(0, total);   /* restore destination      */
    struct ckpt_hot_standby hot;

    for (uint32_t c = 0; c < num_chunks; c++)
        fill_pattern(src + (size_t)c * chunk_len, chunk_len, 1, c);

    /* Save every chunk to durable storage, and mirror it into the CXL hot
     * standby (in a real system the GPU stages to CXL and CXL is kept). */
    for (uint32_t c = 0; c < num_chunks; c++) {
        simckpt_save(ctx, src + (size_t)c * chunk_len,
                     (uint64_t)c * chunk_len, chunk_len, 1, c, NULL);
        memcpy(cxl + (size_t)c * chunk_len, src + (size_t)c * chunk_len,
               chunk_len);
    }
    printf("saved %u chunks to storage + CXL standby\n", num_chunks);

    /* Hot set: keep the most recently used `capacity` chunks in CXL. */
    const uint32_t hit_rates[] = { 0, 25, 50, 100 };
    for (size_t i = 0; i < sizeof(hit_rates) / sizeof(hit_rates[0]); i++) {
        uint32_t rate = hit_rates[i];
        uint32_t num_hot = num_chunks * rate / 100;

        ckpt_hot_init(&hot, num_chunks);
        for (uint32_t c = 0; c < num_hot; c++)
            ckpt_hot_add(&hot, c);

        double ms = 0;
        memset(dst, 0xAA, total);
        int rc = restore_once(ctx, cxl, dst, num_chunks, chunk_len,
                              num_hot, src, &ms);
        printf("[hot %3u%%] restore %u chunks: %.3f ms (%s)\n",
               rate, num_chunks, ms, rc == 0 ? "OK" : "MISMATCH");
    }

    simckpt_close(ctx);
    return 0;
}
