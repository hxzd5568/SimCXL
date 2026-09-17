/* SPDX-License-Identifier: MIT */
#include "trace_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void
trace_generate(struct ckpt_trace *t, uint64_t size_bytes,
               uint64_t chunk_size, uint64_t interval,
               uint32_t num_ckpts, double hot_frac)
{
    memset(t, 0, sizeof(*t));
    t->chunk_size = chunk_size;
    t->hot_frac = hot_frac;
    t->num_ckpts = num_ckpts;
    t->n_chunks = (uint32_t)(size_bytes / chunk_size);
    t->hot_cap = (uint32_t)llround(hot_frac * (double)t->n_chunks);

    for (uint32_t i = 0; i < num_ckpts && i < TRACE_MAX_CKPTS; i++) {
        t->events[i].step = (uint64_t)(i + 1) * interval;
        t->events[i].ckpt_id = i + 1;
        t->events[i].size_bytes = size_bytes;
    }
}

double
model_gpu_ingress(int topology)
{
    switch (topology) {
    case TOPO_DRAM: return B_GPU_DRAM;
    case TOPO_CXL:  return B_GPU_CXL;
    default:        return B_GPU_DRAM + B_GPU_CXL;
    }
}

double
model_mem_egress(int topology)
{
    switch (topology) {
    case TOPO_DRAM: return B_DRAM_READ;
    case TOPO_CXL:  return B_CXL_READ;
    default:        return B_DRAM_READ + B_CXL_READ;
    }
}

double
model_bsave_bound(int topology, int n_channels)
{
    double a = model_gpu_ingress(topology);
    double b = model_mem_egress(topology);
    double c = (double)n_channels * B_CHANNEL;
    double m = a < b ? a : b;
    return m < c ? m : c;
}

double
model_bresume_bound(int topology, int n_channels)
{
    double landing = (topology == TOPO_DRAM) ? B_DRAM_WRITE
                   : (topology == TOPO_CXL) ? B_CXL_WRITE
                   : B_DRAM_WRITE + B_CXL_WRITE;
    double gpu_read = model_mem_egress(topology);
    double storage = (double)n_channels * B_CHANNEL;
    double m = storage < landing ? storage : landing;
    return m < gpu_read ? m : gpu_read;
}

double
model_serial_save_time(const struct ckpt_trace *t)
{
    return (double)t->n_chunks * t->num_ckpts * T_SAVE_CHUNK + T_STARTUP;
}

/* LRU hot-standby over *global* chunk ids (ckpt_id-1)*n_chunks + chunk_id,
 * saved in checkpoint order. Returns a heap bitmap over global ids. */
static uint8_t *
lru_hot_set(const struct ckpt_trace *t)
{
    uint64_t total = (uint64_t)t->n_chunks * t->num_ckpts;
    uint8_t *hot = calloc((size_t)total, 1);
    uint32_t *order = malloc((size_t)total * sizeof(uint32_t));
    uint32_t n = 0;

    if (!hot || !order) {
        free(hot);
        free(order);
        return NULL;
    }

    for (uint32_t i = 0; i < t->num_ckpts; i++) {
        uint32_t base = i * t->n_chunks;
        for (uint32_t c = 0; c < t->n_chunks; c++) {
            uint32_t g = base + c;
            uint32_t j;
            for (j = 0; j < n; j++)
                if (order[j] == g)
                    break;
            if (j == n) {           /* new: insert at front, grow            */
                memmove(&order[1], &order[0], (size_t)n * sizeof(uint32_t));
                order[0] = g;
                n++;
            } else {                /* existing: move to front               */
                memmove(&order[1], &order[0], (size_t)j * sizeof(uint32_t));
                order[0] = g;
            }
            if (n > t->hot_cap)
                n = t->hot_cap;
        }
    }

    for (uint32_t i = 0; i < n; i++)
        hot[order[i]] = 1;

    free(order);
    return hot;
}

double
model_serial_resume_time(const struct ckpt_trace *t, uint32_t restore_ckpt,
                         double *hit_rate_out)
{
    uint8_t *hot = lru_hot_set(t);
    uint32_t hits = 0;

    if (hot) {
        uint32_t base = (restore_ckpt - 1) * t->n_chunks;
        for (uint32_t c = 0; c < t->n_chunks; c++)
            hits += hot[base + c];
        free(hot);
    }
    uint32_t cold = t->n_chunks - hits;

    if (hit_rate_out)
        *hit_rate_out = (double)hits / (double)t->n_chunks;

    return (double)hits * T_RESTORE_HOT + (double)cold * T_RESTORE_COLD
         + T_STARTUP;
}

int
trace_chunk_is_hot(const struct ckpt_trace *t, uint32_t ckpt_id,
                   uint32_t chunk_id)
{
    uint64_t total = (uint64_t)t->n_chunks * t->num_ckpts;
    uint64_t g = (uint64_t)(ckpt_id - 1) * t->n_chunks + chunk_id;
    return g + t->hot_cap >= total;   /* g in [total-hot_cap, total) */
}
