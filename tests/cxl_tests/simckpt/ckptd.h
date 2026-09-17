/* SPDX-License-Identifier: MIT */
/*
 * ckptd - checkpoint daemon core (manifest + pinned-pool management).
 *
 * The daemon owns the DRAM (node 0) and CXL (node 1) pinned pools and the
 * checkpoint manifest: per-chunk metadata (CRC, location, state) plus the
 * generation that tracks which checkpoint version is COMMITTED. Only a fully
 * COMMITTED generation may be restored; an in-flight (WRITING) checkpoint is
 * discarded and rolled back to the previous generation.
 */
#ifndef CKPTD_H
#define CKPTD_H

#include <stddef.h>
#include <stdint.h>

#define CKPT_MAX_CHUNKS 4096
#define CKPT_SOURCE_DISK     0
#define CKPT_SOURCE_CXL_HOT  1
#define CKPT_STATE_WRITING   0
#define CKPT_STATE_COMMITTED 1

struct chunk_meta {
    uint64_t checkpoint_id;
    uint32_t chunk_id;
    uint64_t logical_offset;
    uint32_t length;
    uint32_t crc32;
    uint8_t source;         /* CKPT_SOURCE_*                          */
    uint8_t state;          /* CKPT_STATE_*                           */
} __attribute__((packed));

struct ckpt_manifest {
    uint64_t current_gen;           /* generation of the in-flight ckpt   */
    uint64_t committed_gen;         /* last fully-committed generation    */
    uint32_t num_chunks;            /* chunks recorded so far             */
    uint32_t expected_chunks;       /* total chunks this generation needs */
    struct chunk_meta chunks[CKPT_MAX_CHUNKS];
};

/* --- manifest ---------------------------------------------------------- */
void ckpt_manifest_init(struct ckpt_manifest *m);

/* Start a new checkpoint generation of `expected_chunks` chunks. */
uint64_t ckpt_manifest_begin(struct ckpt_manifest *m, uint64_t ckpt_id,
                             uint32_t expected_chunks);

/* Record a chunk as WRITING (payload saved, not yet durable/verified). */
int ckpt_manifest_add(struct ckpt_manifest *m, uint32_t chunk_id,
                      uint64_t logical_offset, uint32_t length,
                      uint32_t crc, uint8_t source);

/* Mark a chunk COMMITTED (durable + CRC verified). */
int ckpt_manifest_commit(struct ckpt_manifest *m, uint32_t chunk_id);

/* True if every chunk of the current generation is COMMITTED. */
int ckpt_manifest_all_committed(const struct ckpt_manifest *m);

/* Promote the current generation to committed (call once all_committed). */
void ckpt_manifest_finish(struct ckpt_manifest *m);

/* Roll back an in-flight checkpoint to the previous generation. */
void ckpt_manifest_rollback(struct ckpt_manifest *m);

/* --- pinned pool ------------------------------------------------------- */
/* Allocate `size` bytes bound to NUMA node `node` (0 = DRAM, 1 = CXL). */
void *ckpt_pool_alloc(int node, size_t size);

void ckpt_pool_free(void *buf, size_t size);

/* Pool with pressure-driven state machine (borrows CXLMemSim DCD idea):
 *   NORMAL  -> allow reuse and growth
 *   FROZEN  -> reuse only, no new pin
 *   SHRINK  -> release refcount-0 free buffers
 *   STOPPED -> stop new checkpoints, drain in-flight I/O
 */
enum ckpt_pool_state {
    POOL_NORMAL,
    POOL_FROZEN,
    POOL_SHRINK,
    POOL_STOPPED,
};

struct ckpt_pool {
    int numa_node;
    uint64_t capacity;      /* max pinned bytes                       */
    uint64_t pinned;        /* currently pinned bytes                  */
    enum ckpt_pool_state state;
};

void ckpt_pool_init(struct ckpt_pool *p, int node, uint64_t capacity);

/* Allocate `size` bytes from the pool. Returns NULL when the pool cannot
 * grow (FROZEN/SHRINK/STOPPED, or out of headroom). */
void *ckpt_pool_alloc_from(struct ckpt_pool *p, size_t size);

void ckpt_pool_free_from(struct ckpt_pool *p, void *buf, size_t size);

/* Recompute the pool state from a memory-pressure percentage (0..100; higher
 * = more pressure). Mirrors target.md thresholds: <80 NORMAL, 80..90 FROZEN,
 * 90..95 SHRINK, >95 STOPPED. */
void ckpt_pool_update(struct ckpt_pool *p, int pressure_pct);

/* --- CXL hot standby --------------------------------------------------- */
/* CXL keeps a hot subset of the checkpoint (most recently used / highest
 * recovery-cost chunks) as a *cache* of the durable storage copy (borrows
 * CXLMemSim coherency engine's sharer/dirty idea). A restore that hits CXL
 * avoids the slow storage read.
 */
struct ckpt_hot_standby {
    uint32_t capacity;               /* max hot chunks (LRU eviction)     */
    uint32_t num_hot;
    uint32_t order[CKPT_MAX_CHUNKS]; /* hot chunk ids, 0 = most recent    */
};

void ckpt_hot_init(struct ckpt_hot_standby *h, uint32_t capacity);
int ckpt_hot_is_hot(const struct ckpt_hot_standby *h, uint32_t chunk_id);
void ckpt_hot_add(struct ckpt_hot_standby *h, uint32_t chunk_id);

#endif /* CKPTD_H */
