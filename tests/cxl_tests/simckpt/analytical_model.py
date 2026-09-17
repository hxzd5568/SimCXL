#!/usr/bin/env python3
"""P8 analytical model: predict checkpoint save/restore performance.

This is the LLMServingSim-style *analysis model* that runs ahead of (and in
tandem with) the SimCXL cycle simulator. It encodes the P0/P2/P5 calibrated
baselines and predicts two things:

  1. The steady-state (bandwidth-limited) save bound, from target.md:

         Bsave <= min(B_gpu->dram + B_gpu->cxl,
                       B_dram->store + B_cxl->store,
                       sum(B_storage_channel))

     This is the bound that the *parallel/batched* engine (P10 multi-queue)
     approaches once the storage and memory paths are fully pipelined.

  2. The latency-limited serial prediction for the current single-engine
     device (one chunk at a time, as driven by libckpt's synchronous API).
     This is the regime the small sampled cycle-sim runs actually live in, and
     is calibrated from the P5/P7 measured per-chunk latencies.

The cycle simulator (x86-cxl-ckptd-p8.py + ckptbench_p8) samples a few key
points and is compared against regime (2); regime (1) is the analytic headroom
the design is moving toward.

Run standalone:

    python3 analytical_model.py                 # bound sweep + trace prediction
    python3 analytical_model.py --sweep-only    # just the Bsave bound table
    python3 analytical_model.py --trace 4 1 3 0.5   # one parameterized trace
"""

import argparse

# --------------------------------------------------------------------------
# Calibrated baselines (sources in exec_summary.md / readme_cxl.md):
#   GPU->DRAM write ~30 GB/s, DRAM->GPU read ~29 GB/s (P0, Ruby+DDR5)
#   GPU->CXL  write ~30 GB/s, CXL ->GPU read ~20 GB/s (P0)
#   storage: 1 channel ~7.2 GB/s, 2 channels ~11.5 GB/s (P2)
#   serial per-chunk (single-engine device, P5/P7 guest-simulated time):
#     save chunk       ~ 98 us   (P7: 256 chunks / 24.996 ms)
#     restore cold     ~102 us   (P5: 512 chunks / 52.992 ms; P7: 25.996 ms)
#     restore hot      ~ 14 us   (P5: 100% hot 512 chunks / 6.999 ms)
#   ~1 ms DMA pipeline cold start + tail drain (readme_cxl.md)
# --------------------------------------------------------------------------
B_GPU_DRAM   = 30e9
B_GPU_CXL    = 30e9
B_DRAM_READ  = 29e9
B_CXL_READ   = 20e9
B_DRAM_WRITE = 29e9
B_CXL_WRITE  = 30e9
B_CHANNEL    = 7.2e9

T_SAVE_CHUNK        = 98e-6
T_RESTORE_COLD      = 102e-6
T_RESTORE_HOT       = 14e-6
T_STARTUP           = 1e-3

# topology identifiers (match trace_gen.h)
TOPOLOGY = {"dram": 0, "cxl": 1, "both": 2}
TOPOLOGY_NAME = {0: "dram", 1: "cxl", 2: "both"}


def gpu_ingress(topology):
    """GPU staging ingress bandwidth (B/s)."""
    return {
        0: B_GPU_DRAM,
        1: B_GPU_CXL,
        2: B_GPU_DRAM + B_GPU_CXL,
    }[topology]


def mem_egress(topology):
    """Memory read egress toward storage (B/s)."""
    return {
        0: B_DRAM_READ,
        1: B_CXL_READ,
        2: B_DRAM_READ + B_CXL_READ,
    }[topology]


def bsave_bound(topology, n_channels):
    """Steady-state save bandwidth bound (B/s), target.md section 5."""
    return min(gpu_ingress(topology), mem_egress(topology),
               n_channels * B_CHANNEL)


def bresume_bound(topology, n_channels):
    """Steady-state restore (cold) bandwidth bound (B/s)."""
    landing = {0: B_DRAM_WRITE, 1: B_CXL_WRITE,
               2: B_DRAM_WRITE + B_CXL_WRITE}[topology]
    gpu_read = {0: B_DRAM_READ, 1: B_CXL_READ,
                2: B_DRAM_READ + B_CXL_READ}[topology]
    return min(n_channels * B_CHANNEL, landing, gpu_read)


def gen_trace(size_bytes, chunk_size, interval, num_ckpts, hot_frac):
    """Generate a deterministic checkpoint trace (mirrors trace_gen.c).

    Each event is (step, ckpt_id, size_bytes). The hot-cold distribution is
    modelled as: the most-recently-used `hot_frac` fraction of a checkpoint's
    chunks stays hot in the CXL standby (LLMServingSim KV-cache LRU idea).
    """
    n_chunks = size_bytes // chunk_size
    hot_cap = int(round(hot_frac * n_chunks))
    events = [{"step": (i + 1) * interval, "ckpt_id": i + 1,
               "size_bytes": size_bytes}
              for i in range(num_ckpts)]
    return {
        "chunk_size": chunk_size,
        "n_chunks": n_chunks,
        "hot_frac": hot_frac,
        "hot_cap": hot_cap,
        "num_ckpts": num_ckpts,
        "events": events,
    }


def _lru_hot_set(trace):
    """Global LRU hot set over the whole trace. Chunks are identified by their
    *global* index (ckpt_id-1)*n_chunks + chunk_id, so a checkpoint saved long
    ago is naturally evicted from the CXL standby (KV-cache LRU idea)."""
    cap = trace["hot_cap"]
    order = []  # most recent global chunk ids first
    for ev in trace["events"]:
        base = (ev["ckpt_id"] - 1) * trace["n_chunks"]
        for c in range(trace["n_chunks"]):
            g = base + c
            order = [g] + [x for x in order if x != g]
            if len(order) > cap:
                order = order[:cap]
    return set(order)


def predict_serial(trace, topology, n_channels, restore_ckpt=None):
    """Latency-limited serial prediction for the single-engine device."""
    n_chunks = trace["n_chunks"]
    num_ckpts = trace["num_ckpts"]
    chunk = trace["chunk_size"]
    total_bytes = n_chunks * num_ckpts * chunk

    save_time = n_chunks * num_ckpts * T_SAVE_CHUNK + T_STARTUP
    save_bw = total_bytes / save_time

    hot = _lru_hot_set(trace)
    if restore_ckpt is None:
        restore_ckpt = num_ckpts
    base = (restore_ckpt - 1) * n_chunks
    hits = sum(1 for g in range(base, base + n_chunks) if g in hot)
    cold = n_chunks - hits
    resume_time = hits * T_RESTORE_HOT + cold * T_RESTORE_COLD + T_STARTUP
    hit_rate = hits / n_chunks

    return {
        "save_time": save_time,
        "save_bw": save_bw,
        "resume_time": resume_time,
        "hit_rate": hit_rate,
        "p95_chunk": T_SAVE_CHUNK,  # serial, single outstanding -> ~const
    }


def predict_bound(trace, topology, n_channels, restore_ckpt=None):
    """Steady-state (bandwidth-limited) prediction for a parallel engine."""
    total_bytes = trace["n_chunks"] * trace["num_ckpts"] * trace["chunk_size"]
    save_bw = bsave_bound(topology, n_channels)
    save_time = total_bytes / save_bw + T_STARTUP

    hot_bytes = trace["hot_cap"] * trace["chunk_size"]
    cold_bytes = (trace["n_chunks"] - trace["hot_cap"]) * trace["chunk_size"]
    hot_bw = B_CXL_READ
    cold_bw = bresume_bound(topology, n_channels)
    resume_time = hot_bytes / hot_bw + cold_bytes / cold_bw + T_STARTUP

    return {
        "save_time": save_time,
        "save_bw": save_bw,
        "resume_time": resume_time,
        "hit_rate": trace["hot_cap"] / trace["n_chunks"],
        "p95_chunk": None,
    }


def fmt_gbs(bw):
    return f"{bw / 1e9:6.2f} GB/s"


def sweep():
    print("Steady-state save bandwidth bound  Bsave <= min(ingress, egress, storage)")
    print("=" * 78)
    header = f"{'topology':>8} | " + " | ".join(
        f"{n:>2}ch" for n in (1, 2, 4, 8))
    print(header)
    print("-" * 78)
    for topo in (0, 1, 2):
        row = f"{TOPOLOGY_NAME[topo]:>8} |"
        for n in (1, 2, 4, 8):
            row += " " + fmt_gbs(bsave_bound(topo, n)) + " |"
        print(row)
    print()
    print("Restore (cold) bound  Brestore <= min(storage, landing, gpu-read)")
    print("=" * 78)
    print(header)
    print("-" * 78)
    for topo in (0, 1, 2):
        row = f"{TOPOLOGY_NAME[topo]:>8} |"
        for n in (1, 2, 4, 8):
            row += " " + fmt_gbs(bresume_bound(topo, n)) + " |"
        print(row)


def predict_trace(size_mib, num_ckpts, hot_frac, chunk_size=4096,
                  interval=1, topology=2, n_channels=2):
    size_bytes = size_mib << 20
    trace = gen_trace(size_bytes, chunk_size, interval, num_ckpts, hot_frac)
    s = predict_serial(trace, topology, n_channels)
    b = predict_bound(trace, topology, n_channels)

    print(f"trace: {num_ckpts} ckpts x {size_mib} MiB, chunk {chunk_size} B, "
          f"hot_frac {hot_frac:.2f}, topology={TOPOLOGY_NAME[topology]}, "
          f"{n_channels} storage channels")
    print("  [serial / latency-limited]  save %.3f ms  save-bw %s  "
          "resume %.3f ms  hit-rate %.2f  p95 %.1f us"
          % (s["save_time"] * 1e3, fmt_gbs(s["save_bw"]),
             s["resume_time"] * 1e3, s["hit_rate"], s["p95_chunk"] * 1e6))
    print("  [bound / bandwidth-limited] save %.3f ms  save-bw %s  "
          "resume %.3f ms  hit-rate %.2f"
          % (b["save_time"] * 1e3, fmt_gbs(b["save_bw"]),
             b["resume_time"] * 1e3, b["hit_rate"]))
    return trace, s, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", nargs="+", type=float,
                    help="size_mib num_ckpts hot_frac")
    ap.add_argument("--channels", type=int, default=2)
    ap.add_argument("--topology", choices=("dram", "cxl", "both"),
                    default="both")
    args = ap.parse_args()

    sweep()

    if args.trace:
        size_mib, num_ckpts, hot_frac = args.trace
        predict_trace(int(size_mib), int(num_ckpts), hot_frac,
                      topology=TOPOLOGY[args.topology],
                      n_channels=args.channels)


if __name__ == "__main__":
    main()
