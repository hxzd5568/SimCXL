#!/usr/bin/env python3
"""PCIe bandwidth test using cudaMemcpy (H2D / D2H) with pinned memory.

Usage:
    python pcie_bandwidth_test.py --size-mb 512 --iters 10
"""

import argparse
import sys


def main():
    parser = argparse.ArgumentParser(description="Measure PCIe H2D/D2H bandwidth")
    parser.add_argument("--size-mb", type=int, default=512,
                        help="transfer size in MiB (>=256 recommended)")
    parser.add_argument("--iters", type=int, default=10,
                        help="number of timed iterations per direction")
    args = parser.parse_args()

    nbytes = args.size_mb * 1024 * 1024

    try:
        import cupy as cp
        from cupy.cuda import runtime
    except ImportError:
        sys.exit("cupy not found. Run: pip install cupy-cuda11x (match your CUDA)")

    props = runtime.getDeviceProperties(0)
    print(f"Device: {props['name'].decode()}  (compute {props['major']}.{props['minor']})")

    stream = cp.cuda.Stream(non_blocking=True)
    start = cp.cuda.Event()
    end = cp.cuda.Event()

    h = cp.cuda.alloc_pinned_memory(nbytes)
    d = cp.cuda.alloc(nbytes)

    def copy(kind):
        if kind == runtime.memcpyHostToDevice:
            runtime.memcpyAsync(d.ptr, h.ptr, nbytes, kind, stream.ptr)
        else:
            runtime.memcpyAsync(h.ptr, d.ptr, nbytes, kind, stream.ptr)

    # warmup
    for _ in range(3):
        copy(runtime.memcpyHostToDevice)
        copy(runtime.memcpyDeviceToHost)
    stream.synchronize()

    def bench(kind, label):
        times = []
        for _ in range(args.iters):
            start.record(stream)
            copy(kind)
            end.record(stream)
            stream.synchronize()
            times.append(cp.cuda.get_elapsed_time(start, end))
        best = min(times)
        gbps = nbytes / (best / 1e3) / 1e9
        print(f"{label:>12}: best {best:8.3f} ms  ->  {gbps:7.2f} GB/s")
        return gbps

    print(f"Transfer size: {args.size_mb} MiB, iterations: {args.iters}\n")
    h2d = bench(runtime.memcpyHostToDevice, "Host->Device")
    d2h = bench(runtime.memcpyDeviceToHost, "Device->Host")

    print("\nNote: PCIe Gen4 x16 theoretical = 31.5 GB/s, real ~27 GB/s.")
    print("If you see ~half of that, host memory is likely pageable (not pinned).")


if __name__ == "__main__":
    main()
