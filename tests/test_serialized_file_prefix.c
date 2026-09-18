#include "io/serialized_file_prefix.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum {
    FIXTURE_SIZE = 128,
    CANARY_SIZE = 16
};

static const uint8_t header_bytes[48] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    22,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    38,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    128,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    128,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};

static const uint8_t metadata_prefix_bytes[17] = {
    '2',
    '0',
    '2',
    '1',
    '.',
    '3',
    '.',
    '3',
    '5',
    'f',
    '1',
    0,
    0xef,
    0xcd,
    0xab,
    0x89,
    0xa6,
};

typedef struct {
    uint8_t before[CANARY_SIZE];
    SerializedFileHeaderView view;
    uint8_t after[CANARY_SIZE];
} GuardedHeader;

typedef struct {
    uint8_t before[CANARY_SIZE];
    SerializedFilePrefixView view;
    uint8_t after[CANARY_SIZE];
} GuardedPrefix;

static void make_fixture(uint8_t bytes[FIXTURE_SIZE]) {
    /* The unmodeled metadata and gap deliberately contain arbitrary bytes. */
    memset(bytes, 0xe7, FIXTURE_SIZE);
    memcpy(bytes, header_bytes, sizeof(header_bytes));
    memcpy(bytes + 48, metadata_prefix_bytes, sizeof(metadata_prefix_bytes));
}

static void store_be(uint8_t* bytes, size_t width, uint64_t value) {
    for (size_t index = width; index != 0; --index) {
        bytes[index - 1] = (uint8_t)value;
        value >>= 8U;
    }
}

static bool has_canary(const uint8_t* bytes) {
    for (size_t index = 0; index < CANARY_SIZE; ++index) {
        if (bytes[index] != 0xa5) {
            return false;
        }
    }
    return true;
}

static bool span_matches(
    SerializedFilePrefixSpan span, const uint8_t* bytes, size_t offset, size_t size) {
    return span.data == bytes + offset && span.offset == offset && span.size == size;
}

static bool header_failure(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    uint64_t max_work,
    SerializedFilePrefixStatus status,
    uint64_t work_used,
    uint64_t error_offset) {
    GuardedHeader output;
    uint8_t original[sizeof(output)];
    memset(&output, 0xa5, sizeof(output));
    memcpy(original, &output, sizeof(output));
    SerializedFilePrefixResult result =
        serialized_file_header_query(bytes, mapped_size, logical_size, max_work, &output.view);
    CHECK(result.status == status);
    CHECK(result.work_used == work_used);
    CHECK(result.error_offset == error_offset);
    CHECK(memcmp(original, &output, sizeof(output)) == 0);
    return true;
}

static bool prefix_failure(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFilePrefixLimits* limits,
    SerializedFilePrefixStatus status,
    uint64_t work_used,
    uint64_t error_offset) {
    GuardedPrefix output;
    uint8_t original[sizeof(output)];
    memset(&output, 0xa5, sizeof(output));
    memcpy(original, &output, sizeof(output));
    SerializedFilePrefixResult result =
        serialized_file_prefix_query(bytes, mapped_size, logical_size, limits, &output.view);
    CHECK(result.status == status);
    CHECK(result.work_used == work_used);
    CHECK(result.error_offset == error_offset);
    CHECK(memcmp(original, &output, sizeof(output)) == 0);
    return true;
}

static bool test_exact_spans_and_unmapped_ranges(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    GuardedHeader output;
    memset(&output, 0xa5, sizeof(output));
    SerializedFilePrefixResult result =
        serialized_file_header_query(bytes, 48, 128, 49, &output.view);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(result.work_used == 49 && result.error_offset == SERIALIZED_FILE_PREFIX_NO_OFFSET);
    CHECK(has_canary(output.before) && has_canary(output.after));
    const SerializedFileHeaderView* header = &output.view;
    CHECK(header->format_version == 22 && header->endian_selector == 0);
    CHECK(header->metadata_size == 38 && header->file_size == 128 && header->data_offset == 128);
    CHECK(span_matches(header->header_source, bytes, 0, 48));
    CHECK(span_matches(header->legacy_metadata_size_source, bytes, 0, 4));
    CHECK(span_matches(header->legacy_file_size_source, bytes, 4, 4));
    CHECK(span_matches(header->format_version_source, bytes, 8, 4));
    CHECK(span_matches(header->legacy_data_offset_source, bytes, 12, 4));
    CHECK(span_matches(header->metadata_size_source, bytes, 16, 8));
    CHECK(span_matches(header->file_size_source, bytes, 24, 8));
    CHECK(span_matches(header->data_offset_source, bytes, 32, 8));
    CHECK(span_matches(header->endian_selector_source, bytes, 40, 1));
    CHECK(span_matches(header->opaque_source, bytes, 41, 7));
    CHECK(header->metadata.offset == 48 && header->metadata.size == 38);
    CHECK(header->metadata_to_data_gap.offset == 86 && header->metadata_to_data_gap.size == 42);
    CHECK(header->data.offset == 128 && header->data.size == 0);

    GuardedPrefix prefix;
    memset(&prefix, 0xa5, sizeof(prefix));
    SerializedFilePrefixLimits limits = {12, 66};
    result = serialized_file_prefix_query(bytes, 65, 128, &limits, &prefix.view);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(result.work_used == 66 && result.error_offset == SERIALIZED_FILE_PREFIX_NO_OFFSET);
    CHECK(has_canary(prefix.before) && has_canary(prefix.after));
    CHECK(span_matches(prefix.view.metadata_prefix_source, bytes, 48, 17));
    CHECK(span_matches(prefix.view.version_source, bytes, 48, 11));
    CHECK(span_matches(prefix.view.version_terminator_source, bytes, 59, 1));
    CHECK(span_matches(prefix.view.target_platform_source, bytes, 60, 4));
    CHECK(span_matches(prefix.view.type_tree_source, bytes, 64, 1));
    CHECK(memcmp(prefix.view.version_source.data, "2021.3.35f1", 11) == 0);
    CHECK(prefix.view.version_terminator_source.data[0] == 0);
    CHECK(prefix.view.target_platform == UINT32_C(0x89abcdef));
    CHECK(prefix.view.type_tree_enabled_raw == 0xa6);
    CHECK(prefix.view.remaining_metadata.offset == 65 && prefix.view.remaining_metadata.size == 21);
    return true;
}

static bool test_fixed_header_truncations_and_arguments(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    SerializedFilePrefixLimits limits = {100, 1000};
    for (size_t size = 0; size < 48; ++size) {
        CHECK(header_failure(
            bytes, size, 128, 1000, SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, 0, size));
        CHECK(prefix_failure(
            bytes, size, 128, &limits, SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, 0, size));
        CHECK(header_failure(
            bytes, size, size, 1000, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 0, size));
    }
    CHECK(header_failure(NULL, 0, 128, 1000, SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, 0, 0));
    CHECK(header_failure(NULL,
        1,
        128,
        1000,
        SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT,
        0,
        SERIALIZED_FILE_PREFIX_NO_OFFSET));
    CHECK(header_failure(bytes,
        128,
        127,
        1000,
        SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT,
        0,
        SERIALIZED_FILE_PREFIX_NO_OFFSET));
    CHECK(prefix_failure(bytes,
        128,
        128,
        NULL,
        SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT,
        0,
        SERIALIZED_FILE_PREFIX_NO_OFFSET));
    SerializedFilePrefixResult result = serialized_file_header_query(bytes, 48, 128, 1000, NULL);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT && result.work_used == 0);
    result = serialized_file_prefix_query(bytes, 65, 128, &limits, NULL);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT && result.work_used == 0);

    union {
        SerializedFilePrefixView view;
        SerializedFilePrefixLimits limits;
        uint8_t bytes[sizeof(SerializedFilePrefixView)];
    } overlap;

    uint8_t original[sizeof(overlap)];
    memset(&overlap, 0xa5, sizeof(overlap));
    memcpy(overlap.bytes, bytes, sizeof(bytes));
    memcpy(original, &overlap, sizeof(overlap));
    result = serialized_file_prefix_query(overlap.bytes, 128, 128, &limits, &overlap.view);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT && result.work_used == 0);
    CHECK(memcmp(original, &overlap, sizeof(overlap)) == 0);
    result = serialized_file_header_query(overlap.bytes, 128, 128, 1000, &overlap.view.header);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT && result.work_used == 0);
    CHECK(memcmp(original, &overlap, sizeof(overlap)) == 0);
    overlap.limits = limits;
    memcpy(original, &overlap, sizeof(overlap));
    result = serialized_file_prefix_query(bytes, 128, 128, &overlap.limits, &overlap.view);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT && result.work_used == 0);
    CHECK(memcmp(original, &overlap, sizeof(overlap)) == 0);
    return true;
}

static bool test_format_selector_and_opaque_bytes(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    const uint32_t rejected_versions[] = {0, 21, 23, UINT32_MAX};
    for (size_t index = 0; index < sizeof(rejected_versions) / sizeof(rejected_versions[0]);
        ++index) {
        store_be(bytes + 8, 4, rejected_versions[index]);
        CHECK(
            header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_UNSUPPORTED_VERSION, 48, 8));
    }
    make_fixture(bytes);
    const uint8_t rejected_selectors[] = {2, 255};
    for (size_t index = 0; index < sizeof(rejected_selectors); ++index) {
        bytes[40] = rejected_selectors[index];
        CHECK(
            header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_UNSUPPORTED_ENDIAN, 48, 40));
    }
    const size_t raw_offsets[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 41, 42, 43, 44, 45, 46, 47};
    for (size_t index = 0; index < sizeof(raw_offsets) / sizeof(raw_offsets[0]); ++index) {
        make_fixture(bytes);
        bytes[raw_offsets[index]] = 0xd3;
        SerializedFileHeaderView header;
        SerializedFilePrefixResult result =
            serialized_file_header_query(bytes, 48, 128, 49, &header);
        CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
        CHECK(header.header_source.data[raw_offsets[index]] == 0xd3);
        CHECK(header.metadata_size == 38 && header.endian_selector == 0);
    }

    make_fixture(bytes);
    bytes[40] = 1;
    bytes[60] = 0x89;
    bytes[61] = 0xab;
    bytes[62] = 0xcd;
    bytes[63] = 0xef;
    SerializedFilePrefixLimits limits = {12, 66};
    SerializedFilePrefixView prefix;
    SerializedFilePrefixResult result =
        serialized_file_prefix_query(bytes, 65, 128, &limits, &prefix);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(prefix.header.endian_selector == 1 && prefix.target_platform == UINT32_C(0x89abcdef));
    CHECK(memcmp(prefix.target_platform_source.data, "\x89\xab\xcd\xef", 4) == 0);

    /* Header-work failure precedes even an unsupported format; format then
     * precedes selector, which precedes a contradictory length premise. */
    bytes[11] = 23;
    bytes[40] = 2;
    bytes[31] = 127;
    CHECK(header_failure(bytes, 48, 128, 47, SERIALIZED_FILE_PREFIX_WORK_LIMIT, 0, 0));
    CHECK(header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_UNSUPPORTED_VERSION, 48, 8));
    bytes[11] = 22;
    CHECK(header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_UNSUPPORTED_ENDIAN, 48, 40));
    return true;
}

static bool test_layout_and_high_words(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    store_be(bytes + 24, 8, 127);
    CHECK(header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 24));
    make_fixture(bytes);
    store_be(bytes + 32, 8, 129);
    CHECK(header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 32));
    store_be(bytes + 32, 8, 85);
    CHECK(header_failure(bytes, 48, 128, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 16));
    store_be(bytes + 32, 8, 86);
    SerializedFileHeaderView header;
    SerializedFilePrefixResult result = serialized_file_header_query(bytes, 48, 128, 49, &header);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(header.metadata_to_data_gap.offset == 86 && header.metadata_to_data_gap.size == 0);
    CHECK(header.data.offset == 86 && header.data.size == 42);

    /* A nonzero byte16 is a metadata high byte, even when it is not a valid
     * endian selector. The backing contains only the tiny mapped prefix. */
    uint64_t large_metadata = UINT64_C(0x0200000000000000);
    store_be(bytes + 16, 8, large_metadata);
    store_be(bytes + 24, 8, large_metadata + 48);
    store_be(bytes + 32, 8, large_metadata + 48);
    result = serialized_file_header_query(bytes, 48, large_metadata + 48, 49, &header);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK && bytes[16] == 2);
    CHECK(header.metadata_size == large_metadata && header.endian_selector == 0);
    SerializedFilePrefixLimits limits = {12, 66};
    SerializedFilePrefixView prefix;
    result = serialized_file_prefix_query(bytes, 65, large_metadata + 48, &limits, &prefix);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(prefix.remaining_metadata.offset == 65 &&
        prefix.remaining_metadata.size == large_metadata - 17);

    store_be(bytes + 16, 8, UINT64_MAX - 48);
    store_be(bytes + 24, 8, UINT64_MAX);
    store_be(bytes + 32, 8, UINT64_MAX);
    result = serialized_file_header_query(bytes, 48, UINT64_MAX, 49, &header);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK);
    CHECK(
        header.metadata_to_data_gap.offset == UINT64_MAX && header.metadata_to_data_gap.size == 0);
    CHECK(header.data.offset == UINT64_MAX && header.data.size == 0);
    store_be(bytes + 16, 8, UINT64_MAX - 47);
    CHECK(
        header_failure(bytes, 48, UINT64_MAX, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 16));
    store_be(bytes + 16, 8, UINT64_MAX);
    CHECK(
        header_failure(bytes, 48, UINT64_MAX, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 16));

    make_fixture(bytes);
    store_be(bytes + 16, 8, 0);
    store_be(bytes + 24, 8, 48);
    store_be(bytes + 32, 8, 48);
    result = serialized_file_header_query(bytes, 48, 48, 49, &header);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK && header.metadata.size == 0);
    CHECK(
        prefix_failure(bytes, 48, 48, &limits, SERIALIZED_FILE_PREFIX_MALFORMED_METADATA, 48, 48));
    store_be(bytes + 32, 8, 47);
    CHECK(header_failure(bytes, 48, 48, 49, SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, 48, 16));
    return true;
}

static bool test_metadata_and_mapping_truncations(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    SerializedFilePrefixLimits limits = {100, 1000};
    for (size_t mapped_size = 48; mapped_size < 65; ++mapped_size) {
        uint64_t work = mapped_size;
        if (mapped_size >= 60 && mapped_size < 64) {
            work = 60;
        }
        CHECK(prefix_failure(bytes,
            mapped_size,
            128,
            &limits,
            SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING,
            work,
            mapped_size));
    }
    for (uint64_t metadata_size = 0; metadata_size < 17; ++metadata_size) {
        store_be(bytes + 16, 8, metadata_size);
        uint64_t work = 48 + metadata_size;
        if (metadata_size >= 12 && metadata_size < 16) {
            work = 60;
        }
        CHECK(prefix_failure(bytes,
            128,
            128,
            &limits,
            SERIALIZED_FILE_PREFIX_MALFORMED_METADATA,
            work,
            48 + metadata_size));
    }
    store_be(bytes + 16, 8, 17);
    SerializedFilePrefixView prefix;
    SerializedFilePrefixResult result =
        serialized_file_prefix_query(bytes, 65, 128, &limits, &prefix);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK && prefix.remaining_metadata.size == 0);

    make_fixture(bytes);
    memset(bytes + 48, 0x7f, 38);
    CHECK(prefix_failure(
        bytes, 128, 128, &limits, SERIALIZED_FILE_PREFIX_MALFORMED_METADATA, 86, 86));
    limits.max_version_bytes = 38;
    CHECK(prefix_failure(
        bytes, 128, 128, &limits, SERIALIZED_FILE_PREFIX_MALFORMED_METADATA, 86, 86));
    limits.max_version_bytes = 3;
    CHECK(
        prefix_failure(bytes, 51, 128, &limits, SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, 51, 51));
    CHECK(prefix_failure(bytes, 52, 128, &limits, SERIALIZED_FILE_PREFIX_VERSION_LIMIT, 51, 51));
    return true;
}

static bool test_limits_and_empty_or_arbitrary_version(void) {
    uint8_t bytes[FIXTURE_SIZE];
    make_fixture(bytes);
    for (uint64_t maximum = 0; maximum < 49; ++maximum) {
        uint64_t used = maximum < 48 ? 0 : 48;
        uint64_t offset = maximum < 48 ? 0 : SERIALIZED_FILE_PREFIX_NO_OFFSET;
        CHECK(header_failure(
            bytes, 48, 128, maximum, SERIALIZED_FILE_PREFIX_WORK_LIMIT, used, offset));
    }
    for (uint64_t maximum = 0; maximum < 66; ++maximum) {
        SerializedFilePrefixLimits limits = {12, maximum};
        uint64_t used = 0;
        uint64_t offset = 0;
        if (maximum >= 48 && maximum < 60) {
            used = maximum;
            offset = maximum;
        } else if (maximum >= 60 && maximum < 64) {
            used = 60;
            offset = 60;
        } else if (maximum >= 64) {
            used = maximum;
            offset = maximum == 64 ? 64 : SERIALIZED_FILE_PREFIX_NO_OFFSET;
        }
        CHECK(prefix_failure(
            bytes, 65, 128, &limits, SERIALIZED_FILE_PREFIX_WORK_LIMIT, used, offset));
    }
    for (size_t maximum = 0; maximum < 12; ++maximum) {
        SerializedFilePrefixLimits limits = {maximum, 1000};
        CHECK(prefix_failure(bytes,
            65,
            128,
            &limits,
            SERIALIZED_FILE_PREFIX_VERSION_LIMIT,
            48 + maximum,
            48 + maximum));
    }

    static const uint8_t empty_version_prefix[] = {0, 0xff, 0xff, 0xff, 0xff, 255};
    memcpy(bytes + 48, empty_version_prefix, sizeof(empty_version_prefix));
    store_be(bytes + 16, 8, 6);
    SerializedFilePrefixLimits limits = {1, 55};
    SerializedFilePrefixView prefix;
    SerializedFilePrefixResult result =
        serialized_file_prefix_query(bytes, 54, 128, &limits, &prefix);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK && result.work_used == 55);
    CHECK(span_matches(prefix.version_source, bytes, 48, 0));
    CHECK(span_matches(prefix.version_terminator_source, bytes, 48, 1));
    CHECK(prefix.target_platform == UINT32_MAX && prefix.type_tree_enabled_raw == 255);
    CHECK(prefix.remaining_metadata.offset == 54 && prefix.remaining_metadata.size == 0);

    static const uint8_t arbitrary_version_prefix[] = {0xff, 0x80, 'A', 0, 1, 2, 3, 4, 2};
    memcpy(bytes + 48, arbitrary_version_prefix, sizeof(arbitrary_version_prefix));
    store_be(bytes + 16, 8, 9);
    limits.max_version_bytes = 4;
    limits.max_work = 58;
    result = serialized_file_prefix_query(bytes, 57, 128, &limits, &prefix);
    CHECK(result.status == SERIALIZED_FILE_PREFIX_OK && result.work_used == 58);
    CHECK(span_matches(prefix.version_source, bytes, 48, 3));
    CHECK(memcmp(prefix.version_source.data, arbitrary_version_prefix, 3) == 0);
    CHECK(prefix.target_platform == UINT32_C(0x04030201) && prefix.type_tree_enabled_raw == 2);
    return true;
}

#ifndef _WIN32
static bool test_guarded_mapping(void) {
    long page_size_result = sysconf(_SC_PAGESIZE);
    CHECK(page_size_result > 0);
    size_t page_size = (size_t)page_size_result;
    CHECK(page_size >= FIXTURE_SIZE && page_size <= SIZE_MAX / 2);
    uint8_t* pages =
        mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    bool passed = mprotect(pages + page_size, page_size, PROT_NONE) == 0;
    uint8_t fixture[FIXTURE_SIZE];
    make_fixture(fixture);
    SerializedFilePrefixLimits limits = {12, 66};
    for (size_t mapped_size = 0; passed && mapped_size < 65; ++mapped_size) {
        uint8_t* bytes = pages + page_size - mapped_size;
        memcpy(bytes, fixture, mapped_size);
        uint64_t work = 0;
        if (mapped_size >= 48 && mapped_size < 60) {
            work = mapped_size;
        } else if (mapped_size >= 60 && mapped_size < 64) {
            work = 60;
        } else if (mapped_size == 64) {
            work = 64;
        }
        passed = prefix_failure(bytes,
            mapped_size,
            128,
            &limits,
            SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING,
            work,
            mapped_size);
    }
    if (passed) {
        uint8_t* bytes = pages + page_size - 65;
        memcpy(bytes, fixture, 65);
        SerializedFilePrefixView prefix;
        SerializedFilePrefixResult result =
            serialized_file_prefix_query(bytes, 65, 128, &limits, &prefix);
        passed = result.status == SERIALIZED_FILE_PREFIX_OK && result.work_used == 66;
    }
    int close_result = munmap(pages, page_size * 2);
    CHECK(passed);
    CHECK(close_result == 0);
    return true;
}
#endif

int main(void) {
    if (!test_exact_spans_and_unmapped_ranges() || !test_fixed_header_truncations_and_arguments() ||
        !test_format_selector_and_opaque_bytes() || !test_layout_and_high_words() ||
        !test_metadata_and_mapping_truncations() || !test_limits_and_empty_or_arbitrary_version()) {
        return 1;
    }
#ifndef _WIN32
    if (!test_guarded_mapping()) {
        return 1;
    }
#endif
    puts("SerializedFile v22 header/prefix query tests passed");
    return 0;
}
