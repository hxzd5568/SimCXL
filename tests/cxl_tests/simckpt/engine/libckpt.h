/* SPDX-License-Identifier: MIT */
/*
 * libckpt - user-space library for the SimCkptDevice checkpoint engine.
 *
 * Drives the device through its BAR0 (doorbell / counters / control
 * registers) and manages a persistent submission/completion ring pair. Buffer
 * physical addresses are resolved through the simckpt.ko driver (ioctl) or,
 * when the module is not loaded, through /proc/self/pagemap (user-space
 * fallback used for the gem5 tests in this tree).
 */
#ifndef LIBCKPT_H
#define LIBCKPT_H

#include <stddef.h>
#include <stdint.h>

#include "simckpt_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIMCKPT_RING_DEPTH 256

struct simckpt_ctx;

/* Open the device, map BAR0, and allocate the persistent SQ/CQ. */
struct simckpt_ctx *simckpt_open(const char *bar0_override);

void simckpt_close(struct simckpt_ctx *ctx);

/* Resolve the DMA (physical) address of a buffer. */
uint64_t simckpt_phys(struct simckpt_ctx *ctx, void *buf);

/* Submit `n` descriptors (copied into the persistent SQ) and ring the
 * doorbell. Returns the absolute completed-count the batch starts at. */
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
