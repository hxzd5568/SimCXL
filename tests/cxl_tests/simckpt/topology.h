/* SPDX-License-Identifier: MIT */
/*
 * topology - explicit checkpoint topology (borrows CXLMemSim's topology idea).
 *
 * Makes the data-mover topology an explicit, queryable object instead of a set
 * of implicit assumptions: {GPU, DRAM pool, CXL pool, N storage channels}. From
 * it the code derives:
 *
 *   - striping:  channel = chunk_id % N, channel_offset = (chunk_id/N)*chunk
 *                (must match ParallelStorage::mapAddr in the gem5 model);
 *   - candidate paths: for a chunk, {DRAM -> channel, CXL -> channel} on save
 *                and {channel -> DRAM, channel -> CXL} on restore, each with an
 *                effective rate = min(node bw, channel bw);
 *   - steady-state bounds: Bsave <= min(GPU ingress, memory egress, storage)
 *                and the matching restore bound;
 *   - a channel/occupancy-aware path cost (CXLMemSim scheduling idea) so the
 *                dispatcher can pick the least-congested path.
 */
#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include <stdint.h>

#define TOPO_MAX_CHANNELS 16

/* Memory nodes (matching the DRAM/CXL NUMA nodes of the board). */
#define TOPO_DRAM 0
#define TOPO_CXL  1
#define TOPO_NODES 2

struct topo_storage_channel {
    int id;
    double read_bw;        /* B/s                             */
    double write_bw;       /* B/s                             */
    double latency;        /* s                               */
    uint64_t queued_bytes; /* bytes queued but not yet served  */
    uint32_t outstanding;  /* in-flight requests              */
};

struct ckpt_topology {
    uint64_t chunk_size;
    int num_channels;
    struct topo_storage_channel channels[TOPO_MAX_CHANNELS];

    /* calibrated node bandwidths (P0/P2/P5 baselines) */
    double gpu_bw_dram;    /* GPU -> DRAM write              */
    double gpu_bw_cxl;     /* GPU -> CXL  write              */
    double dram_read_bw;   /* DRAM -> device read            */
    double cxl_read_bw;    /* CXL  -> device read            */
    double dram_write_bw;  /* device -> DRAM write           */
    double cxl_write_bw;   /* device -> CXL  write           */
};

/* A candidate data path between a memory node and a storage channel. */
struct topo_path {
    int node;        /* TOPO_DRAM / TOPO_CXL          */
    int channel;
    double rate;     /* effective B/s for this path   */
};

void topology_init(struct ckpt_topology *t, uint64_t chunk_size,
                   int num_channels);

/* --- striping (must mirror ParallelStorage::mapAddr) -------------------- */
int topology_stripe_channel(const struct ckpt_topology *t, uint32_t chunk_id);
uint64_t topology_channel_offset(const struct ckpt_topology *t,
                                 uint32_t chunk_id, uint64_t offset_in_chunk);

/* --- aggregate storage bandwidth ---------------------------------------- */
double topology_storage_bw(const struct ckpt_topology *t, int write);

/* --- steady-state bounds (GB/s-equivalent, in B/s) ---------------------- */
double topology_bsave_bound(const struct ckpt_topology *t, int node);
double topology_bresume_bound(const struct ckpt_topology *t, int node);

/* --- candidate paths ---------------------------------------------------- */
/* Effective rate of a (node, channel) path. `write_to_storage` selects the
 * channel's write vs read bandwidth. */
double topology_path_rate(const struct ckpt_topology *t, int node, int channel,
                          int write_to_storage);

/* Enumerate the candidate save/restore paths for a chunk (one per memory
 * node). Returns the number of paths written (<= 2). */
int topology_candidate_paths(const struct ckpt_topology *t, uint32_t chunk_id,
                             int write_to_storage, struct topo_path out[2]);

/* Channel/occupancy-aware path cost in seconds (predicted finish time). */
double topology_path_cost(const struct ckpt_topology *t,
                          const struct topo_path *p, uint32_t bytes);

#endif /* TOPOLOGY_H */
