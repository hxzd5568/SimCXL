/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p9 - P9: explicit topology + manifest state machine (Ruby).
 *
 * Builds the CXLMemSim-style topology object {GPU, DRAM pool, CXL pool, N
 * storage channels}, derives striping and candidate paths, then drives the real
 * SimCkptDevice through a single-generation save/restore while stepping every
 * chunk through the full lifecycle state machine:
 *
 *     FREE -> PINNED -> IN_FLIGHT -> DISK_COMMITTED -> HOT -> EVICTABLE
 *
 * and reports:
 *   - per-channel byte distribution (must match the striping formula);
 *   - manifest version/generation;
 *   - final chunk-state histogram;
 *   - save/restore correctness (GPU recheck).
 *
 * This exercises P9 items 1 (topology + striping + candidate paths), 2
 * (manifest state machine) and 3 (N-channel striping awareness).
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
#include "gpu_dma_engine.h"
#include "topology.h"

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 4, chunk = 4096;
    int channels = 4;
    double hot_frac = 0.5;
    const char *bar0 = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--channels") && i + 1 < argc)
            channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ckpt-mib") && i + 1 < argc)
            size_mib = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hot-frac") && i + 1 < argc)
            hot_frac = atof(argv[++i]);
        else if (!bar0)
            bar0 = argv[i];
    }
    if (channels < 1 || channels > TOPO_MAX_CHANNELS)
        channels = 4;

    struct ckpt_topology topo;
    topology_init(&topo, chunk, channels);
    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    uint32_t hot_cap = (uint32_t)(hot_frac * (double)n_chunks);
    size_t size = (size_t)n_chunks * chunk;

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened (topology: %d storage channels, %u chunks, "
           "hot_cap=%u)\n", channels, n_chunks, hot_cap);

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0xBEEFCAFEu);

    uint8_t *dram = ckpt_pool_alloc(0, size);
    uint8_t *cxl  = ckpt_pool_alloc(1, size);
    uint8_t *dst  = ckpt_pool_alloc(0, size);
    if (!dram || !cxl || !dst) {
        fprintf(stderr, "FAIL: alloc\n");
        return 1;
    }

    uint64_t chan_bytes[TOPO_MAX_CHANNELS] = { 0 };
    struct ckpt_manifest m;
    ckpt_manifest_init(&m);
    ckpt_manifest_begin(&m, 1, n_chunks);

    double t0 = now_ms();
    for (uint32_t c = 0; c < n_chunks; c++) {
        uint8_t *src = dram + (size_t)c * chunk;
        uint32_t crc = 0;

        gpu_payload_gen(&eng, src, chunk, 1, c);

        /* lifecycle: PINNED -> IN_FLIGHT -> DISK_COMMITTED */
        ckpt_manifest_add(&m, c, (uint64_t)c * chunk, (uint32_t)chunk,
                          crc, CKPT_SOURCE_DISK);
        ckpt_manifest_submit(&m, c);
        if (simckpt_save(ctx, src, (uint64_t)c * chunk, (uint32_t)chunk,
                         1, c, &crc) != 0) {
            printf("FAIL: save chunk %u\n", c);
            return 1;
        }
        ckpt_manifest_commit(&m, c);

        /* striping: which channel did this chunk land on? */
        chan_bytes[topology_stripe_channel(&topo, c)] += chunk;
    }
    double save_ms = now_ms() - t0;

    /* hot promotion + eviction (HOT -> EVICTABLE) for the LRU tail */
    for (uint32_t c = 0; c < hot_cap; c++)
        ckpt_chunk_promote_hot(&m, c);
    if (hot_cap > 0)
        ckpt_chunk_evictable(&m, hot_cap - 1);

    ckpt_manifest_finish(&m);

    /* per-channel balance */
    uint64_t expect = (uint64_t)(n_chunks / (uint32_t)channels) * chunk;
    int balance_ok = 1;
    printf("[topology] per-channel bytes (chunks striped by chunk_id %% N):\n");
    for (int i = 0; i < channels; i++) {
        uint64_t dev = chan_bytes[i] > expect ? chan_bytes[i] - expect
                                              : expect - chan_bytes[i];
        int ok = dev <= chunk;   /* allow +/- one chunk rounding */
        if (!ok)
            balance_ok = 0;
        printf("  channel %d: %" PRIu64 " MiB%s\n", i, chan_bytes[i] >> 20,
               ok ? "" : "  <-- MISMATCH");
    }
    printf("[topology] striping balance: %s\n", balance_ok ? "OK" : "MISMATCH");

    /* candidate paths for a sample chunk */
    struct topo_path paths[2];
    int np = topology_candidate_paths(&topo, 5, /*write_to_storage=*/1, paths);
    printf("[topology] chunk 5 candidate save paths: %d\n", np);

    /* mirror the hot standby into CXL before restoring (GPU also staged there) */
    for (uint32_t c = 0; c < hot_cap; c++)
        memcpy(cxl + (size_t)c * chunk, dram + (size_t)c * chunk, chunk);

    /* restore: hot chunks from CXL standby, cold from storage */
    uint64_t base = 0;
    t0 = now_ms();
    for (uint32_t c = 0; c < n_chunks; c++) {
        if (ckpt_chunk_state(&m, c) == CKPT_STATE_HOT ||
            ckpt_chunk_state(&m, c) == CKPT_STATE_EVICTABLE) {
            memcpy(dst + (size_t)c * chunk, cxl + (size_t)c * chunk, chunk);
        } else {
            if (simckpt_restore(ctx, dst + (size_t)c * chunk,
                                base + (uint64_t)c * chunk, (uint32_t)chunk,
                                1, c, NULL) != 0) {
                printf("FAIL: restore chunk %u\n", c);
                return 1;
            }
        }
    }
    double restore_ms = now_ms() - t0;

    /* verify */
    uint64_t mism = 0;
    for (uint32_t c = 0; c < n_chunks; c++)
        mism += gpu_payload_check(&eng, dst + (size_t)c * chunk, chunk, 1, c);
    int ok = (mism == 0) && balance_ok;

    uint32_t counts[CKPT_STATE_EVICTABLE + 1];
    ckpt_manifest_state_counts(&m, counts);

    printf("[manifest] version=%" PRIu64 " gen=%" PRIu64 " chunks=%u\n",
           m.version, m.committed_gen, m.num_chunks);
    printf("[manifest] states: pinned=%u in_flight=%u committed=%u hot=%u "
           "evictable=%u\n",
           counts[CKPT_STATE_PINNED], counts[CKPT_STATE_IN_FLIGHT],
           counts[CKPT_STATE_DISK_COMMITTED], counts[CKPT_STATE_HOT],
           counts[CKPT_STATE_EVICTABLE]);
    printf("[perf] save %.3f ms, restore %.3f ms (%u chunks, %d channels)\n",
           save_ms, restore_ms, n_chunks, channels);
    printf("gpu recheck: %s (mismatches=%" PRIu64 ")\n",
           ok ? "OK" : "MISMATCH", mism);

    ckpt_pool_free(dram, size);
    ckpt_pool_free(cxl, size);
    ckpt_pool_free(dst, size);
    simckpt_close(ctx);

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
