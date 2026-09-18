#include "io/lz4_decompress.h"

#include "common/common.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum {
    GUARD_BYTES = 16,
    FIXTURE_BYTES = 131600,
    METADATA_CAP = 128 * 1024,
    SOURCE_CANARY = 0xa5,
    DESTINATION_CANARY = 0xcc
};

typedef enum DecodeMode {
    EXACT_BLOCK,
    LEGACY_BLOCK
} DecodeMode;

static uint8_t guarded_source[FIXTURE_BYTES + 2 * GUARD_BYTES];
static uint8_t source_snapshot[sizeof(guarded_source)];
static uint8_t guarded_destination[FIXTURE_BYTES + 2 * GUARD_BYTES];
static uint8_t encoded_fixture[FIXTURE_BYTES];
static uint8_t expected_fixture[FIXTURE_BYTES];
static size_t checked_calls;

static bool repeated_byte(const uint8_t* bytes, size_t count, uint8_t expected) {
    for (size_t index = 0; index < count; ++index) {
        if (bytes[index] != expected) {
            return false;
        }
    }
    return true;
}

static bool expect_decode(const char* scenario,
    DecodeMode mode,
    const uint8_t* source,
    size_t source_bytes,
    size_t destination_bytes,
    int expected_result,
    const uint8_t* expected) {
    CHECK(source_bytes <= FIXTURE_BYTES && destination_bytes <= FIXTURE_BYTES);
    CHECK(source != NULL || source_bytes == 0);
    CHECK(expected_result <= (int)destination_bytes);
    CHECK(expected != NULL || expected_result <= 0);
    memset(guarded_source, SOURCE_CANARY, sizeof(guarded_source));
    if (source_bytes != 0) {
        memcpy(guarded_source + GUARD_BYTES, source, source_bytes);
    }
    memcpy(source_snapshot, guarded_source, sizeof(source_snapshot));
    memset(guarded_destination, DESTINATION_CANARY, sizeof(guarded_destination));
    size_t allocation_count = atomic_load(&g_allocations_count);
    size_t allocated_bytes = atomic_load(&g_allocated_bytes);
    const uint8_t* input = guarded_source + GUARD_BYTES;
    uint8_t* output = guarded_destination + GUARD_BYTES;
    int result = mode == EXACT_BLOCK
        ? lz4_decompress_exact(input, output, (int)source_bytes, (int)destination_bytes)
        : lz4_decompress_safe(input, output, (int)source_bytes, (int)destination_bytes);
    ++checked_calls;
    if (result != expected_result) {
        fprintf(stderr, "%s: expected %d, received %d\n", scenario, expected_result, result);
        return false;
    }
    CHECK(atomic_load(&g_allocations_count) == allocation_count);
    CHECK(atomic_load(&g_allocated_bytes) == allocated_bytes);
    CHECK(memcmp(guarded_source, source_snapshot, sizeof(source_snapshot)) == 0);
    CHECK(repeated_byte(guarded_destination, GUARD_BYTES, DESTINATION_CANARY));
    CHECK(repeated_byte(output + destination_bytes,
        sizeof(guarded_destination) - GUARD_BYTES - destination_bytes,
        DESTINATION_CANARY));
    if (result >= 0) {
        CHECK(result == 0 || memcmp(output, expected, (size_t)result) == 0);
        CHECK(
            repeated_byte(output + result, destination_bytes - (size_t)result, DESTINATION_CANARY));
    }
    return true;
}

static bool valid_block(const char* scenario,
    const uint8_t* source,
    size_t source_bytes,
    const uint8_t* expected,
    size_t expected_bytes,
    bool every_truncation) {
    CHECK(expect_decode(scenario,
        EXACT_BLOCK,
        source,
        source_bytes,
        expected_bytes,
        (int)expected_bytes,
        expected));
    CHECK(expect_decode(scenario,
        LEGACY_BLOCK,
        source,
        source_bytes,
        expected_bytes,
        (int)expected_bytes,
        expected));
    CHECK(expect_decode(scenario, EXACT_BLOCK, source, source_bytes, expected_bytes + 1, -1, NULL));
    CHECK(expect_decode(scenario,
        LEGACY_BLOCK,
        source,
        source_bytes,
        expected_bytes + 1,
        (int)expected_bytes,
        expected));
    if (expected_bytes != 0) {
        CHECK(expect_decode(
            scenario, EXACT_BLOCK, source, source_bytes, expected_bytes - 1, -1, NULL));
    }
    if (every_truncation) {
        for (size_t size = 0; size < source_bytes; ++size) {
            CHECK(expect_decode(scenario, EXACT_BLOCK, source, size, expected_bytes, -1, NULL));
        }
    }
    return true;
}

/* Fixture encoding uses quotient/remainder; expected bytes are authored before
 * decoding and never obtained from either product entry point. */
static size_t write_extension(uint8_t* output, size_t remainder) {
    size_t count = remainder / 255;
    memset(output, 255, count);
    output[count] = (uint8_t)(remainder % 255);
    return count + 1;
}

static size_t write_literals(uint8_t* output, const uint8_t* literals, size_t count) {
    size_t position = 1;
    output[0] = (uint8_t)((count < 15 ? count : 15) << 4);
    if (count >= 15) {
        position += write_extension(output + position, count - 15);
    }
    memcpy(output + position, literals, count);
    return position + count;
}

static bool literal_lengths(void) {
    static const size_t lengths[] = {0, 1, 4, 5, 14, 15, 16, 269, 270, 271, 525, 526, 130559};
    for (size_t index = 0; index < sizeof(expected_fixture); ++index) {
        expected_fixture[index] = (uint8_t)(index % 251);
    }
    for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        size_t count = lengths[index];
        size_t source_bytes = write_literals(encoded_fixture, expected_fixture, count);
        if (count == 130559) {
            CHECK(source_bytes == METADATA_CAP);
        }
        CHECK(valid_block("literal length boundary",
            encoded_fixture,
            source_bytes,
            expected_fixture,
            count,
            count <= 16));
    }
    static const uint8_t unused_match_nibble[] = {0x5f, 'a', 'b', 'c', 'd', 'e'};
    CHECK(valid_block("unused final match nibble",
        unused_match_nibble,
        sizeof(unused_match_nibble),
        (const uint8_t*)"abcde",
        5,
        true));
    return true;
}

static bool match_lengths_and_offsets(void) {
    static const uint8_t minimum_match[] = {
        0x10, 'A', 1, 0, 0x80, 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'};
    CHECK(valid_block("minimum match with complete final window",
        minimum_match,
        sizeof(minimum_match),
        (const uint8_t*)"AAAAAbcdefghi",
        13,
        true));
    static const size_t lengths[] = {7, 18, 19, 20, 273, 274, 275, 529, 530, METADATA_CAP - 6};
    for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        size_t match = lengths[index];
        encoded_fixture[0] = (uint8_t)(0x10 | (match < 19 ? match - 4 : 15));
        encoded_fixture[1] = 'A';
        encoded_fixture[2] = 1;
        encoded_fixture[3] = 0;
        size_t position = 4;
        if (match >= 19) {
            position += write_extension(encoded_fixture + position, match - 19);
        }
        encoded_fixture[position++] = 0x50;
        memcpy(encoded_fixture + position, "bcdef", 5);
        position += 5;
        memset(expected_fixture, 'A', match + 1);
        memcpy(expected_fixture + match + 1, "bcdef", 5);
        CHECK(valid_block("overlapping offset-one match length",
            encoded_fixture,
            position,
            expected_fixture,
            match + 6,
            match <= 20));
    }

    static const uint8_t offset_two[] = {0x24, 'A', 'B', 2, 0, 0x50, 'c', 'd', 'e', 'f', 'g'};
    CHECK(valid_block("overlapping offset two",
        offset_two,
        sizeof(offset_two),
        (const uint8_t*)"ABABABABABcdefg",
        15,
        true));
    static const uint8_t zero_literals[] = {
        0x13, 'A', 1, 0, 0x03, 1, 0, 0x50, 'b', 'c', 'd', 'e', 'f'};
    CHECK(valid_block("zero literal intermediate sequence",
        zero_literals,
        sizeof(zero_literals),
        (const uint8_t*)"AAAAAAAAAAAAAAAbcdef",
        20,
        true));

    static const size_t offsets[] = {256, 65535};
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        size_t offset = offsets[index];
        size_t history = offset + (offset == 256 ? 1 : 0);
        for (size_t byte = 0; byte < history; ++byte) {
            expected_fixture[byte] = (uint8_t)(byte % 251);
        }
        encoded_fixture[0] = 0xf3;
        size_t position = 1 + write_extension(encoded_fixture + 1, history - 15);
        memcpy(encoded_fixture + position, expected_fixture, history);
        position += history;
        encoded_fixture[position++] = (uint8_t)offset;
        encoded_fixture[position++] = (uint8_t)(offset >> 8);
        encoded_fixture[position++] = 0x50;
        memcpy(encoded_fixture + position, "abcde", 5);
        position += 5;
        memcpy(expected_fixture + history, expected_fixture + history - offset, 7);
        memcpy(expected_fixture + history + 7, "abcde", 5);
        CHECK(valid_block("little-endian offset boundary",
            encoded_fixture,
            position,
            expected_fixture,
            history + 12,
            false));
    }
    return true;
}

typedef struct MalformedBlock {
    const char* name;
    const uint8_t* source;
    size_t source_bytes;
    size_t destination_bytes;
} MalformedBlock;

static bool malformed_blocks(void) {
    static const uint8_t literal_extension[] = {0xf0};
    static const uint8_t literal_continuation[] = {0xf0, 0xff};
    static const uint8_t short_literal[] = {0x50, 'a', 'b', 'c', 'd'};
    static const uint8_t short_offset[] = {0x13, 'A', 1};
    static const uint8_t zero_offset[] = {0x13, 'A', 0, 0, 0x50, 'b', 'c', 'd', 'e', 'f'};
    static const uint8_t before_history[] = {0x13, 'A', 2, 0, 0x50, 'b', 'c', 'd', 'e', 'f'};
    static const uint8_t empty_history[] = {0x03, 1, 0, 0x50, 'b', 'c', 'd', 'e', 'f'};
    static const uint8_t match_extension[] = {0x1f, 'A', 1, 0};
    static const uint8_t match_continuation[] = {0x1f, 'A', 1, 0, 0xff};
    static const uint8_t oversized_literal[] = {0xf0, 0xff, 0};
    static const uint8_t oversized_match[] = {0x1f, 'A', 1, 0, 0xff, 0};
    static const uint8_t trailing_byte[] = {0x50, 'a', 'b', 'c', 'd', 'e', 0};
    static const MalformedBlock cases[] = {
        {"absent token", NULL, 0, 0},
        {"literal extension missing", literal_extension, sizeof(literal_extension), 15},
        {"literal continuation missing", literal_continuation, sizeof(literal_continuation), 270},
        {"short literal body", short_literal, sizeof(short_literal), 5},
        {"missing offset high byte", short_offset, sizeof(short_offset), 13},
        {"zero offset", zero_offset, sizeof(zero_offset), 13},
        {"offset before history", before_history, sizeof(before_history), 13},
        {"offset with no history", empty_history, sizeof(empty_history), 12},
        {"match extension missing", match_extension, sizeof(match_extension), 25},
        {"match continuation missing", match_continuation, sizeof(match_continuation), 280},
        {"literal exceeds output", oversized_literal, sizeof(oversized_literal), 15},
        {"match exceeds output", oversized_match, sizeof(oversized_match), 25},
        {"trailing byte", trailing_byte, sizeof(trailing_byte), 5},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const MalformedBlock* block = &cases[index];
        CHECK(expect_decode(block->name,
            EXACT_BLOCK,
            block->source,
            block->source_bytes,
            block->destination_bytes,
            -1,
            NULL));
    }
    /* Extension totals exceed a 16-bit counter before output admission. */
    encoded_fixture[0] = 0xf0;
    memset(encoded_fixture + 1, 255, 258);
    encoded_fixture[259] = 0;
    CHECK(expect_decode(
        "long literal extension sum", EXACT_BLOCK, encoded_fixture, 260, 0, -1, NULL));
    return true;
}

static bool legacy_endings(void) {
    static const uint8_t match_ending[] = {0x10, 'A', 1, 0};
    static const uint8_t empty_final[] = {0x13, 'A', 1, 0, 0};
    static const uint8_t short_final[] = {0x14, 'A', 1, 0, 0x40, 'b', 'c', 'd', 'e'};
    static const uint8_t late_match[] = {0x10, 'A', 1, 0, 0x50, 'b', 'c', 'd', 'e', 'f'};
    static const MalformedBlock cases[] = {
        {"legacy empty input", NULL, 0, 0},
        {"legacy match-ending block", match_ending, sizeof(match_ending), 5},
        {"legacy empty final sequence", empty_final, sizeof(empty_final), 8},
        {"legacy short final literals", short_final, sizeof(short_final), 13},
        {"legacy late last match", late_match, sizeof(late_match), 10},
    };
    static const char* expected[] = {"", "AAAAA", "AAAAAAAA", "AAAAAAAAAbcde", "AAAAAbcdef"};
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const MalformedBlock* block = &cases[index];
        CHECK(expect_decode(block->name,
            LEGACY_BLOCK,
            block->source,
            block->source_bytes,
            block->destination_bytes,
            (int)block->destination_bytes,
            (const uint8_t*)expected[index]));
        CHECK(expect_decode(block->name,
            EXACT_BLOCK,
            block->source,
            block->source_bytes,
            block->destination_bytes,
            -1,
            NULL));
    }
    return true;
}

static bool argument_and_partial_output_contract(void) {
    static const uint8_t empty[] = {0};
    uint8_t output[16];
    memset(output, DESTINATION_CANARY, sizeof(output));
    CHECK(lz4_decompress_exact(NULL, output, 1, 16) == -1);
    CHECK(lz4_decompress_exact(empty, NULL, 1, 1) == -1);
    CHECK(lz4_decompress_exact(empty, output, -1, 16) == -1);
    CHECK(lz4_decompress_exact(empty, output, 1, -1) == -1);
    CHECK(lz4_decompress_exact(NULL, NULL, INT_MAX, 0) == -1);
    CHECK(lz4_decompress_exact(empty, NULL, 1, INT_MAX) == -1);
    CHECK(lz4_decompress_exact(NULL, NULL, 0, 0) == -1);
    CHECK(lz4_decompress_exact(empty, NULL, 1, 0) == 0);
    CHECK(lz4_decompress_safe(NULL, NULL, 0, 0) == 0);
    CHECK(lz4_decompress_safe(NULL, output, 0, 16) == 0);
    CHECK(repeated_byte(output, sizeof(output), DESTINATION_CANARY));

    /* A valid literal prefix is already written before a bad offset is read.
     * Exact-mode failure deliberately does not promise output rollback. */
    static const uint8_t bad_offset[] = {0x30, 'a', 'b', 'c', 0, 0};
    CHECK(lz4_decompress_exact(bad_offset, output, (int)sizeof(bad_offset), 16) == -1);
    CHECK(memcmp(output, "abc", 3) == 0);
    CHECK(repeated_byte(output + 3, sizeof(output) - 3, DESTINATION_CANARY));
    return true;
}

/* Filled from the admitted archive's complete physical [64,182) bytes and
 * independent decoded-metadata bytes. Both coordinates and digests are in
 * textasset-reference-alias-archive/observed-corrected/run-1/comparison.json.
 * These literals are not generated by either production decoder. */
static const uint8_t observed_compressed[] = {
    0x1e,
    0x00,
    0x01,
    0x00,
    0x51,
    0x01,
    0x00,
    0x00,
    0x51,
    0xa0,
    0x04,
    0x00,
    0x5a,
    0x40,
    0x00,
    0x00,
    0x00,
    0x05,
    0x1d,
    0x00,
    0xf4,
    0x0c,
    0x10,
    0x74,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x73,
    0x63,
    0x72,
    0x69,
    0x70,
    0x74,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x22,
    0x00,
    0x13,
    0x80,
    0x08,
    0x00,
    0x18,
    0x6c,
    0x2a,
    0x00,
    0x5a,
    0x6c,
    0x6f,
    0x63,
    0x61,
    0x6c,
    0x29,
    0x00,
    0x23,
    0x20,
    0xf0,
    0x29,
    0x00,
    0x18,
    0x34,
    0x29,
    0x00,
    0x5b,
    0x74,
    0x61,
    0x72,
    0x67,
    0x65,
    0x53,
    0x00,
    0x2f,
    0x31,
    0x30,
    0x2a,
    0x00,
    0x01,
    0x3b,
    0x68,
    0x6f,
    0x73,
    0x28,
    0x00,
    0x23,
    0x41,
    0x70,
    0x28,
    0x00,
    0x18,
    0x30,
    0x28,
    0x00,
    0xc0,
    0x6e,
    0x75,
    0x6c,
    0x6c,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
};
static const uint8_t observed_decoded[] = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01,
    0x00,
    0x00,
    0x51,
    0xa0,
    0x00,
    0x00,
    0x51,
    0xa0,
    0x00,
    0x40,
    0x00,
    0x00,
    0x00,
    0x05,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x74,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x73,
    0x63,
    0x72,
    0x69,
    0x70,
    0x74,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x6c,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x6c,
    0x6f,
    0x63,
    0x61,
    0x6c,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x20,
    0xf0,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x34,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x74,
    0x61,
    0x72,
    0x67,
    0x65,
    0x74,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x31,
    0x30,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x34,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x68,
    0x6f,
    0x73,
    0x74,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x41,
    0x70,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x30,
    0x00,
    0x00,
    0x00,
    0x04,
    0x6c,
    0x6f,
    0x61,
    0x64,
    0x69,
    0x6e,
    0x67,
    0x2d,
    0x6e,
    0x75,
    0x6c,
    0x6c,
    0x2e,
    0x61,
    0x73,
    0x73,
    0x65,
    0x74,
    0x73,
    0x00,
};

static bool observed_metadata(void) {
    CHECK(sizeof(observed_compressed) == 118);
    CHECK(sizeof(observed_decoded) == 239);
    CHECK(valid_block("complete independently observed Unity metadata",
        observed_compressed,
        sizeof(observed_compressed),
        observed_decoded,
        sizeof(observed_decoded),
        true));
    return true;
}

int main(void) {
    if (!literal_lengths() || !match_lengths_and_offsets() || !malformed_blocks() ||
        !legacy_endings() || !argument_and_partial_output_contract() || !observed_metadata()) {
        return 1;
    }
    printf("LZ4 exact block: %zu guarded decode calls passed\n", checked_calls);
    return 0;
}
