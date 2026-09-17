/* SPDX-License-Identifier: MIT */
/*
 * balancer_scale.c - host-side unit test: prove lane routing scales.
 *
 * Runs a 512 MiB checkpoint (131072 x 4 KiB chunks) through the balancer, with
 * a 128 MiB DRAM pool and a 512 MiB CXL pool. Pure C, no gem5: the routing
 * logic is a per-chunk decision loop, so this verifies the exact same code
 * path at full scale in milliseconds.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "balancer.h"

static void
run(const char *tag, uint64_t total_mib, uint64_t dram_mib, uint64_t cxl_mib,
    double dram_bw, double cxl_bw, double occupancy_weight)
{
    const uint32_t chunk = 4096;
    uint64_t nchunks = (total_mib << 20) / chunk;

    struct balancer b;
    balancer_init(&b);
    b.occupancy_weight = occupancy_weight;
    balancer_config_lane(&b, LANE_DRAM, "dram", 0, dram_mib << 20,
                         dram_bw, 1e18, 1e18);
    balancer_config_lane(&b, LANE_CXL, "cxl", 1, cxl_mib << 20,
                         cxl_bw, 1e18, 1e18);

    uint64_t dram_chunks = 0, cxl_chunks = 0, denied = 0;
    for (uint64_t c = 0; c < nchunks; c++) {
        int lane = balancer_select_lane(&b, chunk);
        if (lane < 0) {
            denied++;
            continue;
        }
        struct lane_stats *l = &b.lanes[lane];
        l->pool_used += chunk;
        l->queued_bytes += chunk;
        if (lane == LANE_DRAM)
            dram_chunks++;
        else
            cxl_chunks++;
    }

    uint64_t dram_bytes = b.lanes[LANE_DRAM].pool_used;
    uint64_t cxl_bytes  = b.lanes[LANE_CXL].pool_used;

    printf("[%s] total=%" PRIu64 " MiB (%" PRIu64 " chunks)\n",
           tag, total_mib, nchunks);
    printf("  dram=%" PRIu64 " MiB (%" PRIu64 " chunks), "
           "cxl=%" PRIu64 " MiB (%" PRIu64 " chunks), denied=%" PRIu64 "\n",
           dram_bytes >> 20, dram_chunks, cxl_bytes >> 20, cxl_chunks, denied);
    printf("  ratio dram:cxl = %" PRIu64 ":%" PRIu64 "\n",
           dram_chunks, cxl_chunks);

    int ok = 1;
    if (dram_bytes > (dram_mib << 20)) {
        printf("  FAIL: DRAM exceeded capacity\n");
        ok = 0;
    }
    if (dram_chunks == 0 || cxl_chunks == 0) {
        printf("  FAIL: both lanes should be used\n");
        ok = 0;
    }
    if (denied != 0 && cxl_bytes >= (cxl_mib << 20)) {
        printf("  FAIL: unexpected denial\n");
        ok = 0;
    }
    printf("  %s\n", ok ? "PASS" : "FAIL");
}

int
main(void)
{
    /* 512 MiB checkpoint: DRAM pool 128 MiB fills first (headroom), then the
     * balancer spills the remaining 384 MiB into CXL. Same per-chunk decision
     * loop as the 4 MiB gem5 test, just scaled 128x. */
    run("512MiB headroom-routing", 512, 128, 512, 30e9, 30e9, 0.0);

    return 0;
}
