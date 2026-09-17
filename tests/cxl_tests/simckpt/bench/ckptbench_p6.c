/* SPDX-License-Identifier: MIT */
/*
 * ckptbench_p6 - P6: application-layer LLM training save/fault/recover.
 *
 * Models the training loop as a workload (LLMServingSim style): the "model"
 * (weights) is a deterministic function of the training step, so its state can
 * be recomputed for verification. Two modes:
 *
 *   train   : run training steps, save checkpoints periodically, then inject a
 *             fault (clean crash, or crash mid-checkpoint) and persist the
 *             manifest before exiting.
 *   recover : read the persisted manifest, restore the last COMMITTED
 *             generation from durable storage, verify it byte-for-byte, and
 *             report the time-to-resume.
 *
 * The generation rollback (P6-2) is exercised by ckptd (manifest) and the
 * concurrent restore (P6-3) is intentionally left serial here, to be done at
 * P10 with multiple DMA engines.
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

#define CHUNK_LEN    4096
#define MODEL_CHUNKS 256
#define MODEL_SIZE   ((size_t)MODEL_CHUNKS * CHUNK_LEN)

#define CKPT_INTERVAL 10      /* save every 10 training steps        */
#define MANIFEST_PATH "/home/test_code/ckpt_manifest.bin"

/* Persistent manifest (survives the crash -> restart). */
struct persistent_manifest {
    uint64_t committed_gen;
    uint64_t committed_step;
};

static void
model_at(uint8_t *buf, uint32_t step)
{
    for (size_t i = 0; i < MODEL_SIZE; i++) {
        uint32_t h = step * 2654435761u + (uint32_t)i * 40503u;
        h = h * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(h >> 24);
    }
}

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
save_checkpoint(struct simckpt_ctx *ctx, struct ckpt_manifest *m,
                const uint8_t *model, uint32_t step, uint64_t gen)
{
    uint64_t base = gen * MODEL_SIZE;
    ckpt_manifest_begin(m, gen, MODEL_CHUNKS);

    for (uint32_t c = 0; c < MODEL_CHUNKS; c++) {
        uint32_t crc = 0;
        if (simckpt_save(ctx, model + (size_t)c * CHUNK_LEN,
                         base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                         (uint32_t)gen, c, &crc) != 0)
            return 1;
        ckpt_manifest_add(m, c, base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                          crc, CKPT_SOURCE_DISK);
        ckpt_manifest_commit(m, c);
    }

    if (!ckpt_manifest_all_committed(m))
        return 1;
    ckpt_manifest_finish(m);
    printf("  [train] checkpoint gen=%" PRIu64 " (step %u) committed\n",
           gen, step);
    return 0;
}

static int
persist_manifest(const struct ckpt_manifest *m, uint64_t committed_step)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    if (!f)
        return 1;
    struct persistent_manifest pm = {
        .committed_gen = m->committed_gen,
        .committed_step = committed_step,
    };
    fwrite(&pm, sizeof(pm), 1, f);
    fclose(f);
    return 0;
}

static int
load_manifest(struct persistent_manifest *pm)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f)
        return 1;
    int ok = fread(pm, sizeof(*pm), 1, f) == 1;
    fclose(f);
    return ok ? 0 : 1;
}

/* --- train mode -------------------------------------------------------- */
static int
mode_train(struct simckpt_ctx *ctx, const char *fault_mode)
{
    uint8_t *model = malloc(MODEL_SIZE);
    struct ckpt_manifest m;
    ckpt_manifest_init(&m);

    const uint32_t total_steps = 40;  /* 4 checkpoints: gens 1..4 */
    uint64_t committed_step = 0;

    for (uint32_t step = 1; step <= total_steps; step++) {
        model_at(model, step);  /* advance the model */

        if (step % CKPT_INTERVAL != 0)
            continue;
        uint64_t gen = step / CKPT_INTERVAL;

        if (!strcmp(fault_mode, "mid") && gen == 3) {
            /* crash mid-checkpoint: only 2 of MODEL_CHUNKS written, no finish */
            uint64_t base = gen * MODEL_SIZE;
            ckpt_manifest_begin(&m, gen, MODEL_CHUNKS);
            for (uint32_t c = 0; c < 2; c++) {
                uint32_t crc = 0;
                simckpt_save(ctx, model + (size_t)c * CHUNK_LEN,
                             base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                             (uint32_t)gen, c, &crc);
                ckpt_manifest_add(&m, c, base + (uint64_t)c * CHUNK_LEN,
                                  CHUNK_LEN, crc, CKPT_SOURCE_DISK);
                ckpt_manifest_commit(&m, c);
            }
            printf("  [train] FAULT: crashed mid-checkpoint gen=%" PRIu64
                   " after 2/%d chunks\n", gen, MODEL_CHUNKS);
            persist_manifest(&m, committed_step);
            printf("  [train] manifest persisted: committed_gen=%" PRIu64
                   " (step %" PRIu64 ")\n", m.committed_gen, committed_step);
            return 0;
        }

        if (!strcmp(fault_mode, "after") && gen == 3) {
            /* clean crash right after gen 2, before saving gen 3 */
            printf("  [train] FAULT: crashed after checkpoint gen=2\n");
            persist_manifest(&m, committed_step);
            printf("  [train] manifest persisted: committed_gen=%" PRIu64
                   " (step %" PRIu64 ")\n", m.committed_gen, committed_step);
            return 0;
        }

        if (save_checkpoint(ctx, &m, model, step, gen) != 0) {
            printf("  [train] FAIL: save gen %" PRIu64 "\n", gen);
            return 1;
        }
        committed_step = step;
    }

    printf("  [train] completed all checkpoints without fault\n");
    persist_manifest(&m, committed_step);
    return 0;
}

/* --- recover mode ------------------------------------------------------ */
static int
mode_recover(struct simckpt_ctx *ctx)
{
    struct persistent_manifest pm;
    if (load_manifest(&pm) != 0) {
        printf("  [recover] FAIL: no manifest\n");
        return 1;
    }
    printf("  [recover] manifest: committed_gen=%" PRIu64 " (step %" PRIu64
           ")\n", pm.committed_gen, pm.committed_step);

    uint8_t *model = ckpt_pool_alloc(1, MODEL_SIZE);  /* restore to CXL node  */
    uint8_t *expect = malloc(MODEL_SIZE);              /* reference (CPU only) */
    model_at(expect, (uint32_t)pm.committed_step);

    uint64_t base = pm.committed_gen * MODEL_SIZE;
    double t0 = now_ms();
    for (uint32_t c = 0; c < MODEL_CHUNKS; c++) {
        simckpt_restore(ctx, model + (size_t)c * CHUNK_LEN,
                        base + (uint64_t)c * CHUNK_LEN, CHUNK_LEN,
                        (uint32_t)pm.committed_gen, c, NULL);
    }
    double ms = now_ms() - t0;

    int ok = (memcmp(model, expect, MODEL_SIZE) == 0);
    printf("  [recover] restored gen=%" PRIu64 " in %.3f ms -> %s\n",
           pm.committed_gen, ms, ok ? "VERIFIED OK" : "MISMATCH");
    return ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);  /* unbuffered: see progress live */

    const char *mode = (argc > 1) ? argv[1] : "train";
    const char *fault = (argc > 2) ? argv[2] : "mid";
    const char *bar0 = (argc > 3) ? argv[3] : NULL;

    struct simckpt_ctx *ctx = simckpt_open(bar0);
    if (!ctx) {
        fprintf(stderr, "FAIL: cannot open SimCkptDevice\n");
        return 1;
    }
    printf("SimCkptDevice opened (mode=%s)\n", mode);

    int rc = !strcmp(mode, "recover") ? mode_recover(ctx)
                                      : mode_train(ctx, fault);
    simckpt_close(ctx);
    return rc;
}
