/* SPDX-License-Identifier: MIT */
#include "topology.h"

#include <string.h>

/* Calibrated baselines (see exec_summary.md P0/P2). */
#define B_GPU_DRAM   30e9
#define B_GPU_CXL    30e9
#define B_DRAM_READ  29e9
#define B_CXL_READ   20e9
#define B_DRAM_WRITE 29e9
#define B_CXL_WRITE  30e9
#define B_CHANNEL    7.2e9
#define CHAN_LATENCY 10e-6

void
topology_init(struct ckpt_topology *t, uint64_t chunk_size, int num_channels)
{
    memset(t, 0, sizeof(*t));
    t->chunk_size = chunk_size;
    t->num_channels = num_channels;
    for (int i = 0; i < num_channels; i++) {
        t->channels[i].id = i;
        t->channels[i].read_bw = B_CHANNEL;
        t->channels[i].write_bw = B_CHANNEL;
        t->channels[i].latency = CHAN_LATENCY;
    }
    t->gpu_bw_dram = B_GPU_DRAM;
    t->gpu_bw_cxl = B_GPU_CXL;
    t->dram_read_bw = B_DRAM_READ;
    t->cxl_read_bw = B_CXL_READ;
    t->dram_write_bw = B_DRAM_WRITE;
    t->cxl_write_bw = B_CXL_WRITE;
}

int
topology_stripe_channel(const struct ckpt_topology *t, uint32_t chunk_id)
{
    return (int)(chunk_id % (uint32_t)t->num_channels);
}

uint64_t
topology_channel_offset(const struct ckpt_topology *t, uint32_t chunk_id,
                        uint64_t offset_in_chunk)
{
    return (uint64_t)(chunk_id / (uint32_t)t->num_channels) * t->chunk_size
         + offset_in_chunk;
}

double
topology_storage_bw(const struct ckpt_topology *t, int write)
{
    double sum = 0;
    for (int i = 0; i < t->num_channels; i++)
        sum += write ? t->channels[i].write_bw : t->channels[i].read_bw;
    return sum;
}

double
topology_bsave_bound(const struct ckpt_topology *t, int node)
{
    double gpu_in = (node == TOPO_DRAM) ? t->gpu_bw_dram
                  : (node == TOPO_CXL) ? t->gpu_bw_cxl
                  : t->gpu_bw_dram + t->gpu_bw_cxl;
    double mem_out = (node == TOPO_DRAM) ? t->dram_read_bw
                   : (node == TOPO_CXL) ? t->cxl_read_bw
                   : t->dram_read_bw + t->cxl_read_bw;
    double storage = topology_storage_bw(t, /*write=*/1);

    double m = gpu_in < mem_out ? gpu_in : mem_out;
    return m < storage ? m : storage;
}

double
topology_bresume_bound(const struct ckpt_topology *t, int node)
{
    double landing = (node == TOPO_DRAM) ? t->dram_write_bw
                   : (node == TOPO_CXL) ? t->cxl_write_bw
                   : t->dram_write_bw + t->cxl_write_bw;
    double gpu_read = (node == TOPO_DRAM) ? t->dram_read_bw
                    : (node == TOPO_CXL) ? t->cxl_read_bw
                    : t->dram_read_bw + t->cxl_read_bw;
    double storage = topology_storage_bw(t, /*write=*/0);

    double m = storage < landing ? storage : landing;
    return m < gpu_read ? m : gpu_read;
}

double
topology_path_rate(const struct ckpt_topology *t, int node, int channel,
                   int write_to_storage)
{
    double node_bw;
    if (write_to_storage)
        node_bw = (node == TOPO_DRAM) ? t->dram_read_bw : t->cxl_read_bw;
    else
        node_bw = (node == TOPO_DRAM) ? t->dram_write_bw : t->cxl_write_bw;

    double chan_bw = write_to_storage ? t->channels[channel].write_bw
                                      : t->channels[channel].read_bw;
    return node_bw < chan_bw ? node_bw : chan_bw;
}

int
topology_candidate_paths(const struct ckpt_topology *t, uint32_t chunk_id,
                         int write_to_storage, struct topo_path out[2])
{
    int channel = topology_stripe_channel(t, chunk_id);
    int n = 0;
    for (int node = TOPO_DRAM; node <= TOPO_CXL; node++) {
        out[n].node = node;
        out[n].channel = channel;
        out[n].rate = topology_path_rate(t, node, channel, write_to_storage);
        n++;
    }
    return n;
}

double
topology_path_cost(const struct ckpt_topology *t, const struct topo_path *p,
                   uint32_t bytes)
{
    double rate = p->rate > 0 ? p->rate : 1.0;
    double queued = (double)t->channels[p->channel].queued_bytes;
    return (queued + (double)bytes) / rate + t->channels[p->channel].latency;
}
