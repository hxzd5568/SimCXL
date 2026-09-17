/* SPDX-License-Identifier: MIT */
/*
 * trace_model.c - host-side analytical replay of the checkpoint trace.
 *
 * Complements analytical_model.py: replays the *same* parameterized trace at
 * full scale (512 MiB, 131072 x 4 KiB chunks) in milliseconds on the host --
 * no gem5 needed -- and prints the analytical model predictions. This is the
 * "analysis model at scale" half of P8; the cycle simulator samples a few
 * points (ckptbench_p8) and is compared against the latency-limited numbers
 * printed here.
 */
#include <inttypes.h>
#include <stdio.h>

#include "trace_gen.h"

static const char *
topo_name(int t)
{
    switch (t) {
    case TOPO_DRAM: return "dram";
    case TOPO_CXL:  return "cxl";
    default:        return "both";
    }
}

static void
print_bound_table(void)
{
    printf("Steady-state save bound  Bsave <= min(ingress, egress, storage)\n");
    printf("%8s | %10s | %10s | %10s | %10s\n",
           "topology", "1ch", "2ch", "4ch", "8ch");
    for (int t = 0; t <= TOPO_BOTH; t++)
        printf("%8s | %9.2f | %9.2f | %9.2f | %9.2f GB/s\n",
               topo_name(t),
               model_bsave_bound(t, 1) / 1e9,
               model_bsave_bound(t, 2) / 1e9,
               model_bsave_bound(t, 4) / 1e9,
               model_bsave_bound(t, 8) / 1e9);
}

int
main(void)
{
    /* Full-scale trace: 512 MiB = 131072 x 4 KiB chunks, 4 checkpoints. */
    const uint64_t size = 512ull << 20;
    const uint64_t chunk = 4096;
    struct ckpt_trace t;

    trace_generate(&t, size, chunk, 10 /* steps/ckpt */, 4, 0.5);
    printf("trace: %u ckpts x %" PRIu64 " MiB, %u chunks each, "
           "hot_cap=%u chunks\n\n",
           t.num_ckpts, size >> 20, t.n_chunks, t.hot_cap);

    print_bound_table();
    printf("\n");

    double save_s = model_serial_save_time(&t);
    uint64_t total_bytes = (uint64_t)t.n_chunks * t.num_ckpts * chunk;
    printf("serial (latency-limited, single engine):\n");
    printf("  save_time      = %.3f ms\n", save_s * 1e3);
    printf("  save_bw        = %.3f GB/s\n",
           (double)total_bytes / save_s / 1e9);

    double latest_hit = 0, oldest_hit = 0;
    double resume_latest = model_serial_resume_time(&t, t.num_ckpts,
                                                    &latest_hit);
    double resume_oldest = model_serial_resume_time(&t, 1, &oldest_hit);
    printf("  resume latest  = %.3f ms (hit-rate %.2f)\n",
           resume_latest * 1e3, latest_hit);
    printf("  resume oldest  = %.3f ms (hit-rate %.2f)\n",
           resume_oldest * 1e3, oldest_hit);

    /* Scale-independence checks. */
    int ok = 1;
    if (latest_hit < 0.49 || latest_hit > 0.51) {
        printf("  FAIL: latest hit-rate should be ~hot_frac (0.50)\n");
        ok = 0;
    }
    if (oldest_hit >= latest_hit) {
        printf("  FAIL: oldest checkpoint should be evicted from CXL "
               "(lower hit-rate)\n");
        ok = 0;
    }
    if (resume_oldest <= resume_latest) {
        printf("  FAIL: cold restore should be slower than hot\n");
        ok = 0;
    }

    printf("\n%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
