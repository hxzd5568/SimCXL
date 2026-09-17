/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p8 - P8: analysis model + cycle-simulation co-design.
 *
 * Generates a parameterized checkpoint trace (frequency/size/chunk_size/
 * hot-cold distribution, LLMServingSim request-trace style), drives the real
 * SimCkptDevice through it, and reports the four target.md P8 metrics:
 *
 *   - checkpoint save time + save bandwidth
 *   - restore time (time-to-resume)
 *   - CXL hot-standby hit rate
 *   - per-chunk P95 latency
 *
 * It also prints the analytical model prediction (trace_gen.c) for the *same*
 * trace, so the cycle-sim sample can be compared against the analysis model
 * directly (sampling validation of the key points).
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
#include "trace_gen.h"

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    uint64_t size_mib = 1, chunk = 4096, interval = 1, num = 3;
    double hot_frac = 0.5;
    const char *bar0 = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ckpt-mib") && i + 1 < argc)
            size_mib = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc)
            interval = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--num") && i + 1 < argc)
            num = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hot-frac") && i + 1 < argc)
            hot_frac = atof(argv[++i]);
        else if (!bar0)
            bar0 = argv[i];
    }

    struct ckpt_trace t;
    trace_generate(&t, size_mib << 20, chunk, interval, (uint32_t)num,
                   hot_frac);

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened (trace: %u ckpts x %" PRIu64
           " MiB, %u chunks, hot_frac %.2f)\n",
           t.num_ckpts, size_mib, t.n_chunks, hot_frac);

    struct gpu_dma_engine eng;
    gpu_dma_engine_init(&eng, 0xDEADBEEFu);

    size_t size = (size_t)t.n_chunks * chunk;
    uint8_t *dram = ckpt_pool_alloc(0, size);   /* staging (DRAM)      */
    uint8_t *cxl  = ckpt_pool_alloc(1, size);   /* CXL hot standby     */
    uint8_t *dst  = ckpt_pool_alloc(0, size);   /* restore destination */
    double *lat = malloc((size_t)t.n_chunks * t.num_ckpts * sizeof(double));

    if (!dram || !cxl || !dst || !lat) {
        fprintf(stderr, "FAIL: alloc\n");
        return 1;
    }

    /* --- save phase: replay the trace, timing each chunk ------------- */
    uint64_t nlat = 0;
    double save_ms = 0;
    for (uint32_t g = 1; g <= t.num_ckpts; g++) {
        for (uint32_t c = 0; c < t.n_chunks; c++)
            gpu_payload_gen(&eng, dram + (size_t)c * chunk, chunk, g, c);

        for (uint32_t c = 0; c < t.n_chunks; c++) {
            uint32_t crc = 0;
            double t0 = now_ms();
            if (simckpt_save(ctx, dram + (size_t)c * chunk,
                             (uint64_t)(g - 1) * size + (uint64_t)c * chunk,
                             (uint32_t)chunk, g, c, &crc) != 0) {
                printf("FAIL: save ckpt %u chunk %u\n", g, c);
                return 1;
            }
            lat[nlat++] = now_ms() - t0;
            save_ms += lat[nlat - 1];
        }

        /* mirror the latest checkpoint into the CXL hot standby */
        if (g == t.num_ckpts)
            memcpy(cxl, dram, size);
    }

    qsort(lat, nlat, sizeof(double), cmp_double);
    double p95 = lat[(size_t)(0.95 * (double)nlat)];
    double mean_chunk_us = save_ms / (double)nlat * 1e3;   /* accurate (aggregate) */
    double save_bw = (double)((uint64_t)size * t.num_ckpts) / save_ms / 1e6;

    /* --- restore phase: time-to-resume + hot hit rate --------------- */
    uint32_t hits = 0;
    uint64_t base = (uint64_t)(t.num_ckpts - 1) * size;
    double t0 = now_ms();
    for (uint32_t c = 0; c < t.n_chunks; c++) {
        if (trace_chunk_is_hot(&t, t.num_ckpts, c)) {
            memcpy(dst + (size_t)c * chunk, cxl + (size_t)c * chunk, chunk);
            hits++;
        } else {
            if (simckpt_restore(ctx, dst + (size_t)c * chunk,
                                base + (uint64_t)c * chunk, (uint32_t)chunk,
                                t.num_ckpts, c, NULL) != 0) {
                printf("FAIL: restore chunk %u\n", c);
                return 1;
            }
        }
    }
    double resume_ms = now_ms() - t0;
    double hit_rate = (double)hits / (double)t.n_chunks;

    /* --- verify the restored model byte-for-byte (GPU recheck) ------ */
    uint64_t mism = 0;
    for (uint32_t c = 0; c < t.n_chunks; c++)
        mism += gpu_payload_check(&eng, dst + (size_t)c * chunk, chunk,
                                  t.num_ckpts, c);
    int ok = (mism == 0);

    /* --- metrics (measured) vs analytical model (predicted) --------- */
    double pred_save = model_serial_save_time(&t);
    double pred_hit = 0;
    double pred_resume = model_serial_resume_time(&t, t.num_ckpts, &pred_hit);

    printf("[P8 metrics]\n");
    printf("  save:      %.3f ms (%.3f GB/s)   [model %.3f ms]\n",
           save_ms, save_bw, pred_save * 1e3);
    printf("  per-ckpt:  %.3f ms (mean chunk %.1f us)\n",
           save_ms / (double)t.num_ckpts, mean_chunk_us);
    printf("  resume:    %.3f ms               [model %.3f ms]\n",
           resume_ms, pred_resume * 1e3);
    printf("  hit-rate:  %.2f                  [model %.2f]\n",
           hit_rate, pred_hit);
    printf("  p95 chunk: %.1f us (guest clock ~1ms quantized; cycle-accurate "
           "P95 in gem5 stats chunkLatency)\n", p95 * 1e3);
    printf("  gpu recheck: %s (mismatches=%" PRIu64 ")\n",
           ok ? "OK" : "MISMATCH", mism);

    struct simckpt_counters cnt;
    simckpt_get_counters(ctx, &cnt);
    printf("counters: completed=%" PRIu64 " errors=%" PRIu64 " intr=%" PRIu64
           "\n", cnt.completed, cnt.errors, cnt.intr_posted);

    ckpt_pool_free(dram, size);
    ckpt_pool_free(cxl, size);
    ckpt_pool_free(dst, size);
    free(lat);
    simckpt_close(ctx);

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
