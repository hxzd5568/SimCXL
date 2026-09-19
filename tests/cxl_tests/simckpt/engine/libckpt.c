/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "libckpt.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "virt2phy.h"

#define BAR_SIZE (64 * 1024)

struct simckpt_qstate {
    struct simckpt_desc *sq;   /* persistent submission queue             */
    struct simckpt_cpl *cq;    /* persistent completion queue             */
    uint64_t sq_phys;
    uint64_t cq_phys;
    uint32_t tail;             /* absolute SQ tail (next write index)     */
    uint32_t head;             /* absolute CQ head (next read index)      */
    int programmed;            /* SQ/CQ base/depth programmed?            */
};

struct simckpt_ctx {
    volatile uint8_t *bar;     /* BAR0 mapping                             */
    int devfd;                 /* /dev/simckpt0 or /dev/mem                */
    int use_driver;            /* 1 = simckpt.ko, 0 = /dev/mem fallback    */
    int n_queues;              /* number of device queues                  */
    struct simckpt_qstate q[SIMCKPT_MAX_QUEUES];
};

static void
bar_wq(struct simckpt_ctx *ctx, uint64_t off, uint64_t v)
{
    *(volatile uint64_t *)(ctx->bar + off) = v;
}

static uint64_t
bar_rq(struct simckpt_ctx *ctx, uint64_t off)
{
    return *(volatile uint64_t *)(ctx->bar + off);
}

static uint32_t crc_table[256];
static int crc_table_init;

uint32_t
simckpt_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!crc_table_init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            crc_table[i] = c;
        }
        crc_table_init = 1;
    }
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFU;
}

static uint64_t
find_bar0_sysfs(void)
{
    FILE *f = fopen("/sys/bus/pci/devices/0000:00:07.0/resource", "r");
    uint64_t start = 0, end = 0;
    if (!f)
        return 0;
    if (fscanf(f, "0x%lx 0x%lx", &start, &end) != 2)
        start = 0;
    fclose(f);
    return start;
}

static int
alloc_rings(struct simckpt_ctx *ctx, int q)
{
    struct simckpt_qstate *qs = &ctx->q[q];
    if (qs->sq)
        return 0;

    qs->sq = mmap(NULL, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    qs->cq = mmap(NULL, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (qs->sq == MAP_FAILED || qs->cq == MAP_FAILED)
        return -1;
    memset(qs->sq, 0, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE);
    memset(qs->cq, 0, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE);
    qs->sq_phys = simckpt_phys(ctx, qs->sq);
    qs->cq_phys = simckpt_phys(ctx, qs->cq);
    if (qs->sq_phys == (uint64_t)-1 || qs->cq_phys == (uint64_t)-1)
        return -1;
    return 0;
}

static void
free_rings(struct simckpt_ctx *ctx, int q)
{
    struct simckpt_qstate *qs = &ctx->q[q];
    if (qs->sq && qs->sq != MAP_FAILED)
        munmap(qs->sq, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE);
    if (qs->cq && qs->cq != MAP_FAILED)
        munmap(qs->cq, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE);
    qs->sq = NULL;
    qs->cq = NULL;
}

static void
select_queue(struct simckpt_ctx *ctx, int q)
{
    bar_wq(ctx, SIMCKPT_REG_QUEUE_SEL, (uint64_t)q);
}

struct simckpt_ctx *
simckpt_open(const char *bar0_override)
{
    struct simckpt_ctx *ctx = calloc(1, sizeof(*ctx));
    uint64_t bar0 = 0;
    int fd = -1;
    int use_driver = 0;

    if (!ctx)
        return NULL;

    fd = open("/dev/simckpt0", O_RDWR);
    if (fd >= 0) {
        use_driver = 1;
    } else {
        if (bar0_override && bar0_override[0])
            bar0 = strtoull(bar0_override, NULL, 0);
        if (!bar0)
            bar0 = find_bar0_sysfs();
        if (!bar0) {
            free(ctx);
            return NULL;
        }
        fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) {
            free(ctx);
            return NULL;
        }
    }

    if (use_driver) {
        ctx->bar = (volatile uint8_t *)mmap(NULL, BAR_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
    } else {
        ctx->bar = (volatile uint8_t *)mmap(NULL, BAR_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, bar0);
    }
    if (ctx->bar == MAP_FAILED) {
        close(fd);
        free(ctx);
        return NULL;
    }

    ctx->devfd = fd;
    ctx->use_driver = use_driver;

    /* Allocate queue 0's rings eagerly (legacy single-queue API). */
    if (alloc_rings(ctx, 0) != 0) {
        simckpt_close(ctx);
        return NULL;
    }

    ctx->n_queues = (int)bar_rq(ctx, SIMCKPT_REG_NUM_QUEUES);
    if (ctx->n_queues < 1)
        ctx->n_queues = 1;
    if (ctx->n_queues > SIMCKPT_MAX_QUEUES)
        ctx->n_queues = SIMCKPT_MAX_QUEUES;
    return ctx;
}

void
simckpt_close(struct simckpt_ctx *ctx)
{
    if (!ctx)
        return;
    for (int q = 0; q < SIMCKPT_MAX_QUEUES; q++)
        free_rings(ctx, q);
    if (ctx->bar && ctx->bar != MAP_FAILED)
        munmap((void *)ctx->bar, BAR_SIZE);
    if (ctx->devfd >= 0)
        close(ctx->devfd);
    free(ctx);
}

uint64_t
simckpt_phys(struct simckpt_ctx *ctx, void *buf)
{
    if (ctx->use_driver) {
        struct simckpt_register_arg a = { .buf = (uint64_t)buf, .len = 4096 };
        if (ioctl(ctx->devfd, SIMCKPT_IOC_REGISTER, &a) == 0)
            return a.dma_addr;
        return (uint64_t)-1;
    }
    return virt2phy2(buf);
}

int
simckpt_num_queues(struct simckpt_ctx *ctx)
{
    return ctx->n_queues;
}

/* ---- legacy single-queue API (queue 0, per-batch reset semantics) ---- */

int
simckpt_submit(struct simckpt_ctx *ctx,
               const struct simckpt_desc *desc, uint32_t n,
               int enable_intr)
{
    uint64_t sq_phys, cq_phys;

    if (!ctx || !desc || n == 0 || n > SIMCKPT_RING_DEPTH)
        return -EINVAL;

    struct simckpt_qstate *qs = &ctx->q[0];
    sq_phys = qs->sq_phys;
    cq_phys = qs->cq_phys;
    if (sq_phys == (uint64_t)-1 || cq_phys == (uint64_t)-1)
        return -EIO;

    memcpy(qs->sq, desc, (size_t)n * SIMCKPT_DESC_SIZE);

    /* Reset the device (rings + counters), then program and ring. After the
     * reset, COMPLETED starts at 0 and the CQ is written from entry 0. */
    select_queue(ctx, 0);
    bar_wq(ctx, SIMCKPT_REG_CTRL, 0x2);
    bar_wq(ctx, SIMCKPT_REG_SQ_BASE, sq_phys);
    bar_wq(ctx, SIMCKPT_REG_SQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_CQ_BASE, cq_phys);
    bar_wq(ctx, SIMCKPT_REG_CQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_INTR_EN, enable_intr ? 1 : 0);
    bar_wq(ctx, SIMCKPT_REG_SQ_DOORBELL, n);
    qs->tail = n;

    return 0;
}

int
simckpt_wait(struct simckpt_ctx *ctx, uint64_t n)
{
    select_queue(ctx, 0);
    for (int spins = 0; spins < 100000000; spins++) {
        if (bar_rq(ctx, SIMCKPT_REG_COMPLETED) >= n)
            return 0;
    }
    return -ETIMEDOUT;
}

int
simckpt_completions(struct simckpt_ctx *ctx,
                    struct simckpt_cpl *out, uint32_t n)
{
    struct simckpt_qstate *qs = &ctx->q[0];
    for (uint32_t i = 0; i < n; i++)
        out[i] = qs->cq[i % SIMCKPT_RING_DEPTH];
    return 0;
}

int
simckpt_get_counters(struct simckpt_ctx *ctx, struct simckpt_counters *c)
{
    select_queue(ctx, 0);
    c->completed   = bar_rq(ctx, SIMCKPT_REG_COMPLETED);
    c->intr_posted = bar_rq(ctx, SIMCKPT_REG_INTR_POSTED);
    c->errors      = bar_rq(ctx, SIMCKPT_REG_ERRORS);
    c->sq_head     = bar_rq(ctx, SIMCKPT_REG_SQ_HEAD);
    c->cq_tail     = bar_rq(ctx, SIMCKPT_REG_CQ_TAIL);
    return 0;
}

/* ---- multi-queue batch API (P10) ------------------------------------ */

int
simckpt_queue_init(struct simckpt_ctx *ctx, int q, int enable_intr)
{
    if (!ctx || q < 0 || q >= ctx->n_queues)
        return -EINVAL;
    if (alloc_rings(ctx, q) != 0)
        return -EIO;

    struct simckpt_qstate *qs = &ctx->q[q];
    select_queue(ctx, q);
    bar_wq(ctx, SIMCKPT_REG_SQ_BASE, qs->sq_phys);
    bar_wq(ctx, SIMCKPT_REG_SQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_CQ_BASE, qs->cq_phys);
    bar_wq(ctx, SIMCKPT_REG_CQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_INTR_EN, enable_intr ? 1 : 0);
    qs->programmed = 1;
    qs->tail = 0;
    qs->head = 0;
    return 0;
}

int
simckpt_submit_batch(struct simckpt_ctx *ctx, int q,
                     const struct simckpt_desc *desc, uint32_t n)
{
    if (!ctx || !desc || n == 0 || q < 0 || q >= ctx->n_queues)
        return -EINVAL;
    if (n > SIMCKPT_RING_DEPTH)
        return -EINVAL;

    struct simckpt_qstate *qs = &ctx->q[q];
    if (!qs->programmed)
        return -EIO;

    /* Copy descriptors into the ring, handling wraparound. */
    uint32_t start = qs->tail;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (start + i) % SIMCKPT_RING_DEPTH;
        memcpy(&qs->sq[slot], &desc[i], SIMCKPT_DESC_SIZE);
    }
    qs->tail = start + n;

    select_queue(ctx, q);
    bar_wq(ctx, SIMCKPT_REG_SQ_DOORBELL, qs->tail);
    return 0;
}

int
simckpt_wait_q(struct simckpt_ctx *ctx, int q, uint64_t target)
{
    if (!ctx || q < 0 || q >= ctx->n_queues)
        return -EINVAL;
    select_queue(ctx, q);
    for (int spins = 0; spins < 100000000; spins++) {
        if (bar_rq(ctx, SIMCKPT_REG_COMPLETED) >= target)
            return 0;
        sched_yield();
    }
    return -ETIMEDOUT;
}

int
simckpt_completions_q(struct simckpt_ctx *ctx, int q,
                      uint64_t start, struct simckpt_cpl *out, uint32_t n)
{
    if (!ctx || q < 0 || q >= ctx->n_queues)
        return -EINVAL;
    struct simckpt_qstate *qs = &ctx->q[q];
    for (uint32_t i = 0; i < n; i++)
        out[i] = qs->cq[(start + i) % SIMCKPT_RING_DEPTH];
    return 0;
}

int
simckpt_get_qcounters(struct simckpt_ctx *ctx, int q,
                      struct simckpt_qcounters *c)
{
    if (!ctx || !c || q < 0 || q >= ctx->n_queues)
        return -EINVAL;
    select_queue(ctx, q);
    c->outstanding     = bar_rq(ctx, SIMCKPT_REG_Q_OUTSTANDING);
    c->retry           = bar_rq(ctx, SIMCKPT_REG_Q_RETRY);
    c->queue_full      = bar_rq(ctx, SIMCKPT_REG_Q_QUEUE_FULL);
    c->completed_bytes = bar_rq(ctx, SIMCKPT_REG_Q_COMPLETED_BYTES);
    c->latency_avg     = bar_rq(ctx, SIMCKPT_REG_Q_LATENCY_AVG);
    c->latency_p95     = bar_rq(ctx, SIMCKPT_REG_Q_LATENCY_P95);
    c->issue_tick      = bar_rq(ctx, SIMCKPT_REG_Q_ISSUE_TICK);
    c->done_tick       = bar_rq(ctx, SIMCKPT_REG_Q_DONE_TICK);
    return 0;
}

int
simckpt_save(struct simckpt_ctx *ctx, const void *src,
             uint64_t storage_offset, uint32_t len,
             uint32_t ckpt_id, uint32_t chunk_id, uint32_t *crc_out)
{
    struct simckpt_desc d;
    struct simckpt_cpl c;
    uint32_t crc = simckpt_crc32(src, len);
    int ret;

    memset(&d, 0, sizeof(d));
    d.src_addr = simckpt_phys(ctx, (void *)src);
    d.storage_offset = storage_offset;
    d.length = len;
    d.checkpoint_id = ckpt_id;
    d.chunk_id = chunk_id;
    d.flags = SIMCKPT_FLAG_SAVE;
    d.crc32 = crc;

    ret = simckpt_submit(ctx, &d, 1, 0);
    if (ret)
        return ret;
    ret = simckpt_wait(ctx, 1);
    if (ret)
        return ret;
    simckpt_completions(ctx, &c, 1);
    if (c.status != 0 || c.crc32 != crc)
        return -EIO;

    if (crc_out)
        *crc_out = crc;
    return 0;
}

int
simckpt_restore(struct simckpt_ctx *ctx, void *dst,
                uint64_t storage_offset, uint32_t len,
                uint32_t ckpt_id, uint32_t chunk_id, uint32_t *crc_out)
{
    struct simckpt_desc d;
    struct simckpt_cpl c;
    uint32_t crc;
    int ret;

    memset(&d, 0, sizeof(d));
    d.dst_addr = simckpt_phys(ctx, dst);
    d.storage_offset = storage_offset;
    d.length = len;
    d.checkpoint_id = ckpt_id;
    d.chunk_id = chunk_id;
    d.flags = SIMCKPT_FLAG_RESTORE;

    ret = simckpt_submit(ctx, &d, 1, 0);
    if (ret)
        return ret;
    ret = simckpt_wait(ctx, 1);
    if (ret)
        return ret;
    simckpt_completions(ctx, &c, 1);

    crc = simckpt_crc32(dst, len);
    if (crc_out)
        *crc_out = crc;
    return 0;
}
