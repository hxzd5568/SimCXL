#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "virt2phy.h"

/* Raw mbind syscall (avoid libnuma dependency). */
#ifndef SYS_mbind
#define SYS_mbind 237 /* x86_64 */
#endif
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif
#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT (1 << 0)
#endif

static long
do_mbind(void *addr, unsigned long len, int mode, unsigned long *nodemask,
         unsigned long maxnode, unsigned flags)
{
    return syscall(SYS_mbind, addr, len, mode, nodemask, maxnode, flags);
}

static double
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Copy `size` bytes from a buffer bound to `src_node` into a buffer bound to
 * `dst_node`, `iters` times, and print the achieved bandwidth.
 *
 * usage: memcpy_test <size-bytes> <src-node> <dst-node> [iters]
 */
int
main(int argc, char **argv)
{
    size_t size = (argc > 1) ? strtoull(argv[1], NULL, 0) : (64UL << 20);
    int src_node = (argc > 2) ? atoi(argv[2]) : 0;
    int dst_node = (argc > 3) ? atoi(argv[3]) : 1;
    int iters = (argc > 4) ? atoi(argv[4]) : 1;

    void *src = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *dst = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED || dst == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Touch both buffers first so pages are faulted in. */
    memset(src, 0x5a, size);
    memset(dst, 0x00, size);

    unsigned long src_mask = 1UL << src_node;
    unsigned long dst_mask = 1UL << dst_node;
    if (do_mbind(src, size, MPOL_BIND, &src_mask, 64,
                 MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
        perror("mbind(src)");
    }
    if (do_mbind(dst, size, MPOL_BIND, &dst_mask, 64,
                 MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
        perror("mbind(dst)");
    }

    printf("src phys = 0x%lx (node %d), dst phys = 0x%lx (node %d)\n",
           (unsigned long)virt2phy2(src), src_node,
           (unsigned long)virt2phy2(dst), dst_node);

    memcpy(dst, src, size); /* warm-up */

    double t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        memcpy(dst, src, size);
    }
    double t1 = now_ns();

    double secs = (t1 - t0) / 1e9;
    double gbps = (double)size * iters / secs / 1e9;
    printf("memcpy %zu bytes x%d = %.3f GB/s (%.2f ns/byte)\n", size, iters,
           gbps, secs * 1e9 / ((double)size * iters));
    return 0;
}
