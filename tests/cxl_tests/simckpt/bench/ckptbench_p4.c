/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p4 - P4 test: dynamic lane routing (分流) + pressure control.
 *
 *   test_balancing : routes a burst of chunks through the balancer. The DRAM
 *                    lane has a small pool (1 MiB) and CXL a large one (8 MiB),
 *                    so the balancer fills DRAM first and then spills into CXL
 *                    (headroom-driven routing). Verifies both lanes are used,
 *                    DRAM is capped at capacity, and every chunk round-trips
 *                    with the correct CRC.
 *
 *   test_pressure  : drives the pinned-pool state machine (NORMAL -> FROZEN ->
 *                    SHRINK -> STOPPED) and checks that once FROZEN, the pool
 *                    stops growing (pinned_bytes stops increasing).
 *
 *   --save-mib N --mode dram|cxl|balanced : optional single-shot bandwidth run
 *                    (bandwidth is read from the gem5 device execTicks stat).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libckpt.h"
#include "ckptd.h"
#include "balancer.h"

static void
fill_pattern(uint8_t *buf, size_t len, uint32_t ckpt, uint32_t chunk)
{
    uint32_t state = ckpt * 2654435761U + chunk * 40503U;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
}

/* --- Test A: dynamic lane routing -------------------------------------- */
static int
test_balancing(struct simckpt_ctx *ctx)
{
    const uint32_t chunk_len = 4096;
    const uint32_t num_chunks = 1024;       /* 4 MiB total */
    const uint64_t dram_cap = 1u << 20;     /* 1 MiB  */
    const uint64_t cxl_cap  = 8u << 20;     /* 8 MiB  */

    uint8_t *dram = ckpt_pool_alloc(0, dram_cap);
    uint8_t *cxl  = ckpt_pool_alloc(1, cxl_cap);
    if (!dram || !cxl) {
        printf("[balancing] FAIL: pool alloc\n");
        return 1;
    }

    struct balancer b;
    balancer_init(&b);
    /* equal bandwidth, huge token buckets -> only headroom drives routing */
    balancer_config_lane(&b, LANE_DRAM, "dram", 0, dram_cap,
                         30e9, 1e18, 1e18);
    balancer_config_lane(&b, LANE_CXL, "cxl", 1, cxl_cap,
                         30e9, 1e18, 1e18);

    uint64_t dram_chunks = 0, cxl_chunks = 0;
    int pass = 1;

    for (uint32_t c = 0; c < num_chunks; c++) {
        int lane = balancer_select_lane(&b, chunk_len);
        if (lane < 0) {
            printf("[balancing] FAIL: no lane available at chunk %u\n", c);
            pass = 0;
            break;
        }

        struct lane_stats *l = &b.lanes[lane];
        uint8_t *src = (lane == LANE_DRAM ? dram : cxl) + l->pool_used;
        uint32_t crc = 0;

        fill_pattern(src, chunk_len, 1, c);
        if (simckpt_save(ctx, src, (uint64_t)c * chunk_len, chunk_len,
                         1, c, &crc) != 0) {
            printf("[balancing] FAIL: save chunk %u\n", c);
            pass = 0;
            break;
        }

        l->pool_used += chunk_len;
        l->completed_chunks++;
        if (lane == LANE_DRAM)
            dram_chunks++;
        else
            cxl_chunks++;
    }

    printf("[balancing] chunks: dram=%" PRIu64 " (%" PRIu64 " B), "
           "cxl=%" PRIu64 " (%" PRIu64 " B)\n",
           dram_chunks, b.lanes[LANE_DRAM].pool_used,
           cxl_chunks, b.lanes[LANE_CXL].pool_used);

    if (dram_chunks == 0 || cxl_chunks == 0) {
        printf("[balancing] FAIL: both lanes should be used\n");
        pass = 0;
    }
    if (b.lanes[LANE_DRAM].pool_used > dram_cap) {
        printf("[balancing] FAIL: DRAM exceeded capacity\n");
        pass = 0;
    }

    printf("[balancing] %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

/* --- Test B: pinned-pool pressure control ------------------------------ */
static int
test_pressure(struct simckpt_ctx *ctx)
{
    (void)ctx;  /* pool state machine is device-independent */
    const size_t chunk = 4096;
    const uint64_t cap = 64 * 1024;   /* 64 KiB pool */

    struct ckpt_pool pool;
    ckpt_pool_init(&pool, 0, cap);

    /* NORMAL: fill to capacity. */
    void *bufs[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        bufs[i] = ckpt_pool_alloc_from(&pool, chunk);
        if (!bufs[i])
            break;
        n++;
    }
    printf("[pressure] NORMAL pinned=%" PRIu64 " (cap=%" PRIu64 ")\n",
           pool.pinned, cap);

    if (pool.pinned != cap) {
        printf("[pressure] FAIL: expected pool filled to capacity\n");
        return 1;
    }

    /* Pressure 85% -> FROZEN: reuse only, no growth. */
    ckpt_pool_update(&pool, 85);
    if (pool.state != POOL_FROZEN) {
        printf("[pressure] FAIL: expected FROZEN at 85%%\n");
        return 1;
    }
    void *extra = ckpt_pool_alloc_from(&pool, chunk);
    if (extra) {
        printf("[pressure] FAIL: FROZEN pool should not grow\n");
        return 1;
    }
    printf("[pressure] FROZEN at 85%%: new pin denied, pinned=%" PRIu64 "\n",
           pool.pinned);

    /* Pressure 92% -> SHRINK, 97% -> STOPPED. */
    ckpt_pool_update(&pool, 92);
    printf("[pressure] state at 92%% = %s\n",
           pool.state == POOL_SHRINK ? "SHRINK" : "WRONG");
    ckpt_pool_update(&pool, 97);
    printf("[pressure] state at 97%% = %s\n",
           pool.state == POOL_STOPPED ? "STOPPED" : "WRONG");

    if (pool.state != POOL_STOPPED) {
        printf("[pressure] FAIL: expected STOPPED at 97%%\n");
        return 1;
    }

    /* free everything */
    for (int i = 0; i < n; i++)
        ckpt_pool_free_from(&pool, bufs[i], chunk);

    printf("[pressure] PASS\n");
    return 0;
}

int
main(int argc, char **argv)
{
    const char *bar0 = NULL;
    int bench_mib = 0;
    const char *mode = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--save-mib") && i + 1 < argc)
            bench_mib = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            mode = argv[++i];
        else if (!bar0)
            bar0 = argv[i];
    }

    if (bench_mib > 0) {
        /* single-shot bandwidth run; the precise number comes from the gem5
         * execTicks stat, not the coarse guest clock. */
        const uint32_t chunk_len = 4096;
        uint32_t nchunks = (uint32_t)(((uint64_t)bench_mib << 20) / chunk_len);
        size_t total = (size_t)nchunks * chunk_len;
        struct simckpt_ctx *ctx = simckpt_open(bar0);
        if (!ctx) {
            fprintf(stderr, "FAIL: open\n");
            return 1;
        }

        uint8_t *src;
        if (mode && !strcmp(mode, "cxl"))
            src = ckpt_pool_alloc(1, total);
        else
            src = ckpt_pool_alloc(0, total);

        for (uint32_t c = 0; c < nchunks; c++)
            fill_pattern(src + (size_t)c * chunk_len, chunk_len, 3, c);

        uint64_t done = 0;
        for (uint32_t c = 0; c < nchunks; c++) {
            if (simckpt_save(ctx, src + (size_t)c * chunk_len,
                             (uint64_t)c * chunk_len, chunk_len, 3, c, NULL))
                break;
            done++;
        }
        printf("BENCH mode=%s chunks=%" PRIu64 "/%u\n",
               mode ? mode : "dram", done, nchunks);
        simckpt_close(ctx);
        return 0;
    }

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened\n");

    int pass = 0;
    pass |= test_balancing(ctx);
    pass |= test_pressure(ctx);

    simckpt_close(ctx);
    printf("%s\n", pass ? "FAIL" : "PASS");
    return pass ? 1 : 0;
}
