/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_e6_budget - target3 CXL-memory-budget sweep (write staging).
 *
 * Generalizes S2 to N GPUs (4 or 8) simultaneously writing, with a variable
 * number of CXL P2P lanes and an *optimal* load split between host DRAM
 * (through the shared switch uplink) and CXL (P2P, on-device DMA).
 *
 * Topology (configured by x86-cxl-ckptd-e6.py / x86_board.py S2):
 *   queues 0 .. N-1     : N GPU DRAM lanes, each Gen4 x16 (27.77 GB/s),
 *                         fanned into ONE shared Gen5 x16 uplink (55.53)
 *   queues N .. N+M-1   : M CXL P2P lanes, each Gen5 x16 (on-device DMA)
 *
 * Measures (cycle-accurate, per-queue issue/done tick registers):
 *   DRAM-only : all N GPUs -> their DRAM lanes -> shared uplink -> DRAM
 *   CXL-only  : all N GPUs -> CXL lanes -> CXL backend (optional probe)
 *   dual      : single-cut per GPU: each GPU's cpg chunks are cut once at the
 *               4KB-aligned boundary n_d = round(cpg * f_dram); the first n_d
 *               chunks -> its DRAM lane, the remaining cpg-n_d -> a CXL lane
 *               (approximate water-fill, matching the real 4KB-granularity
 *               software split).
 *
 * The optimal split fraction f_dram is passed in via --split-dram-frac (the
 * config computes it from the theoretical line rates) or, when omitted and
 * --probe-cxl is given, self-calibrated from the measured single-path rates.
 *
 * Data size: total MiB spread evenly over the N GPUs (default 32 MiB for the
 * fast sweep; 512 MiB for the full-scale confirmation endpoints).
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

#define MAX_GPUS 8
#define MAX_CXL  8

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
        out[i] = (e & 0x7fffffffffffffffULL) == 0
                     ? (uint64_t)-1
                     : (e & 0x7fffffffffffffffULL) * (uint64_t)page_size;
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

/* Route of one GPU in a staging phase. */
struct route {
    int q;    /* target queue index */
    int cxl;  /* 1 = dst in CXL node, 0 = dst in DRAM node */
};

/* Run one phase: submit every GPU's `cpg` descriptors, ring all touched queues
 * back-to-back, wait, and return the phase span (min issue_tick .. max
 * done_tick) across the touched queues.
 *
 * `n_d < 0` -> whole-GPU routing: each GPU's full `cpg` chunks go to the queue
 * given by `rt[g]`. `n_d >= 0` -> single-cut routing: each GPU's `cpg` chunks
 * are cut once at the 4KB-aligned boundary `n_d` (first `n_d` chunks -> its
 * DRAM lane, the remaining `cpg - n_d` -> a CXL lane). */
static void
run_phase(struct simckpt_ctx *ctx, int n_gpu, int n_cxl, uint32_t cpg,
          uint32_t chunk, uint8_t **dram, uint8_t **cxl,
          uint64_t **dram_phys, uint64_t **cxl_phys,
          int n_d, const struct route *rt, uint64_t *qdone,
          uint64_t *start_out, uint64_t *end_out)
{
    int nq = n_gpu + n_cxl;
    int touched[MAX_GPUS + MAX_CXL] = { 0 };
    uint32_t per_q[MAX_GPUS + MAX_CXL] = { 0 };

    struct simckpt_desc *desc = calloc(cpg, sizeof(*desc));
    if (!desc) {
        fprintf(stderr, "FAIL: desc calloc\n");
        exit(1);
    }

    /* Submit (noring) each GPU's chunks. */
    for (int g = 0; g < n_gpu; g++) {
        if (n_d >= 0) {
            /* Single-cut: one 4KB-aligned cut per GPU. */
            int q_dram = g;
            int q_cxl = n_gpu + (g % n_cxl);
            uint32_t nd = (uint32_t)n_d;
            for (uint32_t c = 0; c < nd; c++)
                fill_stage_desc(&desc[c], dram_phys[g][c], chunk, 1, c);
            simckpt_submit_batch_noring(ctx, q_dram, desc, nd);
            touched[q_dram] = 1;
            per_q[q_dram] += nd;

            for (uint32_t c = nd; c < cpg; c++)
                fill_stage_desc(&desc[c - nd], cxl_phys[g][c], chunk, 1, c);
            simckpt_submit_batch_noring(ctx, q_cxl, desc, cpg - nd);
            touched[q_cxl] = 1;
            per_q[q_cxl] += cpg - nd;
        } else {
            uint8_t *dst_base = rt[g].cxl ? cxl[g] : dram[g];
            uint64_t *dst_phys = rt[g].cxl ? cxl_phys[g] : dram_phys[g];
            for (uint32_t c = 0; c < cpg; c++)
                fill_stage_desc(&desc[c], dst_phys[c], chunk, 1, c);
            simckpt_submit_batch_noring(ctx, rt[g].q, desc, cpg);
            touched[rt[g].q] = 1;
            per_q[rt[g].q] += cpg;
            (void)dst_base;
        }
    }

    /* Ring all touched queues back-to-back. */
    for (int q = 0; q < nq; q++)
        if (touched[q])
            simckpt_ring(ctx, q);

    /* Wait for each touched queue. */
    for (int q = 0; q < nq; q++)
        if (touched[q]) {
            simckpt_wait_q(ctx, q, qdone[q] + per_q[q]);
            qdone[q] += per_q[q];
        }

    /* Span = min issue .. max done across touched queues. */
    struct simckpt_qcounters qc;
    uint64_t s = UINT64_MAX, e = 0;
    for (int q = 0; q < nq; q++) {
        if (!touched[q])
            continue;
        simckpt_get_qcounters(ctx, q, &qc);
        if (qc.issue_tick && qc.issue_tick < s)
            s = qc.issue_tick;
        if (qc.done_tick > e)
            e = qc.done_tick;
    }
    *start_out = s;
    *end_out = e;
    free(desc);
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 32, chunk = 4096;
    int n_gpu = 4, n_cxl = 1;
    double split_frac = -1.0;
    int probe_cxl = 0;
    const char *bar0 = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gpus") && i + 1 < argc)
            n_gpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cxl-lanes") && i + 1 < argc)
            n_cxl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ckpt-mib") && i + 1 < argc)
            size_mib = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--split-dram-frac") && i + 1 < argc)
            split_frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--probe-cxl"))
            probe_cxl = 1;
        else if (!bar0)
            bar0 = argv[i];
    }

    if (n_gpu < 1 || n_gpu > MAX_GPUS || n_cxl < 1 || n_cxl > MAX_CXL) {
        fprintf(stderr, "FAIL: gpus in [1,%d], cxl-lanes in [1,%d]\n",
                MAX_GPUS, MAX_CXL);
        return 1;
    }

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    int nq = simckpt_num_queues(ctx);
    printf("E6-budget N=%d GPU + %d CXL staging (queues=%d, ckpt-mib=%" PRIu64
           ")\n", n_gpu, n_cxl, nq, size_mib);
    if (nq < n_gpu + n_cxl) {
        fprintf(stderr, "FAIL: expected >=%d queues, got %d\n",
                n_gpu + n_cxl, nq);
        return 1;
    }

    uint32_t n_chunks = (uint32_t)((size_mib << 20) / chunk);
    uint32_t cpg = n_chunks / n_gpu; /* chunks per GPU */
    size_t gpu_bytes = (size_t)cpg * chunk;

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0x10C0FFEEu);

    uint8_t *dram[MAX_GPUS], *cxl[MAX_GPUS];
    uint64_t *dram_phys[MAX_GPUS], *cxl_phys[MAX_GPUS];
    for (int g = 0; g < n_gpu; g++) {
        dram[g] = alloc_staging(0, gpu_bytes);
        dram_phys[g] = calloc(cpg, sizeof(**dram_phys));
        if (!dram[g] || !dram_phys[g] ||
            phys_bulk(dram[g], cpg, dram_phys[g]) != 0) {
            fprintf(stderr, "FAIL: DRAM alloc gpu %d\n", g);
            return 1;
        }
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

    uint64_t qdone[MAX_GPUS + MAX_CXL] = { 0 };
    struct route rt[MAX_GPUS];
    uint64_t s, e;
    size_t total = (size_t)n_gpu * cpg * chunk;

    /* ---- DRAM-only baseline: all GPUs -> DRAM lanes ---- */
    for (int g = 0; g < n_gpu; g++) {
        rt[g].q = g;
        rt[g].cxl = 0;
    }
    run_phase(ctx, n_gpu, n_cxl, cpg, chunk, dram, cxl, dram_phys, cxl_phys,
              -1, rt, qdone, &s, &e);
    double b_dram_only = gbps(total, e > s ? e - s : 0);

    /* ---- optional CXL-only probe ---- */
    double b_cxl_only = 0.0;
    if (probe_cxl) {
        for (int g = 0; g < n_gpu; g++) {
            rt[g].q = n_gpu + (g % n_cxl);
            rt[g].cxl = 1;
        }
        run_phase(ctx, n_gpu, n_cxl, cpg, chunk, dram, cxl, dram_phys,
                  cxl_phys, -1, rt, qdone, &s, &e);
        b_cxl_only = gbps(total, e > s ? e - s : 0);
    }

    /* ---- dual: single-cut per GPU (4KB-aligned water-fill) ---- */
    double f = split_frac;
    if (f < 0.0) {
        if (probe_cxl && b_dram_only > 0 && b_cxl_only > 0)
            f = b_dram_only / (b_dram_only + b_cxl_only);
        else
            f = 0.5;
    }
    /* One cut point per GPU (4KB-aligned, integer chunk count). */
    int n_d = (int)((double)cpg * f + 0.5);
    if (n_d < 1)
        n_d = 1;
    if (n_d >= (int)cpg)
        n_d = (int)cpg - 1;
    run_phase(ctx, n_gpu, n_cxl, cpg, chunk, dram, cxl, dram_phys, cxl_phys,
              n_d, rt, qdone, &s, &e);
    double b_dual = gbps(total, e > s ? e - s : 0);

    /* verify payloads: DRAM chunks [0, n_d), CXL chunks [n_d, cpg) */
    uint64_t mism = 0;
    for (int g = 0; g < n_gpu; g++) {
        for (uint32_t c = 0; c < (uint32_t)n_d; c++)
            mism += gpu_payload_check(&eng, dram[g] + (size_t)c * chunk,
                                      chunk, 1, c);
        for (uint32_t c = (uint32_t)n_d; c < cpg; c++)
            mism += gpu_payload_check(&eng, cxl[g] + (size_t)c * chunk,
                                      chunk, 1, c);
    }

    double speedup = b_dram_only > 0 ? b_dual / b_dram_only : 0.0;

    printf("  DRAM-only : %.2f GB/s (%d GPU -> uplink -> DRAM)\n",
           b_dram_only, n_gpu);
    if (probe_cxl)
        printf("  CXL-only  : %.2f GB/s (%d GPU -> %d CXL lane(s))\n",
               b_cxl_only, n_gpu, n_cxl);
    printf("  split     : %d DRAM + %d CXL chunks/GPU (f_dram=%.3f)\n",
           n_d, (int)cpg - n_d, f);
    printf("  dual      : %.2f GB/s\n", b_dual);
    printf("  speedup vs DRAM-only : %.3fx\n", speedup);
    printf("  mismatches : %" PRIu64 "\n", mism);

    int ok = (mism == 0) && (speedup > 1.0);
    printf("%s\n", ok ? "PASS" : "FAIL");

    for (int g = 0; g < n_gpu; g++) {
        free(dram_phys[g]);
        free(cxl_phys[g]);
        munmap(dram[g], gpu_bytes);
        munmap(cxl[g], gpu_bytes);
    }
    simckpt_close(ctx);
    return ok ? 0 : 1;
}
