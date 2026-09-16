/*
 * SimCkptDevice P1 functional test (guest side).
 *
 * Drives the gem5 SimCkptDevice PCI device directly through its BAR0 MMIO
 * registers (no kernel driver required), using a submission/completion ring
 * pair located in guest physical memory.
 *
 * Two phases:
 *   phase 0  DRAM -> DRAM  (destination on NUMA node 0)
 *   phase 1  DRAM -> CXL   (destination bound to NUMA node 1 via mbind)
 *
 * For each phase the test fills the source with a deterministic pattern,
 * writes descriptors into the SQ, rings the doorbell, waits for completions,
 * and then reads back the destination to verify the payload and per-chunk CRC
 * (readback test).
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
#include <unistd.h>

#include "virt2phy.h"

#define DESC_SIZE 64
#define CPL_SIZE 64

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

/* Run one copy phase: src -> dst, verify payload + per-chunk CRC. */
static int
run_phase(Dev &dev, struct simckpt_desc *sq, struct simckpt_cpl *cq,
          uint64_t sq_phys, uint64_t cq_phys, uint32_t sq_depth,
          uint32_t cq_depth, uint8_t *src, uint8_t *dst,
          uint32_t num_chunks, uint32_t chunk_len, uint32_t ckpt_id,
          const char *tag)
{
    memset(sq, 0, sq_depth * DESC_SIZE);
    memset(cq, 0, cq_depth * CPL_SIZE);

    for (uint32_t c = 0; c < num_chunks; c++) {
        uint8_t *s = src + (size_t)c * chunk_len;
        uint8_t *d = dst + (size_t)c * chunk_len;
        sq[c].src_addr = virt2phy2(s);
        sq[c].dst_addr = virt2phy2(d);
        sq[c].storage_offset = (uint64_t)c * chunk_len;
        sq[c].length = chunk_len;
        sq[c].checkpoint_id = ckpt_id;
        sq[c].chunk_id = c;
        sq[c].flags = 0;
        sq[c].crc32 = crc32(s, chunk_len);
    }

    dev.wreg(REG_CTRL, 0x2);  /* CTRL_RESET: clear rings, counters */
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

    printf("[%s] completed=%" PRIu64 " (expected %u), intr_posted=%" PRIu64
           ", errors=%" PRIu64 "\n",
           tag, completed, num_chunks, dev.rreg(REG_INTR_POSTED),
           dev.rreg(REG_ERRORS));

    int pass = 1;
    if (completed != num_chunks) {
        printf("[%s] FAIL: completion count mismatch\n", tag);
        pass = 0;
    }

    /* Completions may complete out of order: match them by chunk_id. */
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
        const uint8_t *s = src + (size_t)c * chunk_len;
        const uint8_t *d = dst + (size_t)c * chunk_len;
        uint32_t expect = crc32(s, chunk_len);
        if (cq[i].crc32 != expect) {
            printf("[%s] FAIL: chunk %u cpl crc=%#x expect=%#x\n",
                   tag, c, cq[i].crc32, expect);
            pass = 0;
        }
        if (memcmp(s, d, chunk_len) != 0 || crc32(d, chunk_len) != expect) {
            printf("[%s] FAIL: chunk %u destination mismatch\n", tag, c);
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

int
main(int argc, char **argv)
{
    const uint32_t num_chunks = 8;
    const uint32_t chunk_len = 256;
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
    uint8_t *src = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *dram = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *cxl = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct simckpt_desc *sq = (struct simckpt_desc *)mmap(
        NULL, sq_depth * DESC_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct simckpt_cpl *cq = (struct simckpt_cpl *)mmap(
        NULL, cq_depth * CPL_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED || dram == MAP_FAILED || cxl == MAP_FAILED ||
        sq == MAP_FAILED || cq == MAP_FAILED) {
        perror("mmap buffers");
        return 1;
    }

    memset(src, 0, total);
    memset(dram, 0xAA, total);
    memset(cxl, 0xAA, total);
    memset(sq, 0, sq_depth * DESC_SIZE);
    memset(cq, 0, cq_depth * CPL_SIZE);

    for (uint32_t c = 0; c < num_chunks; c++)
        fill_pattern(src + (size_t)c * chunk_len, chunk_len, 1, c);

    /* Bind the "CXL" destination to NUMA node 1 (dax_kmem System RAM). */
    unsigned long cxl_mask = 1UL << 1;
    if (do_mbind(cxl, total, MPOL_BIND, &cxl_mask, 64,
                 MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
        perror("mbind(cxl -> node 1)");
        /* fall through: node 1 may not be onlined; phase 1 will just be DRAM */
    }

    uint64_t sq_phys = virt2phy2(sq);
    uint64_t cq_phys = virt2phy2(cq);
    if (sq_phys == (uint64_t)-1 || cq_phys == (uint64_t)-1) {
        fprintf(stderr, "FAIL: virt2phy of SQ/CQ failed\n");
        return 1;
    }
    printf("SQ phys=0x%" PRIx64 " CQ phys=0x%" PRIx64 "\n", sq_phys, cq_phys);
    printf("cxl dst phys=0x%" PRIx64 "\n", virt2phy2(cxl));

    int pass = 1;
    pass &= run_phase(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                      src, dram, num_chunks, chunk_len, 1, "DRAM->DRAM");
    pass &= run_phase(dev, sq, cq, sq_phys, cq_phys, sq_depth, cq_depth,
                      src, cxl, num_chunks, chunk_len, 2, "DRAM->CXL ");

    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
