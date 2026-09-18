// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_SHA256_H
#define COMMON_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define COMMON_SHA256_DIGEST_SIZE 32U
#define COMMON_SHA256_HEX_SIZE (COMMON_SHA256_DIGEST_SIZE * 2U + 1U)

typedef struct {
    uint32_t state[8];
    uint64_t total_size;
    uint8_t block[64];
    size_t block_size;
} CommonSha256Context;

void common_sha256_init(CommonSha256Context* context);
void common_sha256_update(CommonSha256Context* context,
                          const void* data, size_t size);
void common_sha256_final(
    CommonSha256Context* context,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);
void common_sha256(
    const void* data, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);

/* Lowercase hexadecimal encoding with a trailing NUL. */
void common_sha256_digest_to_hex(
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    char hex[COMMON_SHA256_HEX_SIZE]);

#endif /* COMMON_SHA256_H */
