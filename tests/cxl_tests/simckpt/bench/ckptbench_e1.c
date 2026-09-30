/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_e1 - E1 pure-write staging bandwidth experiment.
 *
 * Isolates the "metadata" (descriptor fetch read + completion write) from the
 * payload write path: the device is put into pure-stage mode, where it skips
 * the descriptor DMA-read and the completion DMA-write and only issues
 * sequential 4 KiB payload writes to stageBase + i*chunk.
 *
 * Measures (device-side issue/done ticks, cycle accurate):
 *   single DRAM : all chunks -> queue 0 -> DRAM buffer
 *   single CXL  : all chunks -> queue 1 -> CXL  buffer
 *   dual-lane   : balanced split across queue 0 (DRAM) + queue 1 (CXL)
 *
 * If dual-lane pure-write aggregate approaches B_dram + B_cxl, the P10 #2
 * slowdown was caused by the metadata DMA interleaving on the shared DRAM
 * controller. If it stays ~25 GB/s, the cause is elsewhere (shared Ruby /
 * intrinsic two-lane contention).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ckptd.h"
#include "gpu_dma_engine.h"
#include "libckpt.h"

/* Expected payload byte at offset j of a chunk (matches the device's PRNG
 * generation in gpu_dma_engine.c). */
static uint8_t
expected_byte(uint32_t chunk, size_t j)
{
    uint32_t w = gpu_payload_word(1, chunk, (uint32_t)(j / 4));
    return (uint8_t)(w >> (8 * (j % 4)));
}

static double
gbps(size_t bytes, uint64_t ticks)
{
    return ticks ? (double)bytes * 1000.0 / (double)ticks : 0.0;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 4, chunk = 4096;
    const char *bar0 = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ckpt-mib") && i + 1 < argc)
            size_mib = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = strtoull(argv[++i], NULL, 0);
        else if (!bar0)
            bar0 = argv[i];
    }

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    int nq = simckpt_num_queues(ctx);
    if (nq > 2)
        nq = 2;
    printf("E1 pure-write staging (queues=%d)\n", nq);

    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    size_t size = (size_t)n_chunks * chunk;

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0x10C0FFEEu);

    uint8_t *dram = ckpt_pool_alloc(0, size);
    uint8_t *cxl  = ckpt_pool_alloc(1, size);
    if (!dram || !cxl) {
        fprintf(stderr, "FAIL: alloc\n");
        return 1;
    }

    /* Contiguity check: the pure-stage device writes to stageBase+i*chunk,
     * which is only correct if the physical pages are contiguous. The CXL
     * node here hands out 2 MiB extents, so a larger buffer is fragmented. */
    uint64_t dram_base = simckpt_phys(ctx, dram);
    uint64_t cxl_base  = simckpt_phys(ctx, cxl);
    uint64_t end_off = (uint64_t)(n_chunks - 1) * chunk;
    uint64_t dram_last = simckpt_phys(ctx, dram + end_off);
    uint64_t cxl_last  = simckpt_phys(ctx, cxl + end_off);
    int dram_contig = (dram_last == dram_base + end_off);
    int cxl_contig  = (cxl_last  == cxl_base  + end_off);
    printf("  contiguity: dram 0x%" PRIx64 "..0x%" PRIx64 " %s, "
           "cxl 0x%" PRIx64 "..0x%" PRIx64 " %s\n",
           dram_base, dram_last, dram_contig ? "contig" : "NON-CONTIG",
           cxl_base, cxl_last, cxl_contig ? "contig" : "NON-CONTIG");
    if (!dram_contig || !cxl_contig) {
        fprintf(stderr, "FAIL: staging buffer not physically contiguous; "
                "shrink --ckpt-mib\n");
        simckpt_close(ctx);
        return 1;
    }

    for (int q = 0; q < nq; q++)
        simckpt_queue_init(ctx, q, 0);

    struct simckpt_qcounters qc;
    uint64_t qdone[2] = { 0, 0 };

    /* ---- single-lane DRAM (queue 0) ---- */
    simckpt_set_stage_base(ctx, 0, simckpt_phys(ctx, dram));
    simckpt_set_pure_stage(ctx, 0, 1);
    simckpt_stage_doorbell(ctx, 0, n_chunks);
    simckpt_wait_q(ctx, 0, qdone[0] + n_chunks);
    qdone[0] += n_chunks;
    simckpt_get_qcounters(ctx, 0, &qc);
    uint64_t t_dram = qc.done_tick - qc.issue_tick;

    /* ---- single-lane CXL (queue 1) ---- */
    uint64_t t_cxl = 0;
    if (nq > 1) {
        simckpt_set_stage_base(ctx, 1, simckpt_phys(ctx, cxl));
        simckpt_set_pure_stage(ctx, 1, 1);
        simckpt_stage_doorbell(ctx, 1, n_chunks);
        simckpt_wait_q(ctx, 1, qdone[1] + n_chunks);
        qdone[1] += n_chunks;
        simckpt_get_qcounters(ctx, 1, &qc);
        t_cxl = qc.done_tick - qc.issue_tick;
    }

    /* ---- balanced split (same formula as ckptbench_p10 #2) ---- */
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

    /* ---- dual-lane (both queues concurrently) ---- */
    simckpt_set_stage_base(ctx, 0, simckpt_phys(ctx, dram));
    simckpt_set_pure_stage(ctx, 0, 1);
    simckpt_set_stage_base(ctx, 1, simckpt_phys(ctx, cxl));
    simckpt_set_pure_stage(ctx, 1, 1);
    simckpt_stage_doorbell(ctx, 0, n_dram);
    if (nq > 1)
        simckpt_stage_doorbell(ctx, 1, n_cxl);
    simckpt_wait_q(ctx, 0, qdone[0] + n_dram);
    if (nq > 1)
        simckpt_wait_q(ctx, 1, qdone[1] + n_cxl);
    qdone[0] += n_dram;
    qdone[1] += n_cxl;

    uint64_t dual_start = UINT64_MAX, dual_end = 0;
    uint64_t q_issue[2] = { 0, 0 }, q_done[2] = { 0, 0 };
    for (int q = 0; q < nq; q++) {
        simckpt_get_qcounters(ctx, q, &qc);
        q_issue[q] = qc.issue_tick;
        q_done[q] = qc.done_tick;
        if (qc.issue_tick && qc.issue_tick < dual_start)
            dual_start = qc.issue_tick;
        if (qc.done_tick > dual_end)
            dual_end = qc.done_tick;
    }
    uint64_t dual_ticks = (dual_end > dual_start) ? dual_end - dual_start : 0;

    /* ---- verify (each lane independently 0-indexed in pure-stage) ---- */
    uint64_t mism = 0;
    for (uint32_t i = 0; i < n_dram; i++)
        mism += gpu_payload_check(&eng, dram + (size_t)i * chunk, chunk, 1, i);
    for (uint32_t i = 0; i < n_cxl; i++)
        mism += gpu_payload_check(&eng, cxl + (size_t)i * chunk, chunk, 1, i);

    /* Find and dump the first mismatching byte, if any. */
    if (mism) {
        int done = 0;
        for (uint32_t i = 0; i < n_dram && !done; i++) {
            const uint8_t *p = dram + (size_t)i * chunk;
            for (size_t j = 0; j < chunk && !done; j++) {
                uint8_t exp = expected_byte(i, j);
                if (p[j] != exp) {
                    printf("  first mismatch: DRAM chunk %u byte %zu "
                           "exp=0x%02x got=0x%02x\n",
                           i, j, exp, p[j]);
                    done = 1;
                }
            }
        }
        for (uint32_t i = 0; i < n_cxl && !done; i++) {
            const uint8_t *p = cxl + (size_t)i * chunk;
            for (size_t j = 0; j < chunk && !done; j++) {
                uint8_t exp = expected_byte(i, j);
                if (p[j] != exp) {
                    printf("  first mismatch: CXL chunk %u byte %zu "
                           "exp=0x%02x got=0x%02x\n",
                           i, j, exp, p[j]);
                    done = 1;
                }
            }
        }
    }

    double b_dram = gbps(size, t_dram);
    double b_cxl  = gbps(size, t_cxl);
    double b_dual = gbps(size, dual_ticks);
    double b_single = b_dram > b_cxl ? b_dram : b_cxl;

    printf("  single DRAM : %.2f GB/s\n", b_dram);
    printf("  single CXL  : %.2f GB/s\n", b_cxl);
    printf("  split: %u DRAM + %u CXL chunks\n", n_dram, n_cxl);
    printf("  dual-lane   : %.2f GB/s (vs fastest single %.2f GB/s, "
           "sum %.2f GB/s)\n", b_dual, b_single, b_dram + b_cxl);
    for (int q = 0; q < nq; q++) {
        uint32_t n = q == 0 ? n_dram : n_cxl;
        uint64_t span = (q_done[q] > q_issue[q]) ? q_done[q] - q_issue[q] : 0;
        double bwi = span ? (double)((uint64_t)n * chunk) * 1000.0 /
                            (double)span : 0.0;
        printf("  lane q%d    : span=%" PRIu64 " ticks, own %.2f GB/s "
               "(issue=%" PRIu64 ", done=%" PRIu64 ")\n",
               q, span, bwi, q_issue[q], q_done[q]);
    }
    printf("  mismatches  : %" PRIu64 "\n", mism);

    int ok = (mism == 0);
    printf("%s\n", ok ? "PASS" : "FAIL");

    ckpt_pool_free(dram, size);
    ckpt_pool_free(cxl, size);
    simckpt_close(ctx);
    return ok ? 0 : 1;
}
