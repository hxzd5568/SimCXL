/* SPDX-License-Identifier: MIT */
#include "balancer.h"

#include <math.h>
#include <string.h>

void
balancer_init(struct balancer *b)
{
    memset(b, 0, sizeof(*b));
    b->alpha = 0.3;            /* EWMA: weight new samples moderately */
    b->occupancy_weight = 0.0; /* seconds of penalty per 0..1 occupancy */
    b->backlog_weight = 0.0;
    b->retry_weight = 0.0;
    b->p95_weight = 0.0;
}

void
balancer_config_lane(struct balancer *b, int lane, const char *name,
                     int numa_node, uint64_t capacity,
                     double ewma_bw, double token_rate, double token_burst)
{
    struct lane_stats *l = &b->lanes[lane];
    l->name = name;
    l->numa_node = numa_node;
    l->pool_capacity = capacity;
    l->ewma_bw = ewma_bw;
    l->token_rate = token_rate;
    l->token_burst = token_burst;
    l->tokens = token_burst;
}

double
balancer_predicted_finish(const struct balancer *b, int lane,
                          uint32_t chunk_size)
{
    const struct lane_stats *l = &b->lanes[lane];
    double r = l->ewma_bw > 0 ? l->ewma_bw : 1.0;
    return (double)(l->queued_bytes + chunk_size) / r;
}

int
balancer_select_lane(struct balancer *b, uint32_t chunk_size)
{
    int best = -1;
    double best_cost = INFINITY;

    for (int i = 0; i < SIMCKPT_NUM_LANES; i++) {
        struct lane_stats *l = &b->lanes[i];

        /* Pool headroom. */
        if (l->pool_used + chunk_size > l->pool_capacity)
            continue;
        /* Token bucket: cannot exceed the ingest rate. */
        if (l->tokens < (double)chunk_size)
            continue;

        double predicted = balancer_predicted_finish(b, i, chunk_size);
        double occupancy = (double)l->pool_used / (double)l->pool_capacity;
        double cost = predicted
                    + b->occupancy_weight * occupancy
                    + b->backlog_weight * l->queued_bytes
                    + b->retry_weight * (double)l->retries
                    + b->p95_weight * l->p95_latency;

        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }

    return best;
}

void
balancer_on_issue(struct balancer *b, int lane, uint32_t bytes)
{
    struct lane_stats *l = &b->lanes[lane];
    l->queued_bytes += bytes;
    l->outstanding++;
    l->tokens -= (double)bytes;
}

void
balancer_on_complete(struct balancer *b, int lane, uint32_t bytes,
                     double latency_s)
{
    struct lane_stats *l = &b->lanes[lane];

    if (l->queued_bytes >= bytes)
        l->queued_bytes -= bytes;
    else
        l->queued_bytes = 0;

    if (l->outstanding > 0)
        l->outstanding--;

    l->completed_chunks++;
    l->completed_bytes += bytes;

    /* EWMA bandwidth update. */
    if (latency_s > 0) {
        double bw = (double)bytes / latency_s;
        l->ewma_bw = b->alpha * bw + (1.0 - b->alpha) * l->ewma_bw;
    }
}

void
balancer_refill(struct balancer *b, double dt_s)
{
    for (int i = 0; i < SIMCKPT_NUM_LANES; i++) {
        struct lane_stats *l = &b->lanes[i];
        l->tokens += l->token_rate * dt_s;
        if (l->tokens > l->token_burst)
            l->tokens = l->token_burst;
    }
}
