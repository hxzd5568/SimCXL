/* SPDX-License-Identifier: MIT */
/*
 * ckptbench - P3 functional test: VM driver + user-space stack.
 *
 * Exercises libckpt + ckptd (manifest / pinned pool) end-to-end:
 *   1. checkpoint save (DRAM -> storage, then CXL -> storage);
 *   2. checkpoint restore (storage -> DRAM, then storage -> CXL) with CRC +
 *      byte-for-byte verification;
 *   3. fault injection: crash mid-checkpoint, verify the manifest rolls back
 *      to the previous COMMITTED generation.
 *
 * Run without a kernel module: libckpt falls back to /dev/mem + pagemap so the
 * exact same ABI (simckpt_uapi.h) and library path are exercised.
 *
 * Each checkpoint generation is placed at a distinct storage base
 * (gen * checkpoint_size) so multiple generations coexist in the storage
 * namespace, and restore is keyed by generation.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libckpt.h"
#include "ckptd.h"

static void
fill_pattern(uint8_t *buf, size_t len, uint32_t ckpt, uint32_t chunk)
{
    uint32_t state = ckpt * 2654435761U + chunk * 40503U;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
}

static uint64_t
gen_base(uint64_t gen, uint32_t num_chunks, uint32_t chunk_len)
{
    return gen * (uint64_t)num_chunks * chunk_len;
}

static int
save_checkpoint(struct simckpt_ctx *ctx, struct ckpt_manifest *m,
                const uint8_t *src, uint32_t num_chunks, uint32_t chunk_len,
                uint64_t gen)
{
    uint64_t base = gen_base(gen, num_chunks, chunk_len);

    ckpt_manifest_begin(m, gen, num_chunks);

    for (uint32_t c = 0; c < num_chunks; c++) {
        const uint8_t *s = src + (size_t)c * chunk_len;
        uint32_t crc = 0;
        if (simckpt_save(ctx, s, base + (uint64_t)c * chunk_len, chunk_len,
                         (uint32_t)gen, c, &crc) != 0) {
            printf("FAIL: save chunk %u (gen %" PRIu64 ")\n", c, gen);
            return 1;
        }
        ckpt_manifest_add(m, c, base + (uint64_t)c * chunk_len, chunk_len,
                          crc, CKPT_SOURCE_DISK);
        ckpt_manifest_commit(m, c);
    }

    if (!ckpt_manifest_all_committed(m)) {
        printf("FAIL: not all chunks committed (gen %" PRIu64 ")\n", gen);
        return 1;
    }
    ckpt_manifest_finish(m);
    return 0;
}

static int
restore_checkpoint(struct simckpt_ctx *ctx, uint8_t *dst, const uint8_t *ref,
                   uint32_t num_chunks, uint32_t chunk_len, uint64_t gen)
{
    int pass = 1;
    uint64_t base = gen_base(gen, num_chunks, chunk_len);

    for (uint32_t c = 0; c < num_chunks; c++) {
        uint8_t *d = dst + (size_t)c * chunk_len;
        uint32_t crc = 0;
        if (simckpt_restore(ctx, d, base + (uint64_t)c * chunk_len, chunk_len,
                            (uint32_t)gen, c, &crc) != 0) {
            printf("FAIL: restore chunk %u (gen %" PRIu64 ")\n", c, gen);
            pass = 0;
            continue;
        }
        if (crc != simckpt_crc32(ref + (size_t)c * chunk_len, chunk_len)) {
            printf("FAIL: chunk %u crc=%#x\n", c, crc);
            pass = 0;
        }
        if (memcmp(d, ref + (size_t)c * chunk_len, chunk_len) != 0) {
            printf("FAIL: chunk %u byte mismatch (gen %" PRIu64 ")\n", c, gen);
            pass = 0;
        }
    }
    return pass ? 0 : 1;
}

static int
test_roundtrip(struct simckpt_ctx *ctx)
{
    const uint32_t num_chunks = 4;
    const uint32_t chunk_len = 4096;
    size_t total = (size_t)num_chunks * chunk_len;

    uint8_t *dram_src = ckpt_pool_alloc(0, total);
    uint8_t *dram_dst = ckpt_pool_alloc(0, total);
    uint8_t *cxl_src  = ckpt_pool_alloc(1, total);
    uint8_t *cxl_dst  = ckpt_pool_alloc(1, total);
    struct ckpt_manifest m;

    ckpt_manifest_init(&m);

    for (uint32_t c = 0; c < num_chunks; c++) {
        fill_pattern(dram_src + (size_t)c * chunk_len, chunk_len, 1, c);
        fill_pattern(cxl_src + (size_t)c * chunk_len, chunk_len, 2, c);
    }
    memset(dram_dst, 0xAA, total);
    memset(cxl_dst, 0xAA, total);

    /* gen 1: DRAM -> storage -> DRAM */
    if (save_checkpoint(ctx, &m, dram_src, num_chunks, chunk_len, 1)) {
        printf("[roundtrip] FAIL: save DRAM\n");
        return 1;
    }
    if (restore_checkpoint(ctx, dram_dst, dram_src, num_chunks, chunk_len, 1)) {
        printf("[roundtrip] FAIL: restore DRAM\n");
        return 1;
    }
    printf("[roundtrip] DRAM round-trip OK (gen 1)\n");

    /* gen 2: CXL -> storage -> CXL */
    if (save_checkpoint(ctx, &m, cxl_src, num_chunks, chunk_len, 2)) {
        printf("[roundtrip] FAIL: save CXL\n");
        return 1;
    }
    if (restore_checkpoint(ctx, cxl_dst, cxl_src, num_chunks, chunk_len, 2)) {
        printf("[roundtrip] FAIL: restore CXL\n");
        return 1;
    }
    printf("[roundtrip] CXL round-trip OK (gen 2, committed_gen=%" PRIu64 ")\n",
           m.committed_gen);

    struct simckpt_counters cnt;
    simckpt_get_counters(ctx, &cnt);
    printf("[roundtrip] counters: completed=%" PRIu64 " errors=%" PRIu64
           " intr=%" PRIu64 "\n",
           cnt.completed, cnt.errors, cnt.intr_posted);

    ckpt_pool_free(dram_src, total);
    ckpt_pool_free(dram_dst, total);
    ckpt_pool_free(cxl_src, total);
    ckpt_pool_free(cxl_dst, total);
    printf("[roundtrip] PASS\n");
    return 0;
}

static int
test_fault_injection(struct simckpt_ctx *ctx)
{
    const uint32_t num_chunks = 4;
    const uint32_t chunk_len = 4096;
    size_t total = (size_t)num_chunks * chunk_len;

    uint8_t *src = ckpt_pool_alloc(0, total);
    struct ckpt_manifest m;

    ckpt_manifest_init(&m);
    for (uint32_t c = 0; c < num_chunks; c++)
        fill_pattern(src + (size_t)c * chunk_len, chunk_len, 7, c);

    /* gen 1: complete checkpoint, becomes COMMITTED. */
    if (save_checkpoint(ctx, &m, src, num_chunks, chunk_len, 7)) {
        printf("[fault] FAIL: save gen 1\n");
        return 1;
    }
    uint64_t committed_before = m.committed_gen;
    printf("[fault] gen %" PRIu64 " committed\n", committed_before);

    /* gen 2 (id 8): crash after committing only the first 2 chunks. */
    uint64_t base = gen_base(8, num_chunks, chunk_len);
    ckpt_manifest_begin(&m, 8, num_chunks);
    for (uint32_t c = 0; c < 2; c++) {
        uint32_t crc = 0;
        simckpt_save(ctx, src + (size_t)c * chunk_len,
                     base + (uint64_t)c * chunk_len, chunk_len, 8, c, &crc);
        ckpt_manifest_add(&m, c, base + (uint64_t)c * chunk_len, chunk_len,
                          crc, CKPT_SOURCE_DISK);
        ckpt_manifest_commit(&m, c);
    }
    /* chunks 2,3 never written: the process "crashed". */
    printf("[fault] crash after 2/%u chunks of gen 8\n", num_chunks);

    if (ckpt_manifest_all_committed(&m)) {
        printf("[fault] FAIL: incomplete gen marked committed\n");
        return 1;
    }
    ckpt_manifest_rollback(&m);

    if (m.committed_gen != committed_before) {
        printf("[fault] FAIL: rollback to gen %" PRIu64 ", expected %" PRIu64
               "\n", m.committed_gen, committed_before);
        return 1;
    }
    printf("[fault] rolled back to gen %" PRIu64
           " (incomplete gen discarded)\n", m.committed_gen);

    ckpt_pool_free(src, total);
    printf("[fault] PASS\n");
    return 0;
}

int
main(int argc, char **argv)
{
    const char *bar0 = (argc > 1 && argv[1][0]) ? argv[1] : NULL;
    struct simckpt_ctx *ctx = simckpt_open(bar0);
    int pass = 0;

    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened (backend=%s)\n",
           bar0 ? "dev/mem" : "auto");

    pass |= test_roundtrip(ctx);
    pass |= test_fault_injection(ctx);

    simckpt_close(ctx);
    printf("%s\n", pass ? "FAIL" : "PASS");
    return pass ? 1 : 0;
}
