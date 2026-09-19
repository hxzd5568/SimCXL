/* SPDX-License-Identifier: GPL-2.0 */
/*
 * simckpt_uapi.h - UAPI (ABI) shared between the simckpt.ko driver and the
 * user-space libckpt / ckptd / ckptbench stack.
 *
 * Freezes the wire format of the SimCkptDevice submission/completion rings
 * and the BAR0 register layout (see src/dev/storage/sim_ckpt_device.hh), plus
 * the ioctl() command set.
 */
#ifndef _UAPI_SIMCKPT_H
#define _UAPI_SIMCKPT_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

/* --- BAR0 register offsets (64-bit, little-endian) --------------------- */
#define SIMCKPT_REG_CTRL          0x00
#define SIMCKPT_REG_STATUS        0x08
#define SIMCKPT_REG_SQ_BASE       0x10
#define SIMCKPT_REG_SQ_DEPTH      0x18
#define SIMCKPT_REG_CQ_BASE       0x20
#define SIMCKPT_REG_CQ_DEPTH      0x28
#define SIMCKPT_REG_SQ_DOORBELL   0x30
#define SIMCKPT_REG_CQ_DOORBELL   0x38
#define SIMCKPT_REG_COMPLETED     0x40
#define SIMCKPT_REG_INTR_EN       0x48
#define SIMCKPT_REG_INTR_POSTED   0x50
#define SIMCKPT_REG_SQ_HEAD       0x58
#define SIMCKPT_REG_CQ_TAIL       0x60
#define SIMCKPT_REG_ERRORS        0x68

/* --- P10 multi-queue control + per-link statistics --------------------- */
#define SIMCKPT_REG_QUEUE_SEL        0x70
#define SIMCKPT_REG_NUM_QUEUES       0x78
#define SIMCKPT_REG_Q_OUTSTANDING    0x80   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_RETRY          0x88   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_QUEUE_FULL     0x90   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_COMPLETED_BYTES 0x98  /* read-only, selected queue     */
#define SIMCKPT_REG_Q_LATENCY_AVG    0xA0   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_LATENCY_P95    0xA8   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_ISSUE_TICK     0xB0   /* read-only, selected queue     */
#define SIMCKPT_REG_Q_DONE_TICK      0xB8   /* read-only, selected queue     */

/* --- Descriptor / completion entry sizes (cache-line sized, 64 B) ------ */
#define SIMCKPT_DESC_SIZE  64
#define SIMCKPT_CPL_SIZE   64

/* --- Descriptor flags -------------------------------------------------- */
#define SIMCKPT_FLAG_SAVE     (1u << 0)  /* memory(src) -> storage          */
#define SIMCKPT_FLAG_RESTORE  (1u << 1)  /* storage -> memory(dst)         */
#define SIMCKPT_FLAG_STAGE    (1u << 2)  /* GPU staging: generate PRNG      */
                                         /* payload -> dst_addr (DRAM/CXL)  */

/* --- Descriptor (submission queue entry), packed to 64 B --------------- */
struct simckpt_desc {
    uint64_t src_addr;         /* +0  save: source in DRAM/CXL                */
    uint64_t dst_addr;         /* +8  restore: dest in DRAM/CXL               */
    uint64_t storage_offset;   /* +16 logical offset in the checkpoint ns     */
    uint32_t length;           /* +24 payload bytes (<= chunk_size)           */
    uint32_t checkpoint_id;    /* +28                                         */
    uint32_t chunk_id;         /* +32                                         */
    uint32_t flags;            /* +36 SIMCKPT_FLAG_SAVE / _RESTORE            */
    uint32_t crc32;            /* +40 expected CRC (0 = don't check)          */
    uint32_t reserved[5];      /* +44 pad to 64 B                             */
} __attribute__((packed));

/* --- Completion (completion queue entry), packed to 64 B --------------- */
struct simckpt_cpl {
    uint32_t checkpoint_id;
    uint32_t chunk_id;
    uint32_t status;           /* 0 = success                                 */
    uint32_t crc32;            /* CRC computed by the device over the payload */
    uint32_t reserved[12];     /* pad to 64 B                                 */
} __attribute__((packed));

/* --- Device counters exposed to ckptd (read-only BAR registers) -------- */
struct simckpt_counters {
    uint64_t completed;        /* total completions posted                    */
    uint64_t intr_posted;      /* total interrupts posted                     */
    uint64_t errors;           /* descriptor errors                           */
    uint64_t sq_head;          /* device-owned SQ head                        */
    uint64_t cq_tail;          /* device-owned CQ tail                        */
};

/* --- ioctl() command set ---------------------------------------------- */
#define SIMCKPT_IOC_MAGIC 'k'

/* Pin a user buffer and return its DMA (physical) address.
 * arg: struct simckpt_register_arg (in: buf/len, out: dma_addr)           */
#define SIMCKPT_IOC_REGISTER \
    _IOWR(SIMCKPT_IOC_MAGIC, 1, struct simckpt_register_arg)

/* Unpin a previously registered buffer. arg: uint64_t dma_addr               */
#define SIMCKPT_IOC_UNREGISTER \
    _IOW(SIMCKPT_IOC_MAGIC, 2, uint64_t)

/* Program the SQ/CQ bases and depths, then ring the doorbell.
 * arg: struct simckpt_submit_arg                                           */
#define SIMCKPT_IOC_SUBMIT \
    _IOW(SIMCKPT_IOC_MAGIC, 3, struct simckpt_submit_arg)

/* Block until `count` more completions have been posted.
 * arg: uint64_t count (in)                                                    */
#define SIMCKPT_IOC_WAIT \
    _IOW(SIMCKPT_IOC_MAGIC, 4, uint64_t)

/* Read the device counters. arg: struct simckpt_counters (out)            */
#define SIMCKPT_IOC_GET_COUNTERS \
    _IOR(SIMCKPT_IOC_MAGIC, 5, struct simckpt_counters)

#define SIMCKPT_IOC_MAXNR 5

struct simckpt_register_arg {
    uint64_t buf;              /* user virtual address                        */
    uint64_t len;              /* length in bytes                             */
    uint64_t dma_addr;         /* out: DMA (physical) address                 */
};

struct simckpt_submit_arg {
    uint64_t sq_base;          /* physical address of the SQ                  */
    uint32_t sq_depth;         /* number of SQ entries                        */
    uint64_t cq_base;          /* physical address of the CQ                  */
    uint32_t cq_depth;         /* number of CQ entries                        */
    uint32_t sq_tail;          /* new SQ tail (doorbell value)                */
    uint32_t flags;            /* bit0: enable interrupt posting              */
};

#endif /* _UAPI_SIMCKPT_H */
