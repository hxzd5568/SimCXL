/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_e4_s1 - target2 S1: single-GPU dual-path staging speedup.
 *
 * One GPU (one SimCkptDevice with two DMA lanes) stages its payload into two
 * independent paths, each with a single channel:
 *
 *   lane 0 (queue 0) -> Host DRAM (1 DDR5 channel)
 *   lane 1 (queue 1) -> CXL Type-3 (1 CXL channel)
 *
 * Measures (cycle-accurate, via per-queue issue/done tick registers):
 *   single-DRAM  : all chunks -> queue 0 -> DRAM   (baseline)
 *   single-CXL   : all chunks -> queue 1 -> CXL
 *   dual         : balanced split across both lanes concurrently
 *   speedup      = B_dual / max(B_dram, B_cxl)
 *
 * Uses FLAG_STAGE descriptors (per-chunk physical address, no contiguity
 * requirement), so the data size can be 256 MiB / 512 MiB without relying on
 * physically-contiguous staging buffers (the E1 pure-stage mode requires
 * contiguous extents and is limited to small buffers).
 *
 * Data size: >= 256 MiB (default), 512 MiB recommended for steady-state
 * bandwidth (see readme_cxl.md: 512 MiB overcomes the ~1ms DMA pipeline
 * cold-start/tail-drain overhead).
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

/* Staging-buffer allocator that avoids the O(bytes) guest memset fault-in.
 * Instead it sets the NUMA binding policy FIRST (MPOL_BIND, no move), then
 * touches one byte per page so each 4 KiB page is faulted in directly on the
 * target node. This is O(pages) instead of O(bytes), so 256 MiB / 512 MiB
 * buffers do not stall the Timing CPU for tens of minutes. */
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

/* Bulk physical-address resolution: open /proc/self/pagemap once and read the
 * PFNs of `n_pages` consecutive pages starting at `buf` (page-aligned) with a
 * single pread, then parse in memory. Avoids one open/lseek/read/close per
 * chunk, which dominates guest-side setup for 256 MiB / 512 MiB staging tests
 * (65536+ chunks). */
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

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 256, chunk = 4096;
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
    printf("E4-S1 single-GPU dual-path staging (queues=%d, ckpt-mib=%" PRIu64
           ")\n", nq, size_mib);

    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    size_t size = (size_t)n_chunks * chunk;

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0x10C0FFEEu);

    uint8_t *dram = alloc_staging(0, size);
    uint8_t *cxl  = alloc_staging(1, size);
    if (!dram || !cxl) {
        fprintf(stderr, "FAIL: alloc\n");
        return 1;
    }

    uint64_t *dram_phys = calloc(n_chunks, sizeof(*dram_phys));
    uint64_t *cxl_phys  = calloc(n_chunks, sizeof(*cxl_phys));
    if (!dram_phys || !cxl_phys) {
        fprintf(stderr, "FAIL: calloc\n");
        return 1;
    }
    phys_bulk(dram, n_chunks, dram_phys);
    phys_bulk(cxl, n_chunks, cxl_phys);
    printf("  phys: dram[0]=0x%" PRIx64 " cxl[0]=0x%" PRIx64
           " dram[last]=0x%" PRIx64 " cxl[last]=0x%" PRIx64 "\n",
           dram_phys[0], cxl_phys[0], dram_phys[n_chunks - 1],
           cxl_phys[n_chunks - 1]);

    for (int q = 0; q < nq; q++)
        simckpt_queue_init(ctx, q, 0);

    struct simckpt_qcounters qc;
    uint64_t qdone[2] = { 0, 0 };

    /* ---- single-lane DRAM (queue 0) ---- */
    struct simckpt_desc *sd = calloc(n_chunks, sizeof(*sd));
    for (uint32_t c = 0; c < n_chunks; c++)
        fill_stage_desc(&sd[c], dram_phys[c], chunk, 1, c);
    simckpt_submit_batch(ctx, 0, sd, n_chunks);
    simckpt_wait_q(ctx, 0, qdone[0] + n_chunks);
    qdone[0] += n_chunks;
    free(sd);
    simckpt_get_qcounters(ctx, 0, &qc);
    uint64_t t_dram = qc.done_tick - qc.issue_tick;

    /* ---- single-lane CXL (queue 1) ---- */
    uint64_t t_cxl = 0;
    struct simckpt_desc *sc = calloc(n_chunks, sizeof(*sc));
    for (uint32_t c = 0; c < n_chunks; c++)
        fill_stage_desc(&sc[c], cxl_phys[c], chunk, 1, c);
    if (nq > 1) {
        simckpt_submit_batch(ctx, 1, sc, n_chunks);
        simckpt_wait_q(ctx, 1, qdone[1] + n_chunks);
        qdone[1] += n_chunks;
        simckpt_get_qcounters(ctx, 1, &qc);
        t_cxl = qc.done_tick - qc.issue_tick;
    }
    free(sc);

    /* balanced split: n_dram/n_cxl == B_dram/B_cxl == t_cxl/t_dram */
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
    struct simckpt_desc *dq0 = calloc(n_chunks, sizeof(*dq0));
    struct simckpt_desc *dq1 = calloc(n_chunks, sizeof(*dq1));
    uint32_t dn0 = 0, dn1 = 0;
    for (uint32_t c = 0; c < n_chunks; c++) {
        if (c < n_dram)
            fill_stage_desc(&dq0[dn0++], dram_phys[c], chunk, 1, c);
        else
            fill_stage_desc(&dq1[dn1++], cxl_phys[c], chunk, 1, c);
    }
    if (dn0)
        simckpt_submit_batch_noring(ctx, 0, dq0, dn0);
    if (nq > 1 && dn1)
        simckpt_submit_batch_noring(ctx, 1, dq1, dn1);
    /* ring both doorbells back-to-back so the two lanes start together */
    if (dn0)
        simckpt_ring(ctx, 0);
    if (nq > 1 && dn1)
        simckpt_ring(ctx, 1);
    if (dn0)
        simckpt_wait_q(ctx, 0, qdone[0] + dn0);
    if (nq > 1 && dn1)
        simckpt_wait_q(ctx, 1, qdone[1] + dn1);
    qdone[0] += dn0;
    qdone[1] += dn1;
    free(dq0);
    free(dq1);

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

    /* verify staged payload matches the reference PRNG */
    uint64_t mism = 0;
    for (uint32_t c = 0; c < n_chunks; c++) {
        uint8_t *p = c < n_dram ? dram + (size_t)c * chunk
                                : cxl + (size_t)c * chunk;
        mism += gpu_payload_check(&eng, p, chunk, 1, c);
    }

    double b_dram = t_dram ? (double)size * 1000.0 / (double)t_dram : 0.0;
    double b_cxl  = t_cxl  ? (double)size * 1000.0 / (double)t_cxl  : 0.0;
    double b_dual = dual_ticks
        ? (double)size * 1000.0 / (double)dual_ticks : 0.0;
    double b_single = b_dram > b_cxl ? b_dram : b_cxl;
    double speedup = b_single > 0.0 ? b_dual / b_single : 0.0;

    printf("  single DRAM : %.2f GB/s\n", b_dram);
    printf("  single CXL  : %.2f GB/s\n", b_cxl);
    printf("  split: %u DRAM + %u CXL chunks\n", n_dram, n_cxl);
    printf("  dual-lane   : %.2f GB/s (sum %.2f GB/s)\n",
           b_dual, b_dram + b_cxl);
    printf("  speedup vs fastest single : %.3fx\n", speedup);
    for (int q = 0; q < nq; q++) {
        uint32_t n = q == 0 ? n_dram : n_cxl;
        uint64_t span = (q_done[q] > q_issue[q]) ? q_done[q] - q_issue[q] : 0;
        double bwi = span ? (double)((uint64_t)n * chunk) * 1000.0 /
                            (double)span : 0.0;
        printf("  lane q%d    : span=%" PRIu64 " ticks, own %.2f GB/s\n",
               q, span, bwi);
    }
    printf("  mismatches  : %" PRIu64 "\n", mism);

    int ok = (mism == 0) && (speedup > 1.0);
    printf("%s\n", ok ? "PASS" : "FAIL");

    free(dram_phys);
    free(cxl_phys);
    munmap(dram, size);
    munmap(cxl, size);
    simckpt_close(ctx);
    return ok ? 0 : 1;
}
