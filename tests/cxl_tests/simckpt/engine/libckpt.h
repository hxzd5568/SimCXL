/* SPDX-License-Identifier: MIT */
/*
 * libckpt - user-space library for the SimCkptDevice checkpoint engine.
 *
 * Drives the device through its BAR0 (doorbell / counters / control
 * registers) and manages persistent submission/completion ring pairs. Buffer
 * physical addresses are resolved through the simckpt.ko driver (ioctl) or,
 * when the module is not loaded, through /proc/self/pagemap (user-space
 * fallback used for the gem5 tests in this tree).
 *
 * Since P10 the device exposes several independent queues (one per GPU DMA
 * lane). The legacy single-queue API (simckpt_submit/wait/save/restore) keeps
 * operating on queue 0 with its per-batch reset semantics; the new batch API
 * (simckpt_queue_init / simckpt_submit_batch / simckpt_wait_q) programs each
 * queue once and appends descriptors at an absolute tail, removing the
 * per-chunk reset + per-chunk busy-wait that dominated the guest-side
 * checkpoint path (see target.md P8/P10).
 */
#ifndef LIBCKPT_H
#define LIBCKPT_H

#include <stddef.h>
#include <stdint.h>

#include "simckpt_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIMCKPT_RING_DEPTH 16384
#define SIMCKPT_MAX_QUEUES 8

struct simckpt_ctx;

/* Per-queue (per-link) statistics exposed through read-only BAR registers. */
struct simckpt_qcounters {
    uint64_t outstanding;        /* currently busy slots                       */
    uint64_t retry;              /* storage/DMA send retries                   */
    uint64_t queue_full;         /* times the queue ran out of free slots      */
    uint64_t completed_bytes;    /* bytes persisted to destination             */
    uint64_t latency_avg;        /* per-chunk latency, avg (ticks)             */
    uint64_t latency_p95;        /* per-chunk latency, P95 (ticks)             */
    uint64_t issue_tick;         /* first descriptor issue tick (per queue)    */
    uint64_t done_tick;          /* last completion tick (per queue)           */
};

/* Open the device, map BAR0, and allocate the persistent SQ/CQ. */
struct simckpt_ctx *simckpt_open(const char *bar0_override);

void simckpt_close(struct simckpt_ctx *ctx);

/* Resolve the DMA (physical) address of a buffer. */
uint64_t simckpt_phys(struct simckpt_ctx *ctx, void *buf);

/* Number of independent queues exposed by the device. */
int simckpt_num_queues(struct simckpt_ctx *ctx);

/* ---- legacy single-queue API (queue 0, per-batch reset semantics) ---- */
/* Submit `n` descriptors (copied into the persistent SQ) and ring the
 * doorbell. Returns 0. */
int simckpt_submit(struct simckpt_ctx *ctx,
                   const struct simckpt_desc *desc, uint32_t n,
                   int enable_intr);

/* Block until at least `n` total completions have been posted. */
int simckpt_wait(struct simckpt_ctx *ctx, uint64_t n);

/* Read the completions of the most recent batch (matched by chunk_id). */
int simckpt_completions(struct simckpt_ctx *ctx,
                        struct simckpt_cpl *out, uint32_t n);

/* Read the device counters. */
int simckpt_get_counters(struct simckpt_ctx *ctx,
                         struct simckpt_counters *c);

/* ---- multi-queue batch API (P10) ------------------------------------ */
/* Allocate + program queue `q`'s SQ/CQ once (no reset). `enable_intr`
 * turns on interrupt posting for that queue. Returns 0 on success. */
int simckpt_queue_init(struct simckpt_ctx *ctx, int q, int enable_intr);

/* Append `n` descriptors to queue `q` and ring its doorbell (one doorbell
 * for the whole batch). The descriptors are copied into the queue's SQ at its
 * absolute tail. Returns the absolute completion count the batch starts at
 * (callers wait for `start + n`). */
int simckpt_submit_batch(struct simckpt_ctx *ctx, int q,
                         const struct simckpt_desc *desc, uint32_t n);

/* Block until queue `q` posts at least `target` absolute completions. */
int simckpt_wait_q(struct simckpt_ctx *ctx, int q, uint64_t target);

/* Read queue `q`'s completion ring from index `start` (absolute), writing
 * `n` entries into `out`. */
int simckpt_completions_q(struct simckpt_ctx *ctx, int q,
                          uint64_t start, struct simckpt_cpl *out, uint32_t n);

/* Read queue `q`'s per-link statistics. */
int simckpt_get_qcounters(struct simckpt_ctx *ctx, int q,
                          struct simckpt_qcounters *c);

/* CRC32 (IEEE 802.3, matches the device). */
uint32_t simckpt_crc32(const void *data, size_t len);

/* High-level: save a chunk (memory -> storage). Returns the payload CRC. */
int simckpt_save(struct simckpt_ctx *ctx, const void *src,
                 uint64_t storage_offset, uint32_t len,
                 uint32_t ckpt_id, uint32_t chunk_id, uint32_t *crc_out);

/* High-level: restore a chunk (storage -> memory). Returns the payload CRC. */
int simckpt_restore(struct simckpt_ctx *ctx, void *dst,
                    uint64_t storage_offset, uint32_t len,
                    uint32_t ckpt_id, uint32_t chunk_id, uint32_t *crc_out);

#ifdef __cplusplus
}
#endif

#endif /* LIBCKPT_H */
