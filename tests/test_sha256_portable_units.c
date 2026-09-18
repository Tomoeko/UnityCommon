#include "common/sha256.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static bool digest_matches(const void* data, size_t size,
                           const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(data, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    return strcmp(hex, expected) == 0;
}

int main(void) {
    static const char abc[] = "abc";
    static const char multiblock[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(digest_matches(
        NULL, 0U,
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855"));
    CHECK(digest_matches(
        abc, sizeof(abc) - 1U,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad"));
    CHECK(digest_matches(
        multiblock, sizeof(multiblock) - 1U,
        "248d6a61d20638b8e5c026930c3e6039"
        "a33ce45964ff2167f6ecedd419db06c1"));

    uint8_t pattern[1025];
    for (size_t index = 0U; index < sizeof(pattern); ++index) {
        pattern[index] = (uint8_t)(index * 37U + index / 7U);
    }
    uint8_t expected[COMMON_SHA256_DIGEST_SIZE];
    uint8_t streamed[COMMON_SHA256_DIGEST_SIZE];
    uint8_t cloned[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(pattern, sizeof(pattern), expected);

    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, NULL, 0U);
    for (size_t index = 0U; index < sizeof(pattern); ++index) {
        common_sha256_update(&context, pattern + index, 1U);
    }
    common_sha256_final(&context, streamed);
    CHECK(memcmp(streamed, expected, sizeof(streamed)) == 0);

    CommonSha256Context clone;
    common_sha256_init(&context);
    common_sha256_update(&context, pattern, 513U);
    clone = context;
    common_sha256_update(
        &context, pattern + 513U, sizeof(pattern) - 513U);
    common_sha256_update(
        &clone, pattern + 513U, sizeof(pattern) - 513U);
    common_sha256_final(&context, streamed);
    common_sha256_final(&clone, cloned);
    CHECK(memcmp(streamed, expected, sizeof(streamed)) == 0);
    CHECK(memcmp(cloned, expected, sizeof(cloned)) == 0);

    puts("UnityCommon portable SHA-256 unit tests passed.");
    return 0;
}
