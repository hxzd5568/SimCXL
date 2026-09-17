/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "libckpt.h"

#include <errno.h>
#include <fcntl.h>
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

struct simckpt_ctx {
    volatile uint8_t *bar;      /* BAR0 mapping                            */
    int devfd;                  /* /dev/simckpt0 or /dev/mem               */
    int use_driver;             /* 1 = simckpt.ko, 0 = /dev/mem fallback   */
    struct simckpt_desc *sq;    /* persistent submission queue             */
    struct simckpt_cpl *cq;     /* persistent completion queue            */
    uint32_t batch_n;           /* descriptors in the last batch           */
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

    /* Persistent rings (faulted in so they are physically present). */
    ctx->sq = mmap(NULL, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ctx->cq = mmap(NULL, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctx->sq == MAP_FAILED || ctx->cq == MAP_FAILED) {
        simckpt_close(ctx);
        return NULL;
    }
    memset(ctx->sq, 0, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE);
    memset(ctx->cq, 0, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE);

    ctx->devfd = fd;
    ctx->use_driver = use_driver;
    return ctx;
}

void
simckpt_close(struct simckpt_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->sq && ctx->sq != MAP_FAILED)
        munmap(ctx->sq, SIMCKPT_RING_DEPTH * SIMCKPT_DESC_SIZE);
    if (ctx->cq && ctx->cq != MAP_FAILED)
        munmap(ctx->cq, SIMCKPT_RING_DEPTH * SIMCKPT_CPL_SIZE);
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
simckpt_submit(struct simckpt_ctx *ctx,
               const struct simckpt_desc *desc, uint32_t n,
               int enable_intr)
{
    uint64_t sq_phys, cq_phys;

    if (!ctx || !desc || n == 0 || n > SIMCKPT_RING_DEPTH)
        return -EINVAL;

    sq_phys = simckpt_phys(ctx, ctx->sq);
    cq_phys = simckpt_phys(ctx, ctx->cq);
    if (sq_phys == (uint64_t)-1 || cq_phys == (uint64_t)-1)
        return -EIO;

    memcpy(ctx->sq, desc, (size_t)n * SIMCKPT_DESC_SIZE);
    ctx->batch_n = n;

    /* Reset the device (rings + counters), then program and ring. After the
     * reset, COMPLETED starts at 0 and the CQ is written from entry 0. */
    bar_wq(ctx, SIMCKPT_REG_CTRL, 0x2);
    bar_wq(ctx, SIMCKPT_REG_SQ_BASE, sq_phys);
    bar_wq(ctx, SIMCKPT_REG_SQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_CQ_BASE, cq_phys);
    bar_wq(ctx, SIMCKPT_REG_CQ_DEPTH, SIMCKPT_RING_DEPTH);
    bar_wq(ctx, SIMCKPT_REG_INTR_EN, enable_intr ? 1 : 0);
    bar_wq(ctx, SIMCKPT_REG_SQ_DOORBELL, n);

    return 0;
}

int
simckpt_wait(struct simckpt_ctx *ctx, uint64_t n)
{
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
    for (uint32_t i = 0; i < n; i++)
        out[i] = ctx->cq[i % SIMCKPT_RING_DEPTH];
    return 0;
}

int
simckpt_get_counters(struct simckpt_ctx *ctx, struct simckpt_counters *c)
{
    c->completed   = bar_rq(ctx, SIMCKPT_REG_COMPLETED);
    c->intr_posted = bar_rq(ctx, SIMCKPT_REG_INTR_POSTED);
    c->errors      = bar_rq(ctx, SIMCKPT_REG_ERRORS);
    c->sq_head     = bar_rq(ctx, SIMCKPT_REG_SQ_HEAD);
    c->cq_tail     = bar_rq(ctx, SIMCKPT_REG_CQ_TAIL);
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
