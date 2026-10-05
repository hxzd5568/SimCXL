/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_e5_s2 - target2 S2: 4-GPU dual-path staging speedup under a
 * shared switch uplink (E5), Gen4 GPU links (E3), CXL P2P + CXL.io (E2).
 *
 * Topology (configured by x86-cxl-ckptd-e5.py):
 *   queues 0..3 : 4 GPU lanes, each Gen4 x16 (27.77 GB/s). Their DRAM-bound
 *                 traffic fans into ONE shared Gen5 x16 switch uplink
 *                 (55.53 GB/s) via a NoncoherentXBar -> the shared uplink is
 *                 the bottleneck for DRAM-only staging.
 *   queues 4..5 : 2 CXL P2P lanes, each direct Gen5 x16 (on-device DMA, no
 *                 uplink) with a one-time CXL.io initial-command latency.
 *
 * Measures (cycle-accurate, per-queue issue/done tick registers):
 *   DRAM-only : all 4 GPU lanes (queues 0..3) stage to Host DRAM through the
 *               shared uplink  -> expect ~55.53 GB/s (uplink-limited)
 *   dual      : 2 GPU lanes (queues 0,1) -> DRAM (uplink) + 2 GPU lanes
 *               (queues 4,5) -> CXL (P2P)  -> expect ~111 GB/s (~2.0x)
 *
 * Uses FLAG_STAGE descriptors (per-chunk physical address, no contiguity
 * requirement) so the staging buffers may be fragmented (CXL node 1 hands out
 * 2 MiB extents).
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "ckptd.h"
#include "gpu_dma_engine.h"
#include "libckpt.h"

#ifndef SYS_mbind
#define SYS_mbind 237
#endif
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT (1 << 0)
#endif

#define MAX_GPUS 4
#define MAX_CXL 2

/* NUMA-bound staging allocator (mbind first, touch one byte per page). */
static void *
alloc_staging(int node, size_t size)
{
    void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
        return NULL;
    unsigned long mask = 1UL << node;
    syscall(SYS_mbind, buf, size, MPOL_BIND, &mask, 64, MPOL_MF_STRICT);
    long ps = sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < size; off += (size_t)ps)
        ((volatile unsigned char *)buf)[off] = 0;
    return buf;
}

/* Bulk /proc/self/pagemap read of `n_pages` consecutive PFNs. */
static int
phys_bulk(uint8_t *buf, uint32_t n_pages, uint64_t *out)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0)
        return -1;
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned long base_pfn = (unsigned long)buf / (unsigned long)page_size;

    uint64_t *entries = calloc(n_pages, sizeof(*entries));
    if (!entries) {
        close(fd);
        return -1;
    }
    off_t off = (off_t)base_pfn * 8;
    ssize_t want = (ssize_t)n_pages * 8;
    ssize_t got = pread(fd, entries, want, off);
    close(fd);
    if (got != want) {
        free(entries);
        return -1;
    }
    for (uint32_t i = 0; i < n_pages; i++) {
        uint64_t e = entries[i];
        if ((e & 0x7fffffffffffffffULL) == 0)
            out[i] = (uint64_t)-1;
        else
            out[i] = (e & 0x7fffffffffffffffULL) * (uint64_t)page_size;
    }
    free(entries);
    return 0;
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

static double
gbps(size_t bytes, uint64_t ticks)
{
    return ticks ? (double)bytes * 1000.0 / (double)ticks : 0.0;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 64, chunk = 4096;
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
    printf("E5-S2 4-GPU shared-uplink staging (queues=%d, ckpt-mib=%" PRIu64
           ")\n", nq, size_mib);
    if (nq < 6) {
        fprintf(stderr, "FAIL: expected >=6 queues (4 DRAM + 2 CXL), got %d\n",
                nq);
        return 1;
    }

    /* Total data spread over 4 GPUs; each GPU stages `cpg` chunks. */
    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    uint32_t cpg = n_chunks / MAX_GPUS; /* chunks per GPU */
    size_t gpu_bytes = (size_t)cpg * chunk;

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0x10C0FFEEu);

    /* Per-GPU staging buffers: 4 on DRAM (node 0), 2 on CXL (node 1). */
    uint8_t *dram[MAX_GPUS], *cxl[MAX_CXL];
    uint64_t *dram_phys[MAX_GPUS], *cxl_phys[MAX_CXL];
    for (int g = 0; g < MAX_GPUS; g++) {
        dram[g] = alloc_staging(0, gpu_bytes);
        dram_phys[g] = calloc(cpg, sizeof(**dram_phys));
        if (!dram[g] || !dram_phys[g] ||
            phys_bulk(dram[g], cpg, dram_phys[g]) != 0) {
            fprintf(stderr, "FAIL: DRAM alloc gpu %d\n", g);
            return 1;
        }
    }
    for (int g = 0; g < MAX_CXL; g++) {
        cxl[g] = alloc_staging(1, gpu_bytes);
        cxl_phys[g] = calloc(cpg, sizeof(**cxl_phys));
        if (!cxl[g] || !cxl_phys[g] ||
            phys_bulk(cxl[g], cpg, cxl_phys[g]) != 0) {
            fprintf(stderr, "FAIL: CXL alloc gpu %d\n", g);
            return 1;
        }
    }

    for (int q = 0; q < nq; q++)
        simckpt_queue_init(ctx, q, 0);

    struct simckpt_qcounters qc;
    uint64_t qdone[6] = { 0, 0, 0, 0, 0, 0 };

    /* ---- DRAM-only: 4 GPU lanes (queues 0..3) ---- */
    /* ----   -> shared uplink -> DRAM             ---- */
    struct simckpt_desc *dq[4];
    for (int g = 0; g < MAX_GPUS; g++) {
        dq[g] = calloc(cpg, sizeof(**dq));
        for (uint32_t c = 0; c < cpg; c++)
            fill_stage_desc(&dq[g][c], dram_phys[g][c], chunk, 1, c);
        simckpt_submit_batch_noring(ctx, g, dq[g], cpg);
    }
    for (int g = 0; g < MAX_GPUS; g++)
        simckpt_ring(ctx, g);
    for (int g = 0; g < MAX_GPUS; g++) {
        simckpt_wait_q(ctx, g, qdone[g] + cpg);
        qdone[g] += cpg;
        free(dq[g]);
    }

    uint64_t d_start = UINT64_MAX, d_end = 0;
    for (int g = 0; g < MAX_GPUS; g++) {
        simckpt_get_qcounters(ctx, g, &qc);
        if (qc.issue_tick && qc.issue_tick < d_start)
            d_start = qc.issue_tick;
        if (qc.done_tick > d_end)
            d_end = qc.done_tick;
    }
    uint64_t d_ticks = (d_end > d_start) ? d_end - d_start : 0;
    size_t total = (size_t)MAX_GPUS * cpg * chunk;
    double b_dram_only = gbps(total, d_ticks);

    /* verify DRAM-only payload */
    uint64_t mism = 0;
    for (int g = 0; g < MAX_GPUS; g++)
        for (uint32_t c = 0; c < cpg; c++)
            mism += gpu_payload_check(&eng, dram[g] + (size_t)c * chunk,
                                      chunk, 1, c);

    /* ---- dual: 2 GPU lanes (0,1) -> DRAM + 2 GPU lanes (4,5) -> CXL ---- */
    struct simckpt_desc *du0 = calloc(cpg, sizeof(*du0));
    struct simckpt_desc *du1 = calloc(cpg, sizeof(*du1));
    struct simckpt_desc *cu0 = calloc(cpg, sizeof(*cu0));
    struct simckpt_desc *cu1 = calloc(cpg, sizeof(*cu1));
    for (uint32_t c = 0; c < cpg; c++) {
        fill_stage_desc(&du0[c], dram_phys[0][c], chunk, 1, c);
        fill_stage_desc(&du1[c], dram_phys[1][c], chunk, 1, c);
        fill_stage_desc(&cu0[c], cxl_phys[0][c], chunk, 1, c);
        fill_stage_desc(&cu1[c], cxl_phys[1][c], chunk, 1, c);
    }
    simckpt_submit_batch_noring(ctx, 0, du0, cpg);
    simckpt_submit_batch_noring(ctx, 1, du1, cpg);
    simckpt_submit_batch_noring(ctx, 4, cu0, cpg);
    simckpt_submit_batch_noring(ctx, 5, cu1, cpg);
    /* ring all four doorbells back-to-back so the lanes start together */
    simckpt_ring(ctx, 0);
    simckpt_ring(ctx, 1);
    simckpt_ring(ctx, 4);
    simckpt_ring(ctx, 5);
    simckpt_wait_q(ctx, 0, qdone[0] + cpg);
    simckpt_wait_q(ctx, 1, qdone[1] + cpg);
    simckpt_wait_q(ctx, 4, qdone[4] + cpg);
    simckpt_wait_q(ctx, 5, qdone[5] + cpg);
    qdone[0] += cpg;
    qdone[1] += cpg;
    qdone[4] += cpg;
    qdone[5] += cpg;
    free(du0);
    free(du1);
    free(cu0);
    free(cu1);

    uint64_t du_start = UINT64_MAX, du_end = 0;
    int lanes[4] = { 0, 1, 4, 5 };
    uint64_t lane_span[6] = { 0 };
    for (int k = 0; k < 4; k++) {
        int q = lanes[k];
        simckpt_get_qcounters(ctx, q, &qc);
        if (qc.issue_tick && qc.issue_tick < du_start)
            du_start = qc.issue_tick;
        if (qc.done_tick > du_end)
            du_end = qc.done_tick;
        lane_span[q] = (qc.done_tick > qc.issue_tick)
                           ? qc.done_tick - qc.issue_tick
                           : 0;
    }
    uint64_t du_ticks = (du_end > du_start) ? du_end - du_start : 0;
    double b_dual = gbps(total, du_ticks);

    for (int g = 0; g < 2; g++)
        for (uint32_t c = 0; c < cpg; c++) {
            mism += gpu_payload_check(&eng, dram[g] + (size_t)c * chunk,
                                      chunk, 1, c);
            mism += gpu_payload_check(&eng, cxl[g] + (size_t)c * chunk,
                                      chunk, 1, c);
        }

    double speedup = b_dram_only > 0.0 ? b_dual / b_dram_only : 0.0;

    printf("  DRAM-only   : %.2f GB/s (4 GPU -> shared uplink -> DRAM)\n",
           b_dram_only);
    printf("  dual-lane   : %.2f GB/s (2 GPU -> DRAM + 2 GPU -> CXL P2P)\n",
           b_dual);
    for (int k = 0; k < 4; k++) {
        int q = lanes[k];
        uint32_t n = cpg;
        double bwi = lane_span[q]
                         ? (double)((uint64_t)n * chunk) * 1000.0 /
                               (double)lane_span[q]
                         : 0.0;
        printf("  lane q%d    : span=%" PRIu64 " ticks, own %.2f GB/s\n",
               q, lane_span[q], bwi);
    }
    printf("  speedup vs DRAM-only : %.3fx\n", speedup);
    printf("  mismatches  : %" PRIu64 "\n", mism);

    int ok = (mism == 0) && (speedup > 1.5);
    printf("%s\n", ok ? "PASS" : "FAIL");

    for (int g = 0; g < MAX_GPUS; g++) {
        free(dram_phys[g]);
        munmap(dram[g], gpu_bytes);
    }
    for (int g = 0; g < MAX_CXL; g++) {
        free(cxl_phys[g]);
        munmap(cxl[g], gpu_bytes);
    }
    simckpt_close(ctx);
    return ok ? 0 : 1;
}
