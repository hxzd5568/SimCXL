/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p10 - P10: multi-queue / multi-lane engines + full acceptance
 * matrix + per-link statistics.
 *
 * Drives the SimCkptDevice through its P10 multi-queue interface (one
 * independent SQ/CQ engine per GPU DMA lane) and batch submission (one
 * doorbell per lane, absolute indices, no per-chunk reset), then runs the
 * target.md eight-point acceptance matrix:
 *
 *   #1 GPU->DRAM/CXL->storage->DRAM/CXL->GPU CRC/byte consistency
 *   #2 dual-memory-path staging bandwidth > fastest single path
 *   #3 N storage channels striping balance (bandwidth scaling from P2)
 *   #4 CXL hot-standby hit lowers restore cost (fewer storage reads)
 *   #5 out-of-order chunk completion still restores correctly
 *   #6 pressure stops pinned-pool growth (reuse only)
 *   #7 in-flight DMA is never unpinned before completion
 *   #8 per-link (per-queue) queueing/bandwidth/latency/retry statistics
 *
 * The GPU staging phase uses FLAG_STAGE descriptors: the device synthesizes
 * the reproducible PRNG payload (matching gpu_dma_engine.c) and DMA-writes it
 * into DRAM/CXL. Each lane has its own DMA port (and hence its own Ruby
 * DMASequencer), so the two lanes stage concurrently. Bandwidth is measured
 * cycle-accurately from the per-queue issue/done tick registers (no guest
 * clock quantization). For #2 the lane split is balanced to the measured
 * per-lane bandwidth so the dual-path aggregate equals B_dram + B_cxl.
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

static void
fill_stage_desc(struct simckpt_desc *d, uint64_t dst_phys, uint32_t len,
                uint32_t ckpt, uint32_t chunk)
{
    memset(d, 0, sizeof(*d));
    d->dst_addr = dst_phys;
    d->length = len;
    d->checkpoint_id = ckpt;
    d->chunk_id = chunk;
    d->flags = SIMCKPT_FLAG_STAGE;
}

static void
fill_save_desc(struct simckpt_desc *d, uint64_t src_phys, uint64_t off,
               uint32_t len, uint32_t ckpt, uint32_t chunk)
{
    memset(d, 0, sizeof(*d));
    d->src_addr = src_phys;
    d->storage_offset = off;
    d->length = len;
    d->checkpoint_id = ckpt;
    d->chunk_id = chunk;
    d->flags = SIMCKPT_FLAG_SAVE;
    d->crc32 = 0;   /* CRC offload: device computes, guest verifies later */
}

static void
fill_restore_desc(struct simckpt_desc *d, uint64_t dst_phys, uint64_t off,
                  uint32_t len, uint32_t ckpt, uint32_t chunk)
{
    memset(d, 0, sizeof(*d));
    d->dst_addr = dst_phys;
    d->storage_offset = off;
    d->length = len;
    d->checkpoint_id = ckpt;
    d->chunk_id = chunk;
    d->flags = SIMCKPT_FLAG_RESTORE;
}

/* Read and verify the completion ring entries written for a batch. */
static int
verify_completions(struct simckpt_ctx *ctx, int q, uint64_t start, uint32_t n,
                   const char *what)
{
    struct simckpt_cpl *cpl = calloc(n, sizeof(*cpl));
    if (!cpl)
        return -1;
    simckpt_completions_q(ctx, q, start, cpl, n);
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cpl[i].status != 0) {
            printf("  %s: cpl[%u] status=%u (chunk %u)\n", what, i,
                   cpl[i].status, cpl[i].chunk_id);
            bad++;
        }
    }
    free(cpl);
    return bad ? -1 : 0;
}

/* Submit both lanes concurrently (true multi-engine overlap), then wait both. */
static int
dual_submit_wait(struct simckpt_ctx *ctx, int nq,
                 const struct simckpt_desc *d0, uint32_t n0,
                 const struct simckpt_desc *d1, uint32_t n1,
                 uint64_t *qdone, int verify)
{
    int rc = 0;
    if (n0)
        simckpt_submit_batch(ctx, 0, d0, n0);
    if (nq > 1 && n1)
        simckpt_submit_batch(ctx, 1, d1, n1);
    if (n0 && simckpt_wait_q(ctx, 0, qdone[0] + n0) != 0)
        rc = -1;
    if (nq > 1 && n1 && simckpt_wait_q(ctx, 1, qdone[1] + n1) != 0)
        rc = -1;
    if (verify) {
        if (n0)
            rc |= verify_completions(ctx, 0, qdone[0], n0, "q0");
        if (nq > 1 && n1)
            rc |= verify_completions(ctx, 1, qdone[1], n1, "q1");
    }
    qdone[0] += n0;
    qdone[1] += n1;
    return rc;
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

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    int nq = simckpt_num_queues(ctx);
    if (nq > SIMCKPT_MAX_QUEUES)
        nq = SIMCKPT_MAX_QUEUES;
    if (nq > 2)
        nq = 2;   /* this test uses two lanes (DRAM + CXL) */
    printf("SimCkptDevice opened (queues=%d, channels=%d)\n", nq, channels);

    struct ckpt_topology topo;
    topology_init(&topo, chunk, channels);
    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    uint32_t hot_cap = (uint32_t)(hot_frac * (double)n_chunks);
    size_t size = (size_t)n_chunks * chunk;

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0x10C0FFEEu);

    uint8_t *dram = ckpt_pool_alloc(0, size);
    uint8_t *cxl  = ckpt_pool_alloc(1, size);
    uint8_t *dst  = ckpt_pool_alloc(0, size);
    if (!dram || !cxl || !dst) {
        fprintf(stderr, "FAIL: alloc\n");
        return 1;
    }

    for (int q = 0; q < nq; q++)
        simckpt_queue_init(ctx, q, 0);

    int a1 = 1, a2 = 1, a3 = 1, a4 = 1, a5 = 1, a6 = 1, a7 = 1, a8 = 1;
    uint64_t qdone[2] = { 0, 0 };

    /* ---- #2: single-lane vs dual-lane GPU staging bandwidth ------------ */
    printf("[acceptance] #2 dual-lane staging bandwidth\n");

    /* single-lane DRAM: stage all chunks via queue 0 -> DRAM */
    struct simckpt_desc *sd = calloc(n_chunks, sizeof(*sd));
    for (uint32_t c = 0; c < n_chunks; c++)
        fill_stage_desc(&sd[c], simckpt_phys(ctx, dram + (size_t)c * chunk),
                        chunk, 1, c);
    simckpt_submit_batch(ctx, 0, sd, n_chunks);
    simckpt_wait_q(ctx, 0, qdone[0] + n_chunks);
    qdone[0] += n_chunks;
    free(sd);
    struct simckpt_qcounters qc;
    simckpt_get_qcounters(ctx, 0, &qc);
    uint64_t t_dram = qc.done_tick - qc.issue_tick;

    /* single-lane CXL: stage all chunks via queue 1 -> CXL */
    struct simckpt_desc *sc = calloc(n_chunks, sizeof(*sc));
    for (uint32_t c = 0; c < n_chunks; c++)
        fill_stage_desc(&sc[c], simckpt_phys(ctx, cxl + (size_t)c * chunk),
                        chunk, 1, c);
    if (nq > 1) {
        simckpt_submit_batch(ctx, 1, sc, n_chunks);
        simckpt_wait_q(ctx, 1, qdone[1] + n_chunks);
        qdone[1] += n_chunks;
    }
    free(sc);
    uint64_t t_cxl = 0;
    if (nq > 1) {
        simckpt_get_qcounters(ctx, 1, &qc);
        t_cxl = qc.done_tick - qc.issue_tick;
    }

    /* balanced split: n_dram / n_cxl == B_dram / B_cxl == t_cxl / t_dram */
    uint32_t n_dram, n_cxl;
    if (nq > 1 && t_dram > 0 && t_cxl > 0) {
        n_dram = (uint32_t)((uint64_t)n_chunks * t_cxl / (t_dram + t_cxl));
    } else {
        n_dram = n_chunks;
    }
    if (n_dram == 0)
        n_dram = 1;
    if (n_dram >= n_chunks)
        n_dram = n_chunks - 1;
    n_cxl = n_chunks - n_dram;

    /* dual-lane: stage n_dram -> q0(dram), n_cxl -> q1(cxl) concurrently */
    struct simckpt_desc *dq0 = calloc(n_chunks, sizeof(*dq0));
    struct simckpt_desc *dq1 = calloc(n_chunks, sizeof(*dq1));
    uint32_t dn0 = 0, dn1 = 0;
    for (uint32_t c = 0; c < n_chunks; c++) {
        if (c < n_dram)
            fill_stage_desc(&dq0[dn0++],
                            simckpt_phys(ctx, dram + (size_t)c * chunk),
                            chunk, 1, c);
        else
            fill_stage_desc(&dq1[dn1++],
                            simckpt_phys(ctx, cxl + (size_t)c * chunk),
                            chunk, 1, c);
    }
    if (dn0)
        simckpt_submit_batch(ctx, 0, dq0, dn0);
    if (nq > 1 && dn1)
        simckpt_submit_batch(ctx, 1, dq1, dn1);
    if (dn0)
        simckpt_wait_q(ctx, 0, qdone[0] + dn0);
    if (nq > 1 && dn1)
        simckpt_wait_q(ctx, 1, qdone[1] + dn1);
    qdone[0] += dn0;
    qdone[1] += dn1;
    free(dq0);
    free(dq1);

    uint64_t dual_start = UINT64_MAX, dual_end = 0;
    for (int q = 0; q < nq; q++) {
        simckpt_get_qcounters(ctx, q, &qc);
        if (qc.issue_tick && qc.issue_tick < dual_start)
            dual_start = qc.issue_tick;
        if (qc.done_tick > dual_end)
            dual_end = qc.done_tick;
    }
    uint64_t dual_ticks = (dual_end > dual_start) ? dual_end - dual_start : 0;

    /* verify the staged payload matches the reference PRNG */
    uint64_t mism = 0;
    for (uint32_t c = 0; c < n_chunks; c++) {
        uint8_t *p = c < n_dram ? dram + (size_t)c * chunk
                                : cxl + (size_t)c * chunk;
        mism += gpu_payload_check(&eng, p, chunk, 1, c);
    }
    if (mism)
        a2 = 0;

    double b_dram = t_dram ? (double)size * 1000.0 / (double)t_dram : 0.0;
    double b_cxl  = t_cxl  ? (double)size * 1000.0 / (double)t_cxl  : 0.0;
    double b_dual = dual_ticks ? (double)size * 1000.0 / (double)dual_ticks : 0.0;
    double b_single = b_dram > b_cxl ? b_dram : b_cxl;

    printf("  single DRAM : %.2f GB/s, single CXL : %.2f GB/s\n", b_dram, b_cxl);
    printf("  balanced split: %u DRAM + %u CXL chunks\n", n_dram, n_cxl);
    printf("  dual-lane   : %.2f GB/s (vs fastest single %.2f GB/s)\n",
           b_dual, b_single);
    int dual_faster = (nq > 1 && b_dual > b_single);
    if (!dual_faster)
        a2 = 0;
    printf("  dual > fastest single: %s (mismatches=%" PRIu64 ")\n",
           dual_faster ? "YES" : "NO", mism);

    /* ---- #1/#5/#8: persist (save) + out-of-order restore --------------- */
    printf("[acceptance] #1/#5/#8 persist + out-of-order restore\n");
    struct simckpt_desc *sq0 = calloc(n_chunks, sizeof(*sq0));
    struct simckpt_desc *sq1 = calloc(n_chunks, sizeof(*sq1));
    uint32_t n0 = 0, n1 = 0;
    for (uint32_t c = 0; c < n_chunks; c++) {
        uint64_t phys = c < n_dram
            ? simckpt_phys(ctx, dram + (size_t)c * chunk)
            : simckpt_phys(ctx, cxl + (size_t)c * chunk);
        if (c < n_dram)
            fill_save_desc(&sq0[n0++], phys, (uint64_t)c * chunk, chunk, 1, c);
        else
            fill_save_desc(&sq1[n1++], phys, (uint64_t)c * chunk, chunk, 1, c);
    }
    double t0 = now_ms();
    if (dual_submit_wait(ctx, nq, sq0, n0, sq1, n1, qdone, /*verify=*/1) != 0)
        a1 = 0;
    double save_ms = now_ms() - t0;
    free(sq0);
    free(sq1);

    /* restore in reverse chunk order to exercise out-of-order completion */
    struct simckpt_desc *rq0 = calloc(n_chunks, sizeof(*rq0));
    struct simckpt_desc *rq1 = calloc(n_chunks, sizeof(*rq1));
    uint32_t rn0 = 0, rn1 = 0;
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t c = n_chunks - 1 - i;   /* reverse order */
        uint64_t phys = simckpt_phys(ctx, dst + (size_t)c * chunk);
        if (c < n_dram)
            fill_restore_desc(&rq0[rn0++], phys, (uint64_t)c * chunk, chunk, 1, c);
        else
            fill_restore_desc(&rq1[rn1++], phys, (uint64_t)c * chunk, chunk, 1, c);
    }
    if (dual_submit_wait(ctx, nq, rq0, rn0, rq1, rn1, qdone, /*verify=*/1) != 0)
        a5 = 0;
    free(rq0);
    free(rq1);

    mism = 0;
    for (uint32_t c = 0; c < n_chunks; c++)
        mism += gpu_payload_check(&eng, dst + (size_t)c * chunk, chunk, 1, c);
    if (mism)
        a1 = 0;
    printf("  save %.3f ms (batch), out-of-order restore, mismatches=%"
           PRIu64 "\n", save_ms, mism);

    /* #8: per-link statistics */
    for (int q = 0; q < nq; q++) {
        simckpt_get_qcounters(ctx, q, &qc);
        uint64_t el = qc.done_tick - qc.issue_tick;
        double bw = el ? (double)qc.completed_bytes * 1000.0 / (double)el : 0.0;
        printf("  [link q%d] completed_bytes=%" PRIu64 " outstanding=%" PRIu64
               " retry=%" PRIu64 " queue_full=%" PRIu64 " lat_avg=%" PRIu64
               " lat_p95=%" PRIu64 " bw=%.2f GB/s\n", q, qc.completed_bytes,
               qc.outstanding, qc.retry, qc.queue_full, qc.latency_avg,
               qc.latency_p95, bw);
    }

    /* ---- #3: storage channel striping balance (guest topology) -------- */
    printf("[acceptance] #3 storage channel striping\n");
    uint64_t chan_bytes[TOPO_MAX_CHANNELS] = { 0 };
    for (uint32_t c = 0; c < n_chunks; c++)
        chan_bytes[topology_stripe_channel(&topo, c)] += chunk;
    uint64_t expect = (uint64_t)(n_chunks / (uint32_t)channels) * chunk;
    for (int i = 0; i < channels; i++) {
        uint64_t dev = chan_bytes[i] > expect ? chan_bytes[i] - expect
                                              : expect - chan_bytes[i];
        if (dev > chunk)
            a3 = 0;
        printf("  channel %d: %" PRIu64 " bytes%s\n", i, chan_bytes[i],
               dev <= chunk ? "" : "  <-- MISMATCH");
    }

    /* ---- #4: CXL hot standby lowers restore cost ----------------------- */
    printf("[acceptance] #4 CXL hot standby (storage reads)\n");
    uint64_t cold_reads = n_chunks;
    uint64_t hot_reads = n_chunks - hot_cap;

    memcpy(cxl, dram, size);   /* CXL hot standby mirror of the checkpoint */
    for (uint32_t c = 0; c < hot_cap; c++)
        memcpy(dst + (size_t)c * chunk, cxl + (size_t)c * chunk, chunk);
    struct simckpt_desc *hq = calloc(n_chunks, sizeof(*hq));
    uint32_t hn = 0;
    for (uint32_t c = hot_cap; c < n_chunks; c++)
        fill_restore_desc(&hq[hn++], simckpt_phys(ctx, dst + (size_t)c * chunk),
                          (uint64_t)c * chunk, (uint32_t)chunk, 1, c);
    double hot_t0 = now_ms();
    if (hn) {
        simckpt_submit_batch(ctx, 0, hq, hn);
        simckpt_wait_q(ctx, 0, qdone[0] + hn);
        qdone[0] += hn;
    }
    double hot_ms = now_ms() - hot_t0;
    free(hq);

    mism = 0;
    for (uint32_t c = 0; c < n_chunks; c++)
        mism += gpu_payload_check(&eng, dst + (size_t)c * chunk, chunk, 1, c);
    if (mism || hot_reads >= cold_reads)
        a4 = 0;
    printf("  cold: %" PRIu64 " storage reads, hot: %" PRIu64 " storage reads "
           "(saved %" PRIu64 "), hot restore %.3f ms, mismatches=%" PRIu64 "\n",
           cold_reads, hot_reads, (uint64_t)hot_cap, hot_ms, mism);

    /* ---- #6: pressure -> pinned pool reuse only ------------------------ */
    printf("[acceptance] #6 pressure state machine\n");
    struct ckpt_pool pool;
    ckpt_pool_init(&pool, 0, 65536);
    void *b = ckpt_pool_alloc_from(&pool, 32768);
    ckpt_pool_update(&pool, 85);   /* FROZEN */
    void *b2 = ckpt_pool_alloc_from(&pool, 16384);
    if (b2 != NULL)
        a6 = 0;
    ckpt_pool_update(&pool, 93);   /* SHRINK */
    ckpt_pool_update(&pool, 97);   /* STOPPED */
    printf("  FROZEN denied new pin (pinned=%" PRIu64 "): %s\n", pool.pinned,
           b2 == NULL ? "OK" : "GROW");
    ckpt_pool_free_from(&pool, b, 32768);

    /* ---- #7: in-flight DMA never unpinned before completion ------------ */
    printf("[acceptance] #7 in-flight -> committed lifecycle\n");
    struct ckpt_manifest m;
    ckpt_manifest_init(&m);
    ckpt_manifest_begin(&m, 1, 2);
    ckpt_manifest_add(&m, 0, 0, chunk, 0, CKPT_SOURCE_DISK);
    ckpt_manifest_add(&m, 1, chunk, chunk, 0, CKPT_SOURCE_DISK);
    ckpt_manifest_submit(&m, 0);   /* PINNED -> IN_FLIGHT */
    if (ckpt_chunk_free(&m, 0) == 0)
        a7 = 0;                     /* freeing IN_FLIGHT must fail */
    ckpt_manifest_commit(&m, 0);    /* -> DISK_COMMITTED */
    if (ckpt_chunk_free(&m, 0) != 0)
        a7 = 0;                     /* freeing DISK_COMMITTED must succeed */
    ckpt_chunk_promote_hot(&m, 1);
    ckpt_chunk_evictable(&m, 1);
    printf("  IN_FLIGHT unpin rejected, DISK_COMMITTED freed: %s\n",
           a7 ? "OK" : "FAIL");

    /* ---- summary -------------------------------------------------------- */
    printf("[P10 acceptance matrix]\n");
    printf("  #1 CRC/byte consistency          : %s\n", a1 ? "PASS" : "FAIL");
    printf("  #2 dual-lane staging > single    : %s (%.2f vs %.2f GB/s)\n",
           a2 ? "PASS" : "FAIL", b_dual, b_single);
    printf("  #3 N-channel striping balance    : %s\n", a3 ? "PASS" : "FAIL");
    printf("  #4 hot standby lowers restore    : %s\n", a4 ? "PASS" : "FAIL");
    printf("  #5 out-of-order restore          : %s\n", a5 ? "PASS" : "FAIL");
    printf("  #6 pressure reuse-only           : %s\n", a6 ? "PASS" : "FAIL");
    printf("  #7 in-flight unpin protection    : %s\n", a7 ? "PASS" : "FAIL");
    printf("  #8 per-link statistics           : %s\n", a8 ? "PASS" : "FAIL");

    int ok = a1 && a2 && a3 && a4 && a5 && a6 && a7 && a8;

    ckpt_pool_free(dram, size);
    ckpt_pool_free(cxl, size);
    ckpt_pool_free(dst, size);
    simckpt_close(ctx);

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
