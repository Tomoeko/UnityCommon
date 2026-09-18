// SPDX-License-Identifier: GPL-3.0-only

#include "common/sha256.h"

#include <string.h>

#if defined(__APPLE__) && !defined(UNITY_COMMON_FORCE_PORTABLE_SHA256)
#include <CommonCrypto/CommonDigest.h>

_Static_assert(COMMON_SHA256_DIGEST_SIZE == CC_SHA256_DIGEST_LENGTH,
               "CommonCrypto SHA-256 digest size mismatch");
_Static_assert(sizeof(CC_LONG) == sizeof(uint32_t),
               "CommonCrypto SHA-256 update length is not 32-bit");
_Static_assert(sizeof(CommonSha256Context) >= sizeof(CC_SHA256_CTX),
               "CommonSha256Context cannot hold CommonCrypto state");

/* Keep CommonSha256Context's public layout and ABI independent of Apple SDK
 * headers.  Copying the native state in and out also avoids accessing a
 * CommonSha256Context object through an incompatible CC_SHA256_CTX pointer. */
static void commoncrypto_context_load(
    const CommonSha256Context* context, CC_SHA256_CTX* native_context) {
    memcpy(native_context, context, sizeof(*native_context));
}

static void commoncrypto_context_store(
    CommonSha256Context* context, const CC_SHA256_CTX* native_context) {
    memcpy(context, native_context, sizeof(*native_context));
}

void common_sha256_init(CommonSha256Context* context) {
    CC_SHA256_CTX native_context;
    memset(&native_context, 0, sizeof(native_context));
    (void)CC_SHA256_Init(&native_context);
    memset(context, 0, sizeof(*context));
    commoncrypto_context_store(context, &native_context);
}

void common_sha256_update(CommonSha256Context* context,
                          const void* input, size_t input_size) {
    if (input_size == 0U) return;

    CC_SHA256_CTX native_context;
    commoncrypto_context_load(context, &native_context);
    const uint8_t* data = (const uint8_t*)input;
    while (input_size != 0U) {
        const CC_LONG chunk_size = input_size > (size_t)UINT32_MAX
            ? (CC_LONG)UINT32_MAX : (CC_LONG)input_size;
        (void)CC_SHA256_Update(&native_context, data, chunk_size);
        data += (size_t)chunk_size;
        input_size -= (size_t)chunk_size;
    }
    commoncrypto_context_store(context, &native_context);
}

void common_sha256_final(
    CommonSha256Context* context,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    CC_SHA256_CTX native_context;
    commoncrypto_context_load(context, &native_context);
    (void)CC_SHA256_Final(digest, &native_context);
    commoncrypto_context_store(context, &native_context);
}

#else

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t load_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static void store_be32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static void store_be64(uint8_t* data, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
        data[7U - i] = (uint8_t)(value >> (i * 8U));
    }
}

static void sha256_transform(CommonSha256Context* context,
                             const uint8_t block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t words[64];
    for (unsigned i = 0; i < 16; i++) {
        words[i] = load_be32(block + i * 4U);
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = words[i - 15U];
        uint32_t y = words[i - 2U];
        uint32_t small0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^
                          (x >> 3U);
        uint32_t small1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^
                          (y >> 10U);
        words[i] = words[i - 16U] + small0 + words[i - 7U] + small1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t big1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + big1 + choose + constants[i] + words[i];
        uint32_t big0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = big0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void common_sha256_init(CommonSha256Context* context) {
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->total_size = 0;
    context->block_size = 0;
}

void common_sha256_update(CommonSha256Context* context,
                          const void* input, size_t input_size) {
    const uint8_t* data = (const uint8_t*)input;
    context->total_size += (uint64_t)input_size;
    while (input_size > 0) {
        size_t available = sizeof(context->block) - context->block_size;
        size_t copy_size = input_size < available ? input_size : available;
        memcpy(context->block + context->block_size, data, copy_size);
        context->block_size += copy_size;
        data += copy_size;
        input_size -= copy_size;
        if (context->block_size == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->block_size = 0;
        }
    }
}

void common_sha256_final(
    CommonSha256Context* context,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    uint64_t bit_size = context->total_size * 8U;
    context->block[context->block_size++] = 0x80U;
    if (context->block_size > 56U) {
        memset(context->block + context->block_size, 0,
               sizeof(context->block) - context->block_size);
        sha256_transform(context, context->block);
        context->block_size = 0;
    }
    memset(context->block + context->block_size, 0,
           56U - context->block_size);
    store_be64(context->block + 56U, bit_size);
    sha256_transform(context, context->block);
    for (unsigned i = 0; i < 8; i++) {
        store_be32(digest + i * 4U, context->state[i]);
    }
}

#endif

void common_sha256(
    const void* data, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    CommonSha256Context context;
    common_sha256_init(&context);
    if (size > 0) common_sha256_update(&context, data, size);
    common_sha256_final(&context, digest);
}

void common_sha256_digest_to_hex(
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    char hex[COMMON_SHA256_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    if (!digest || !hex) return;
    for (size_t i = 0U; i < COMMON_SHA256_DIGEST_SIZE; ++i) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[COMMON_SHA256_HEX_SIZE - 1U] = '\0';
}
