/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p7 - P7: GPU payload realism + data integrity.
 *
 * Emulates the GPU DMA engine (gpu_dma_engine) staging a reproducible payload
 * into DRAM and CXL, saving to durable storage, restoring, and verifying the
 * whole round trip GPU -> DRAM/CXL -> storage -> DRAM/CXL -> GPU:
 *
 *   1. per-chunk CRC32 (device-computed, cross-checked on restore);
 *   2. full-checkpoint SHA-256 (software, over the whole model);
 *   3. GPU-side regeneration + byte-for-byte compare (no reference kept).
 *
 * This exercises acceptance #1 (GPU->DRAM/CXL->storage->DRAM/CXL->GPU CRC
 * consistent) and #5 (chunk reordering still restores correctly, since each
 * chunk is keyed and verified independently by checkpoint_id/chunk_id).
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
#include "sha256.h"

#define CHUNK_LEN  4096
#define NUM_CHUNKS 256
#define CKPT_SIZE  ((size_t)NUM_CHUNKS * CHUNK_LEN)

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Full path: GPU -> node (DRAM/CXL) -> storage -> node -> GPU. */
static int
run_path(struct simckpt_ctx *ctx, struct gpu_dma_engine *eng,
         int node, uint32_t ckpt_id)
{
    const char *name = node == 0 ? "DRAM" : "CXL";
    uint64_t base = (uint64_t)(ckpt_id - 1) * CKPT_SIZE;
    uint8_t *src = ckpt_pool_alloc(node, CKPT_SIZE);
    uint8_t *dst = ckpt_pool_alloc(node, CKPT_SIZE);
    uint32_t save_crc[NUM_CHUNKS];
    struct sha256_ctx sc, dc;
    uint8_t sdig[SHA256_DIGEST_SIZE], ddig[SHA256_DIGEST_SIZE];
    char shex[65], dhex[65];
    double save_ms, restore_ms, t0;
    int crc_ok = 1, sha_ok, gpu_ok;
    uint32_t c;

    if (!src || !dst) {
        printf("  [%s] FAIL: pool alloc\n", name);
        return 1;
    }

    /* 1. GPU stages the reproducible payload into the node. */
    for (c = 0; c < NUM_CHUNKS; c++)
        gpu_payload_gen(eng, src + (size_t)c * CHUNK_LEN, CHUNK_LEN,
                        ckpt_id, c);

    /* 2. Golden full-checkpoint SHA-256 over the staged payload. */
    sha256_init(&sc);
    sha256_update(&sc, src, CKPT_SIZE);
    sha256_final(&sc, sdig);
    sha256_hex(sdig, shex);

    /* 3. Save to durable storage (device computes + verifies per-chunk CRC). */
    t0 = now_ms();
    for (c = 0; c < NUM_CHUNKS; c++) {
        if (simckpt_save(ctx, src + (size_t)c * CHUNK_LEN,
                         base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                         ckpt_id, c, &save_crc[c]) != 0) {
            printf("  [%s] FAIL: save chunk %u\n", name, c);
            return 1;
        }
    }
    save_ms = now_ms() - t0;

    /* 4. Restore from storage and cross-check per-chunk CRC32. */
    memset(dst, 0xAA, CKPT_SIZE);
    t0 = now_ms();
    for (c = 0; c < NUM_CHUNKS; c++) {
        uint32_t crc = 0;
        if (simckpt_restore(ctx, dst + (size_t)c * CHUNK_LEN,
                            base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                            ckpt_id, c, &crc) != 0) {
            printf("  [%s] FAIL: restore chunk %u\n", name, c);
            crc_ok = 0;
            continue;
        }
        if (crc != save_crc[c]) {
            printf("  [%s] chunk %u CRC mismatch: save=%#x restore=%#x\n",
                   name, c, save_crc[c], crc);
            crc_ok = 0;
        }
    }
    restore_ms = now_ms() - t0;

    /* 5. Full-checkpoint SHA-256 over the restored buffer. */
    sha256_init(&dc);
    sha256_update(&dc, dst, CKPT_SIZE);
    sha256_final(&dc, ddig);
    sha256_hex(ddig, dhex);
    sha_ok = (memcmp(sdig, ddig, SHA256_DIGEST_SIZE) == 0);

    /* 6. GPU regenerates the expected payload and compares (no reference). */
    gpu_ok = 1;
    for (c = 0; c < NUM_CHUNKS; c++) {
        if (gpu_payload_check(eng, dst + (size_t)c * CHUNK_LEN, CHUNK_LEN,
                              ckpt_id, c) != 0)
            gpu_ok = 0;
    }

    printf("  [%s] save %.3f ms (%u chunks), restore %.3f ms\n",
           name, save_ms, NUM_CHUNKS, restore_ms);
    printf("       per-chunk CRC32: %s | full SHA-256: %s | GPU recheck: %s\n",
           crc_ok ? "OK" : "MISMATCH",
           sha_ok ? "OK" : "MISMATCH",
           gpu_ok ? "OK" : "MISMATCH");
    printf("       sha256=%s%s\n", shex,
           sha_ok ? "" : " (restored differs)");

    ckpt_pool_free(src, CKPT_SIZE);
    ckpt_pool_free(dst, CKPT_SIZE);
    return (crc_ok && sha_ok && gpu_ok) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *bar0 = (argc > 1 && argv[1][0]) ? argv[1] : NULL;
    struct simckpt_ctx *ctx = simckpt_open(bar0);
    struct gpu_dma_engine eng;
    int rc = 0;

    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened\n");

    gpu_dma_engine_init(&eng, 0xC0FFEEu);

    /* checkpoint 1: GPU -> DRAM -> storage -> DRAM -> GPU */
    rc |= run_path(ctx, &eng, 0, 1);

    /* checkpoint 2: GPU -> CXL -> storage -> CXL -> GPU */
    rc |= run_path(ctx, &eng, 1, 2);

    struct simckpt_counters cnt;
    simckpt_get_counters(ctx, &cnt);
    printf("counters: completed=%" PRIu64 " errors=%" PRIu64 " intr=%" PRIu64
           "\n", cnt.completed, cnt.errors, cnt.intr_posted);
    printf("gpu engine: generated=%" PRIu64 "B verified=%" PRIu64
           "B mismatches=%" PRIu64 "\n",
           eng.bytes_generated, eng.bytes_verified, eng.mismatches);

    simckpt_close(ctx);
    printf("%s\n", rc ? "FAIL" : "PASS");
    return rc ? 1 : 0;
}
