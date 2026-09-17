/* SPDX-License-Identifier: MIT */
/*
 * sha256 - minimal, self-contained SHA-256 (FIPS 180-4) for checkpoint
 * integrity. Used to compute a single digest over the whole checkpoint in
 * addition to the per-chunk CRC32 that the device already produces.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    size_t buf_len;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t digest[SHA256_DIGEST_SIZE]);

/* Render a digest as lowercase hex (out must be >= 65 bytes). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[65]);

#endif /* SHA256_H */
