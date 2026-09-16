/*
 * SimCkptDevice P2 functional test (guest side): parallel storage.
 *
 * Drives the SimCkptDevice through its BAR0 (no kernel driver) and exercises
 * the full checkpoint save/restore path through the ParallelStorage backend:
 *
 *   phase 0  SAVE    DRAM -> storage  (src on NUMA node 0)
 *   phase 1  RESTORE storage -> DRAM   (dst on NUMA node 0)
 *   phase 2  SAVE    CXL  -> storage  (src on NUMA node 1)
 *   phase 3  RESTORE storage -> CXL    (dst on NUMA node 1)
 *
 * Each phase submits `num_chunks` descriptors (chunk_size 4KiB) so the
 * ParallelStorage stripes them across its channels. Data integrity is verified
 * via the per-chunk CRC returned in each completion plus a byte-for-byte
 * read-back of the restored destination.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "virt2phy.h"

#define DESC_SIZE 64
#define CPL_SIZE 64

#define FLAG_SAVE    1u
#define FLAG_RESTORE 2u

#define REG_CTRL        0x00
#define REG_STATUS      0x08
#define REG_SQ_BASE     0x10
#define REG_SQ_DEPTH    0x18
#define REG_CQ_BASE     0x20
#define REG_CQ_DEPTH    0x28
#define REG_SQ_DOORBELL 0x30
#define REG_CQ_DOORBELL 0x38
#define REG_COMPLETED   0x40
#define REG_INTR_EN     0x48
#define REG_INTR_POSTED 0x50
#define REG_SQ_HEAD     0x58
#define REG_CQ_TAIL     0x60
#define REG_ERRORS      0x68

struct simckpt_desc {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint64_t storage_offset;
    uint32_t length;
    uint32_t checkpoint_id;
    uint32_t chunk_id;
    uint32_t flags;
    uint32_t crc32;
    uint32_t reserved[5];
} __attribute__((packed));

struct simckpt_cpl {
    uint32_t checkpoint_id;
    uint32_t chunk_id;
    uint32_t status;
    uint32_t crc32;
    uint32_t reserved[12];
} __attribute__((packed));

static uint32_t crc_table[256];
static int crc_table_init;

static void
init_crc_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_init = 1;
}

static uint32_t
crc32(const uint8_t *data, size_t len)
{
    if (!crc_table_init)
        init_crc_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFU;
}

#ifndef SYS_mbind
#define SYS_mbind 237
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

static uint64_t
parse_hex(const char *s)
{
    return strtoull(s, NULL, 0);
}

static uint64_t
find_bar0_from_sysfs(void)
{
    const char *path = "/sys/bus/pci/devices/0000:00:07.0/resource";
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    uint64_t start = 0, end = 0;
    int n = fscanf(f, "0x%" SCNx64 " 0x%" SCNx64, &start, &end);
    fclose(f);
    if (n != 2)
        return 0;
    return start;
}

static void
fill_pattern(uint8_t *buf, size_t len, uint32_t ckpt, uint32_t chunk)
{
    uint32_t state = ckpt * 2654435761U + chunk * 40503U;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
}

static double
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + ts.tv_nsec;
}

struct Dev
{
    volatile uint8_t *bar;
    void wreg(uint64_t off, uint64_t v) {
        *(volatile uint64_t *)(bar + off) = v;
    }
    uint64_t rreg(uint64_t off) {
        return *(volatile uint64_t *)(bar + off);
    }
};

/* Submit `num_chunks` descriptors and wait for their completions. */
static uint64_t
submit_and_wait(Dev &dev, struct simckpt_desc *sq, struct simckpt_cpl *cq,
                uint64_t sq_phys, uint64_t cq_phys, uint32_t sq_depth,
                uint32_t cq_depth, uint32_t num_chunks)
{
    dev.wreg(REG_CTRL, 0x2);  /* reset rings + counters */
    dev.wreg(REG_SQ_BASE, sq_phys);
    dev.wreg(REG_SQ_DEPTH, sq_depth);
    dev.wreg(REG_CQ_BASE, cq_phys);
    dev.wreg(REG_CQ_DEPTH, cq_depth);
    dev.wreg(REG_INTR_EN, 1);
    dev.wreg(REG_SQ_DOORBELL, num_chunks);

    uint64_t completed = 0;
    for (int spins = 0; spins < 10000000; spins++) {
        completed = dev.rreg(REG_COMPLETED);
        if (completed >= num_chunks)
            break;
    }
    return completed;
}

/* Verify completions carry the expected per-chunk CRC (matched by chunk_id). */
static int
verify_crcs(struct simckpt_cpl *cq, const uint8_t *ref,
            uint32_t num_chunks, uint32_t chunk_len, const char *tag)
{
    int pass = 1;
    bool seen[64] = {false};
    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t c = cq[i].chunk_id;
        if (c >= num_chunks || cq[i].status != 0) {
            printf("[%s] FAIL: cpl %u invalid (id=%u status=%u)\n",
                   tag, i, c, cq[i].status);
            pass = 0;
            continue;
        }
        seen[c] = true;
        uint32_t expect = crc32(ref + (size_t)c * chunk_len, chunk_len);
        if (cq[i].crc32 != expect) {
            printf("[%s] FAIL: chunk %u cpl crc=%#x expect=%#x\n",
                   tag, c, cq[i].crc32, expect);
            pass = 0;
        }
    }
    for (uint32_t c = 0; c < num_chunks; c++) {
        if (!seen[c]) {
            printf("[%s] FAIL: chunk %u completion missing\n", tag, c);
            pass = 0;
        }
    }
    return pass;
}

/* SAVE: memory -> storage. */
static int
run_save(Dev &dev, struct simckpt_desc *sq, struct simckpt_cpl *cq,
         uint64_t sq_phys, uint64_t cq_phys, uint32_t sq_depth,
         uint32_t cq_depth, uint8_t *src, uint32_t num_chunks,
         uint32_t chunk_len, uint32_t ckpt_id, const char *tag)
{
    memset(sq, 0, sq_depth * DESC_SIZE);
    memset(cq, 0, cq_depth * CPL_SIZE);
    for (uint32_t c = 0; c < num_chunks; c++) {
        sq[c].src_addr = virt2phy2(src + (size_t)c * chunk_len);
        sq[c].storage_offset = (uint64_t)c * chunk_len;
        sq[c].length = chunk_len;
        sq[c].checkpoint_id = ckpt_id;
        sq[c].chunk_id = c;
        sq[c].flags = FLAG_SAVE;
    }

    uint64_t completed = submit_and_wait(dev, sq, cq, sq_phys, cq_phys,
                                         sq_depth, cq_depth, num_chunks);
    printf("[%s] completed=%" PRIu64 " (expected %u), intr_posted=%" PRIu64
           ", errors=%" PRIu64 "\n", tag, completed, num_chunks,
           dev.rreg(REG_INTR_POSTED), dev.rreg(REG_ERRORS));

    int pass = (completed == num_chunks);
    if (!pass)
        printf("[%s] FAIL: completion count mismatch\n", tag);
    pass &= verify_crcs(cq, src, num_chunks, chunk_len, tag);
    return pass;
}

/* RESTORE: storage -> memory, then read back the destination. */
static int
run_restore(Dev &dev, struct simckpt_desc *sq, struct simckpt_cpl *cq,
            uint64_t sq_phys, uint64_t cq_phys, uint32_t sq_depth,
            uint32_t cq_depth, const uint8_t *src, uint8_t *dst,
            uint32_t num_chunks, uint32_t chunk_len, uint32_t ckpt_id,
            const char *tag)
{
    memset(sq, 0, sq_depth * DESC_SIZE);
    memset(cq, 0, cq_depth * CPL_SIZE);
    for (uint32_t c = 0; c < num_chunks; c++) {
        sq[c].dst_addr = virt2phy2(dst + (size_t)c * chunk_len);
        sq[c].storage_offset = (uint64_t)c * chunk_len;
        sq[c].length = chunk_len;
        sq[c].checkpoint_id = ckpt_id;
        sq[c].chunk_id = c;
        sq[c].flags = FLAG_RESTORE;
    }

    uint64_t completed = submit_and_wait(dev, sq, cq, sq_phys, cq_phys,
                                         sq_depth, cq_depth, num_chunks);
    printf("[%s] completed=%" PRIu64 " (expected %u), intr_posted=%" PRIu64
           ", errors=%" PRIu64 "\n", tag, completed, num_chunks,
           dev.rreg(REG_INTR_POSTED), dev.rreg(REG_ERRORS));

    int pass = (completed == num_chunks);
    if (!pass)
        printf("[%s] FAIL: completion count mismatch\n", tag);
    /* storage data == src, so the completion CRC must equal crc32(src). */
    pass &= verify_crcs(cq, src, num_chunks, chunk_len, tag);

    /* Read-back: restored destination must equal the original source. */
    for (uint32_t c = 0; c < num_chunks; c++) {
        if (memcmp(src + (size_t)c * chunk_len,
                   dst + (size_t)c * chunk_len, chunk_len) != 0) {
            printf("[%s] FAIL: chunk %u destination mismatch\n", tag, c);
            pass = 0;
        }
    }
    return pass;
}

int
main(int argc, char **argv)
{
    const uint32_t num_chunks = 4;
    const uint32_t chunk_len = 4096;
    const uint32_t sq_depth = 16;
    const uint32_t cq_depth = 16;

    uint64_t bar0 = 0;
    if (argc > 1 && argv[1][0] != '\0')
        bar0 = parse_hex(argv[1]);
    if (bar0 == 0)
        bar0 = find_bar0_from_sysfs();
    if (bar0 == 0) {
        fprintf(stderr, "FAIL: cannot locate SimCkptDevice BAR0\n");
        return 1;
    }
    printf("SimCkptDevice BAR0 = 0x%" PRIx64 "\n", bar0);

    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) {
        perror("open /dev/mem");
        return 1;
    }
    size_t bar_size = 64 * 1024;
    volatile uint8_t *bar = (volatile uint8_t *)mmap(
        NULL, bar_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, bar0);
    if (bar == MAP_FAILED) {
        perror("mmap BAR0");
        return 1;
    }
    Dev dev{bar};

    size_t total = (size_t)num_chunks * chunk_len;
    uint8_t *dram_src = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *dram_dst = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *cxl_src = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *cxl_dst = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct simckpt_desc *sq = (struct simckpt_desc *)mmap(
        NULL, sq_depth * DESC_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct simckpt_cpl *cq = (struct simckpt_cpl *)mmap(
        NULL, cq_depth * CPL_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dram_src == MAP_FAILED || dram_dst == MAP_FAILED ||
        cxl_src == MAP_FAILED || cxl_dst == MAP_FAILED ||
        sq == MAP_FAILED || cq == MAP_FAILED) {
        perror("mmap buffers");
        return 1;
    }

    memset(dram_src, 0, total);
    memset(dram_dst, 0xAA, total);
    memset(cxl_src, 0, total);
    memset(cxl_dst, 0xAA, total);
    memset(sq, 0, sq_depth * DESC_SIZE);
    memset(cq, 0, cq_depth * CPL_SIZE);

    for (uint32_t c = 0; c < num_chunks; c++) {
        fill_pattern(dram_src + (size_t)c * chunk_len, chunk_len, 1, c);
        fill_pattern(cxl_src + (size_t)c * chunk_len, chunk_len, 2, c);
    }

    unsigned long node1 = 1UL << 1;
    do_mbind(cxl_src, total, MPOL_BIND, &node1, 64, MPOL_MF_MOVE | MPOL_MF_STRICT);
    do_mbind(cxl_dst, total, MPOL_BIND, &node1, 64, MPOL_MF_MOVE | MPOL_MF_STRICT);

    uint64_t sq_phys = virt2phy2(sq);
    uint64_t cq_phys = virt2phy2(cq);
    if (sq_phys == (uint64_t)-1 || cq_phys == (uint64_t)-1) {
        fprintf(stderr, "FAIL: virt2phy of SQ/CQ failed\n");
        return 1;
    }
    printf("SQ phys=0x%" PRIx64 " CQ phys=0x%" PRIx64 "\n", sq_phys, cq_phys);

    int pass = 1;
    pass &= run_save(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                     dram_src, num_chunks, chunk_len, 1, "SAVE DRAM->stor ");
    pass &= run_restore(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                        dram_src, dram_dst, num_chunks, chunk_len, 1,
                        "RESTORE stor->DRAM");
    pass &= run_save(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                     cxl_src, num_chunks, chunk_len, 2, "SAVE CXL->stor  ");
    pass &= run_restore(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                        cxl_src, cxl_dst, num_chunks, chunk_len, 2,
                        "RESTORE stor->CXL ");

    if (memcmp(dram_src, dram_dst, total) != 0) {
        printf("FAIL: DRAM round-trip mismatch\n");
        pass = 0;
    } else {
        printf("DRAM round-trip OK\n");
    }
    if (memcmp(cxl_src, cxl_dst, total) != 0) {
        printf("FAIL: CXL round-trip mismatch\n");
        pass = 0;
    } else {
        printf("CXL round-trip OK\n");
    }

    printf("%s\n", pass ? "PASS" : "FAIL");

    /* Optional bandwidth benchmark: argv[2] = save size in MiB. */
    if (argc > 2) {
        size_t bench_mib = strtoull(argv[2], NULL, 0);
        const uint32_t bchunk_len = 4096;
        uint32_t bchunks = (uint32_t)((bench_mib << 20) / bchunk_len);
        if (bchunks == 0)
            bchunks = 1;

        size_t btotal = (size_t)bchunks * bchunk_len;
        uint8_t *bsrc = (uint8_t *)mmap(NULL, btotal, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint32_t bsq_depth = bchunks;
        uint32_t bcq_depth = bchunks;
        struct simckpt_desc *bsq = (struct simckpt_desc *)mmap(
            NULL, bsq_depth * DESC_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct simckpt_cpl *bcq = (struct simckpt_cpl *)mmap(
            NULL, bcq_depth * CPL_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (bsrc == MAP_FAILED || bsq == MAP_FAILED || bcq == MAP_FAILED) {
            fprintf(stderr, "bench mmap failed\n");
            return pass ? 0 : 1;
        }
        memset(bsrc, 0, btotal);
        memset(bsq, 0, bsq_depth * DESC_SIZE);
        memset(bcq, 0, bcq_depth * CPL_SIZE);
        for (uint32_t c = 0; c < bchunks; c++)
            fill_pattern(bsrc + (size_t)c * bchunk_len, bchunk_len, 9, c);

        uint64_t bsq_phys = virt2phy2(bsq);
        uint64_t bcq_phys = virt2phy2(bcq);
        if (bsq_phys == (uint64_t)-1 || bcq_phys == (uint64_t)-1) {
            fprintf(stderr, "bench virt2phy failed\n");
            return pass ? 0 : 1;
        }

        for (uint32_t c = 0; c < bchunks; c++) {
            bsq[c].src_addr = virt2phy2(bsrc + (size_t)c * bchunk_len);
            bsq[c].storage_offset = (uint64_t)c * bchunk_len;
            bsq[c].length = bchunk_len;
            bsq[c].checkpoint_id = 9;
            bsq[c].chunk_id = c;
            bsq[c].flags = FLAG_SAVE;
        }

        dev.wreg(REG_CTRL, 0x2);
        dev.wreg(REG_SQ_BASE, bsq_phys);
        dev.wreg(REG_SQ_DEPTH, bsq_depth);
        dev.wreg(REG_CQ_BASE, bcq_phys);
        dev.wreg(REG_CQ_DEPTH, bcq_depth);
        dev.wreg(REG_INTR_EN, 0);

        double t0 = now_ns();
        dev.wreg(REG_SQ_DOORBELL, bchunks);
        uint64_t bdone = 0;
        for (int spins = 0; spins < 100000000; spins++) {
            bdone = dev.rreg(REG_COMPLETED);
            if (bdone >= bchunks)
                break;
        }
        double t1 = now_ns();
        double secs = (t1 - t0) / 1e9;
        double gbps = (double)btotal / secs / 1e9;
        printf("BENCH save %zu MiB (%u chunks): %.6f s = %.3f GB/s "
               "(completed=%" PRIu64 ")\n",
               bench_mib, bchunks, secs, gbps, bdone);
    }

    return pass ? 0 : 1;
}
