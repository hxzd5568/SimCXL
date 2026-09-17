/* SPDX-License-Identifier: MIT */
/*
 * balancer - dynamic chunk-to-lane routing for the checkpoint engine.
 *
 * Implements the target.md section-5 dispatch policy. Each lane (DRAM / CXL)
 * tracks its queued bytes, outstanding count, pool headroom, EWMA bandwidth,
 * P95 latency and retry count, plus a token bucket that bounds the lane's
 * ingest rate. For every incoming chunk the balancer picks the lane with the
 * shortest predicted finish time (queued_bytes + chunk_size) / rate, subject
 * to pool headroom and token availability, plus occupancy / backlog / retry
 * penalties.
 */
#ifndef BALANCER_H
#define BALANCER_H

#include <stdint.h>

#define SIMCKPT_NUM_LANES 2
#define LANE_DRAM 0
#define LANE_CXL  1

struct lane_stats {
    const char *name;
    int numa_node;

    uint64_t pool_capacity;      /* max pinned bytes in this lane         */
    uint64_t pool_used;          /* currently pinned bytes                */
    uint64_t queued_bytes;       /* queued but not yet issued             */
    uint64_t outstanding;        /* in-flight chunks                      */
    uint64_t completed_chunks;   /* total chunks completed                */
    uint64_t completed_bytes;    /* total bytes completed                 */
    uint64_t retries;            /* backpressure / retry events           */

    double ewma_bw;              /* EWMA effective rate (B/s)             */
    double p95_latency;          /* P95 completion latency (s)            */

    double tokens;               /* token bucket: current tokens (bytes)  */
    double token_rate;           /* refill rate (B/s)                     */
    double token_burst;          /* bucket size (bytes)                   */
};

struct balancer {
    struct lane_stats lanes[SIMCKPT_NUM_LANES];

    double alpha;                /* EWMA smoothing factor (0..1]          */
    double occupancy_weight;     /* pool occupancy penalty                */
    double backlog_weight;       /* storage backlog penalty               */
    double retry_weight;         /* retry penalty                         */
    double p95_weight;           /* latency penalty                       */
};

void balancer_init(struct balancer *b);
void balancer_config_lane(struct balancer *b, int lane, const char *name,
                          int numa_node, uint64_t capacity,
                          double ewma_bw, double token_rate,
                          double token_burst);

/* Pick a lane for a chunk of `chunk_size` bytes. Returns the lane index, or
 * -1 if no lane has headroom + tokens (backpressure). */
int balancer_select_lane(struct balancer *b, uint32_t chunk_size);

void balancer_on_issue(struct balancer *b, int lane, uint32_t bytes);
void balancer_on_complete(struct balancer *b, int lane, uint32_t bytes,
                          double latency_s);
void balancer_refill(struct balancer *b, double dt_s);

/* Predicted finish time (s) for a lane, without penalties. */
double balancer_predicted_finish(const struct balancer *b, int lane,
                                 uint32_t chunk_size);

#endif /* BALANCER_H */
