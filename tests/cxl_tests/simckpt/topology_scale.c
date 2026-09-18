/* SPDX-License-Identifier: MIT */
/*
 * topology_scale.c - host-side unit test for the explicit topology (P9).
 *
 * No gem5 needed: proves the CXLMemSim-style topology object at full scale
 * (512 MiB = 131072 x 4 KiB chunks) in milliseconds:
 *
 *   1. striping balance across N channels (must match ParallelStorage);
 *   2. channel-offset arithmetic;
 *   3. steady-state Bsave/Brestore bounds across node x channel counts;
 *   4. candidate-path enumeration + rate = min(node, channel);
 *   5. full chunk lifecycle state machine (FREE->PINNED->IN_FLIGHT->
 *      DISK_COMMITTED->HOT->EVICTABLE->FREE) + invalid transitions;
 *   6. manifest version/generation bumps.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "ckptd.h"
#include "topology.h"

static int
test_striping(void)
{
    struct ckpt_topology t;
    topology_init(&t, 4096, 4);

    const uint64_t nchunks = 512ull << (20 - 12);   /* 512 MiB / 4 KiB */
    uint64_t chan_bytes[4] = { 0 };
    for (uint64_t c = 0; c < nchunks; c++) {
        int ch = topology_stripe_channel(&t, (uint32_t)c);
        if (ch < 0 || ch >= 4) {
            printf("  FAIL: bad channel %d for chunk %" PRIu64 "\n", ch, c);
            return 1;
        }
        chan_bytes[ch] += t.chunk_size;
    }

    uint64_t expect = (nchunks / 4) * t.chunk_size;
    printf("[striping] 512 MiB over 4 channels:\n");
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        printf("  channel %d: %" PRIu64 " MiB%s\n", i, chan_bytes[i] >> 20,
               chan_bytes[i] == expect ? "" : "  <-- MISMATCH");
        if (chan_bytes[i] != expect)
            ok = 0;
    }

    /* channel-offset arithmetic: chunk 5 on channel 1 -> offset 1*4096 */
    uint64_t off = topology_channel_offset(&t, 5, 0);
    if (off != 4096) {
        printf("  FAIL: chunk 5 channel offset = %" PRIu64 ", want 4096\n", off);
        ok = 0;
    }
    printf("  chunk 5 -> channel %d offset %" PRIu64 "\n",
           topology_stripe_channel(&t, 5), off);
    printf("[striping] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int
test_bounds(void)
{
    static const char *node_name[3] = { "dram", "cxl", "both" };
    printf("[bounds] Bsave (GB/s):\n");
    printf("  %8s | %6s | %6s | %6s | %6s\n", "node", "1ch", "2ch", "4ch", "8ch");
    for (int node = TOPO_DRAM; node <= TOPO_BOTH; node++) {
        struct ckpt_topology t;
        printf("  %8s |", node_name[node]);
        for (int ch = 1; ch <= 8; ch *= 2) {
            topology_init(&t, 4096, ch);
            printf(" %6.2f |", topology_bsave_bound(&t, node) / 1e9);
        }
        printf("\n");
    }

    /* sanity: both + 8ch should be limited by memory egress, not storage */
    struct ckpt_topology t;
    topology_init(&t, 4096, 8);
    double b = topology_bsave_bound(&t, TOPO_BOTH) / 1e9;
    if (b < 40.0) {
        printf("  FAIL: both/8ch bound %.2f GB/s < 40 (expected mem-egress "
               "~49)\n", b);
        return 1;
    }
    printf("[bounds] PASS\n");
    return 0;
}

static int
test_paths(void)
{
    struct ckpt_topology t;
    topology_init(&t, 4096, 4);

    struct topo_path paths[2];
    int n = topology_candidate_paths(&t, 5, /*write_to_storage=*/1, paths);
    if (n != 2) {
        printf("  FAIL: expected 2 candidate paths, got %d\n", n);
        return 1;
    }
    printf("[paths] chunk 5 candidate save paths:\n");
    for (int i = 0; i < n; i++) {
        double expect = (paths[i].node == TOPO_DRAM) ? t.dram_read_bw
                                                      : t.cxl_read_bw;
        double chan = t.channels[paths[i].channel].write_bw;
        expect = expect < chan ? expect : chan;
        if (paths[i].rate != expect) {
            printf("  FAIL: path rate %g != min(node,chan) %g\n",
                   paths[i].rate, expect);
            return 1;
        }
        printf("  node %s -> channel %d: rate %.2f GB/s, cost %.3f us\n",
               paths[i].node == TOPO_DRAM ? "dram" : "cxl", paths[i].channel,
               paths[i].rate / 1e9, topology_path_cost(&t, &paths[i], 4096) * 1e6);
    }

    /* cost should increase with channel congestion */
    double c0 = topology_path_cost(&t, &paths[0], 4096);
    t.channels[paths[0].channel].queued_bytes = 4096 * 100;
    double c1 = topology_path_cost(&t, &paths[0], 4096);
    if (c1 <= c0) {
        printf("  FAIL: congested channel cost did not increase\n");
        return 1;
    }
    printf("[paths] PASS\n");
    return 0;
}

static int
test_state_machine(void)
{
    struct ckpt_manifest m;
    ckpt_manifest_init(&m);
    ckpt_manifest_begin(&m, 1, 4);

    uint32_t counts[CKPT_STATE_EVICTABLE + 1];

    /* FREE -> PINNED -> IN_FLIGHT -> DISK_COMMITTED */
    for (uint32_t c = 0; c < 4; c++)
        ckpt_manifest_add(&m, c, c * 4096, 4096, 0xdeadbeef, CKPT_SOURCE_DISK);

    ckpt_manifest_state_counts(&m, counts);
    printf("[state] after add: pinned=%u in_flight=%u committed=%u\n",
           counts[CKPT_STATE_PINNED], counts[CKPT_STATE_IN_FLIGHT],
           counts[CKPT_STATE_DISK_COMMITTED]);
    if (counts[CKPT_STATE_PINNED] != 4) {
        printf("  FAIL: expected 4 pinned chunks\n");
        return 1;
    }

    for (uint32_t c = 0; c < 4; c++)
        ckpt_manifest_submit(&m, c);
    for (uint32_t c = 0; c < 4; c++)
        ckpt_manifest_commit(&m, c);

    /* promote half to HOT, then one to EVICTABLE */
    for (uint32_t c = 0; c < 2; c++)
        ckpt_chunk_promote_hot(&m, c);
    ckpt_chunk_evictable(&m, 0);

    ckpt_manifest_state_counts(&m, counts);
    printf("[state] after commit: committed=%u hot=%u evictable=%u\n",
           counts[CKPT_STATE_DISK_COMMITTED], counts[CKPT_STATE_HOT],
           counts[CKPT_STATE_EVICTABLE]);
    if (counts[CKPT_STATE_DISK_COMMITTED] != 2 ||
        counts[CKPT_STATE_HOT] != 1 || counts[CKPT_STATE_EVICTABLE] != 1) {
        printf("  FAIL: unexpected state distribution\n");
        return 1;
    }

    /* invalid transition: free an in-flight chunk should fail */
    ckpt_manifest_add(&m, 9, 9 * 4096, 4096, 0, CKPT_SOURCE_DISK);
    if (ckpt_chunk_free(&m, 9) != -1) {
        printf("  FAIL: freeing a pinned/in-flight chunk should be rejected\n");
        return 1;
    }

    ckpt_manifest_finish(&m);
    if (m.version != 1 || m.committed_gen != 1) {
        printf("  FAIL: manifest version/generation not bumped\n");
        return 1;
    }
    printf("[state] manifest version=%" PRIu64 " committed_gen=%" PRIu64 "\n",
           m.version, m.committed_gen);
    printf("[state] PASS\n");
    return 0;
}

int
main(void)
{
    int rc = 0;
    rc |= test_striping();
    rc |= test_bounds();
    rc |= test_paths();
    rc |= test_state_machine();
    printf("%s\n", rc ? "FAIL" : "PASS");
    return rc ? 1 : 0;
}
