#include "common/stream.h"
#include "common/string_builder.h"
#include "common/sha256.h"
#include "io/bundle_archive.h"
#include "io/lz4_decompress.h"
#include "io/serialized_file.h"
#include "io/typetree.h"
#include <locale.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[512];
    size_t size;
} TestBundleBuilder;

static bool test_bundle_append_bytes(TestBundleBuilder* builder,
                                     const void* bytes, size_t size) {
    if (!builder || (!bytes && size != 0) ||
        size > sizeof(builder->bytes) - builder->size) {
        return false;
    }
    memcpy(builder->bytes + builder->size, bytes, size);
    builder->size += size;
    return true;
}

static bool test_bundle_append_string(TestBundleBuilder* builder,
                                      const char* value) {
    return test_bundle_append_bytes(builder, value, strlen(value) + 1);
}

static bool test_bundle_append_be16(TestBundleBuilder* builder,
                                    uint16_t value) {
    uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return test_bundle_append_bytes(builder, bytes, sizeof(bytes));
}

static bool test_bundle_append_be32(TestBundleBuilder* builder,
                                    uint32_t value) {
    uint8_t bytes[] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value,
    };
    return test_bundle_append_bytes(builder, bytes, sizeof(bytes));
}

static bool test_bundle_append_be64(TestBundleBuilder* builder,
                                    uint64_t value) {
    return test_bundle_append_be32(builder, (uint32_t)(value >> 32)) &&
           test_bundle_append_be32(builder, (uint32_t)value);
}

static bool test_bundle_append_le32(TestBundleBuilder* builder,
                                    uint32_t value) {
    uint8_t bytes[] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    return test_bundle_append_bytes(builder, bytes, sizeof(bytes));
}

static bool test_bundle_append_le16(TestBundleBuilder* builder,
                                    uint16_t value) {
    uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return test_bundle_append_bytes(builder, bytes, sizeof(bytes));
}

static bool test_bundle_append_le64(TestBundleBuilder* builder,
                                    uint64_t value) {
    return test_bundle_append_le32(builder, (uint32_t)value) &&
           test_bundle_append_le32(builder, (uint32_t)(value >> 32));
}

static bool test_bundle_align(TestBundleBuilder* builder, size_t alignment) {
    while ((builder->size % alignment) != 0) {
        uint8_t zero = 0;
        if (!test_bundle_append_bytes(builder, &zero, 1)) return false;
    }
    return true;
}

static void test_bundle_patch_be64(uint8_t* bytes, size_t offset,
                                   uint64_t value) {
    for (int i = 0; i < 8; i++) {
        bytes[offset + i] = (uint8_t)(value >> (56 - 8 * i));
    }
}

static void test_bundle_patch_be32(uint8_t* bytes, size_t offset,
                                   uint32_t value) {
    for (int i = 0; i < 4; i++) {
        bytes[offset + i] = (uint8_t)(value >> (24 - 8 * i));
    }
}

static size_t test_bundle_total_size_offset(void) {
    return strlen("UnityFS") + 1 + 4 + strlen("5.x.x") + 1 +
           strlen("2021.3.35f1") + 1;
}

static size_t test_bundle_version_offset(void) {
    return strlen("UnityFS") + 1;
}

static size_t test_bundle_generation_offset(void) {
    return test_bundle_version_offset() + 4;
}

static size_t test_bundle_engine_version_offset(void) {
    return test_bundle_generation_offset() + strlen("5.x.x") + 1;
}

static size_t test_bundle_flags_offset(void) {
    return test_bundle_total_size_offset() + 8 + 4 + 4;
}

static bool build_test_unityfs_block(
    TestBundleBuilder* output, const uint8_t* block_data,
    size_t block_data_size, uint32_t declared_compressed_size,
    uint32_t declared_decompressed_size, uint16_t block_flags,
    bool block_info_at_end, bool padding_at_data_start) {
    TestBundleBuilder info;
    memset(&info, 0, sizeof(info));
    uint8_t hash[16] = {0};
    if (!test_bundle_append_bytes(&info, hash, sizeof(hash)) ||
        !test_bundle_append_be32(&info, 1) ||
        !test_bundle_append_be32(&info, declared_decompressed_size) ||
        !test_bundle_append_be32(&info, declared_compressed_size) ||
        !test_bundle_append_be16(&info, block_flags) ||
        !test_bundle_append_be32(&info, 1) ||
        !test_bundle_append_be64(&info, 0) ||
        !test_bundle_append_be64(&info, declared_decompressed_size) ||
        !test_bundle_append_be32(&info, 0) ||
        !test_bundle_append_string(&info, "file")) {
        return false;
    }

    memset(output, 0, sizeof(*output));
    if (!test_bundle_append_string(output, "UnityFS") ||
        !test_bundle_append_be32(output, 8) ||
        !test_bundle_append_string(output, "5.x.x") ||
        !test_bundle_append_string(output, "2021.3.35f1")) {
        return false;
    }
    size_t total_size_offset = test_bundle_total_size_offset();
    if (total_size_offset != output->size) return false;
    uint32_t flags = 0x40u |
                     (block_info_at_end ? 0x80u : 0u) |
                     (padding_at_data_start ? 0x200u : 0u);
    if (!test_bundle_append_be64(output, 0) ||
        !test_bundle_append_be32(output, (uint32_t)info.size) ||
        !test_bundle_append_be32(output, (uint32_t)info.size) ||
        !test_bundle_append_be32(output, flags) ||
        !test_bundle_align(output, 16)) {
        return false;
    }

    if (block_info_at_end) {
        if (padding_at_data_start && !test_bundle_align(output, 16)) {
            return false;
        }
        if (!test_bundle_append_bytes(output, block_data, block_data_size) ||
            !test_bundle_append_bytes(output, info.bytes, info.size)) {
            return false;
        }
    } else {
        if (!test_bundle_append_bytes(output, info.bytes, info.size) ||
            (padding_at_data_start && !test_bundle_align(output, 16)) ||
            !test_bundle_append_bytes(output, block_data, block_data_size)) {
            return false;
        }
    }
    test_bundle_patch_be64(output->bytes, total_size_offset, output->size);
    return true;
}

static bool build_test_unityfs(TestBundleBuilder* output,
                               bool block_info_at_end,
                               bool padding_at_data_start) {
    static const uint8_t payload[] = {'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
    return build_test_unityfs_block(
        output, payload, sizeof(payload), sizeof(payload), sizeof(payload),
        0x40u, block_info_at_end, padding_at_data_start);
}

static int expect_unityfs_rejected(const TestBundleBuilder* bytes) {
    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    BundleArchive archive;
    CHECK(!bundle_open(&archive, bytes->bytes, bytes->size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_unityfs_layout_flags(void) {
    for (int at_end = 0; at_end < 2; at_end++) {
        for (int padded = 0; padded < 2; padded++) {
            TestBundleBuilder bytes;
            CHECK(build_test_unityfs(&bytes, at_end != 0, padded != 0));
            size_t allocation_count = g_allocations_count;
            size_t allocated_bytes = g_allocated_bytes;
            BundleArchive archive;
            CHECK(bundle_open(&archive, bytes.bytes, bytes.size));
            size_t payload_size = 0;
            const uint8_t* payload =
                bundle_get_file(&archive, "file", &payload_size);
            CHECK(payload != NULL && payload_size == 7);
            CHECK(memcmp(payload, "PAYLOAD", payload_size) == 0);
            bundle_close(&archive);
            CHECK(g_allocations_count == allocation_count);
            CHECK(g_allocated_bytes == allocated_bytes);
        }
    }
    return 0;
}

static int test_unityfs_rejects_unsupported_dialect(void) {
    TestBundleBuilder bytes;

    static const uint32_t unsupported_versions[] = {7u, 9u, UINT32_MAX};
    for (size_t i = 0;
         i < sizeof(unsupported_versions) / sizeof(unsupported_versions[0]);
         ++i) {
        CHECK(build_test_unityfs(&bytes, false, false));
        test_bundle_patch_be32(bytes.bytes, test_bundle_version_offset(),
                               unsupported_versions[i]);
        CHECK(expect_unityfs_rejected(&bytes) == 0);
    }

    CHECK(build_test_unityfs(&bytes, false, false));
    bytes.bytes[test_bundle_generation_offset()] = '6';
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    CHECK(build_test_unityfs(&bytes, false, false));
    bytes.bytes[test_bundle_engine_version_offset() +
                strlen("2021.3.35")] = 'p';
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    /* This parser consumes a combined block/directory table.  Treating an
     * archive that does not promise that layout as equivalent is fail-open. */
    CHECK(build_test_unityfs(&bytes, false, false));
    test_bundle_patch_be32(bytes.bytes, test_bundle_flags_offset(), 0u);
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    CHECK(build_test_unityfs(&bytes, true, false));
    test_bundle_patch_be32(bytes.bytes, test_bundle_flags_offset(), 0x80u);
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    /* Compression values 4..63 do not have an implemented, verified codec. */
    for (uint32_t compression = 4u; compression <= 63u; ++compression) {
        CHECK(build_test_unityfs(&bytes, false, false));
        test_bundle_patch_be32(bytes.bytes, test_bundle_flags_offset(),
                               0x40u | compression);
        CHECK(expect_unityfs_rejected(&bytes) == 0);
    }

    /* Bits outside compression/combined/at-end/padding are either unknown or
     * have unsupported semantics (old-web compatibility/encryption). */
    for (unsigned bit = 8; bit < 32; ++bit) {
        if (bit == 9) continue;
        CHECK(build_test_unityfs(&bytes, false, false));
        test_bundle_patch_be32(bytes.bytes, test_bundle_flags_offset(),
                               0x40u | (UINT32_C(1) << bit));
        CHECK(expect_unityfs_rejected(&bytes) == 0);
    }
    return 0;
}

static int test_unityfs_rejects_unsupported_block_flags(void) {
    static const uint8_t payload[] = {'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
    TestBundleBuilder bytes;

    for (uint16_t compression = 4u; compression <= 63u; ++compression) {
        CHECK(build_test_unityfs_block(
            &bytes, payload, sizeof(payload), sizeof(payload),
            sizeof(payload), compression, false, false));
        CHECK(expect_unityfs_rejected(&bytes) == 0);
    }

    /* 0x40 is the sole supported block-level non-compression flag. */
    for (unsigned bit = 7; bit < 16; ++bit) {
        CHECK(build_test_unityfs_block(
            &bytes, payload, sizeof(payload), sizeof(payload),
            sizeof(payload), (uint16_t)(0x40u | (1u << bit)), false,
            false));
        CHECK(expect_unityfs_rejected(&bytes) == 0);
    }
    return 0;
}

static int test_unityfs_rejects_every_truncation(void) {
    TestBundleBuilder bytes;
    CHECK(build_test_unityfs(&bytes, false, true));

    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    for (size_t truncated_size = 0; truncated_size < bytes.size;
         ++truncated_size) {
        BundleArchive archive;
        CHECK(!bundle_open(&archive, bytes.bytes, truncated_size));
        CHECK(g_allocations_count == allocation_count);
        CHECK(g_allocated_bytes == allocated_bytes);
    }
    return 0;
}

static int test_unityfs_rejects_invalid_block_coverage(void) {
    TestBundleBuilder bytes;
    CHECK(build_test_unityfs(&bytes, false, false));

    /* The input buffer is one complete archive; undeclared trailing bytes
     * must not be silently ignored even when the declared block region is
     * otherwise internally consistent. */
    uint8_t trailing = 0;
    CHECK(test_bundle_append_bytes(&bytes, &trailing, sizeof(trailing)));
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    CHECK(build_test_unityfs(&bytes, false, false));
    /* A declared size shorter than the supplied archive is likewise invalid. */
    test_bundle_patch_be64(bytes.bytes, test_bundle_total_size_offset(),
                           bytes.size - 1);
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    /* Conversely, a declared trailing byte is not an unnamed block region. */
    CHECK(build_test_unityfs(&bytes, false, false));
    CHECK(test_bundle_append_bytes(&bytes, &trailing, sizeof(trailing)));
    test_bundle_patch_be64(bytes.bytes, test_bundle_total_size_offset(),
                           bytes.size);
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    /* At-end block data must stop before, rather than overlap, BlockInfo. */
    static const uint8_t payload[] = {'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
    CHECK(build_test_unityfs_block(&bytes, payload, sizeof(payload),
                                   sizeof(payload) + 1,
                                   sizeof(payload) + 1, 0, true, false));
    CHECK(expect_unityfs_rejected(&bytes) == 0);

    /* All directory extents are validated by bundle_open(), before any
     * caller can consume an earlier member from a partially valid table. */
    CHECK(build_test_unityfs(&bytes, false, false));
    size_t info_offset = (test_bundle_flags_offset() + 4U + 15U) &
                         ~(size_t)15U;
    size_t directory_offset_field =
        info_offset + 16U + 4U + 10U + 4U;
    test_bundle_patch_be64(bytes.bytes, directory_offset_field,
                           sizeof(payload) + 1U);
    CHECK(expect_unityfs_rejected(&bytes) == 0);
    return 0;
}

static int test_unityfs_rejects_short_lz4_output(void) {
    static const uint8_t short_lz4[] = {
        0x70, 'P', 'A', 'Y', 'L', 'O', 'A', 'D'
    };
    TestBundleBuilder bytes;
    CHECK(build_test_unityfs_block(
        &bytes, short_lz4, sizeof(short_lz4), sizeof(short_lz4), 8, 2,
        false, false));
    CHECK(expect_unityfs_rejected(&bytes) == 0);
    return 0;
}

static int test_lz4_rejects_malformed_sequences(void) {
    uint8_t output[8] = {0};
    static const uint8_t truncated_literal_extension[] = {0xf0};
    static const uint8_t truncated_long_literal_extension[] = {0xf0, 0xff};
    static const uint8_t missing_offset_byte[] = {0x00, 0x01};
    static const uint8_t zero_offset[] = {0x00, 0x00, 0x00};
    static const uint8_t offset_before_output[] = {0x00, 0xff, 0xff};
    static const uint8_t truncated_match_extension[] = {
        0x1f, 'A', 0x01, 0x00
    };
    static const uint8_t overlapping_match[] = {
        0x10, 'A', 0x01, 0x00
    };

    CHECK(lz4_decompress_safe(NULL, NULL, -1, 0) == -1);
    CHECK(lz4_decompress_safe(NULL, NULL, 0, -1) == -1);
    CHECK(lz4_decompress_safe(truncated_literal_extension, output,
                             (int)sizeof(truncated_literal_extension),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(truncated_long_literal_extension, output,
                             (int)sizeof(truncated_long_literal_extension),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(missing_offset_byte, output,
                             (int)sizeof(missing_offset_byte),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(zero_offset, output,
                             (int)sizeof(zero_offset),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(offset_before_output, output,
                             (int)sizeof(offset_before_output),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(truncated_match_extension, output,
                             (int)sizeof(truncated_match_extension),
                             (int)sizeof(output)) == -1);
    CHECK(lz4_decompress_safe(overlapping_match, output,
                             (int)sizeof(overlapping_match), 5) == 5);
    CHECK(memcmp(output, "AAAAA", 5) == 0);
    return 0;
}

static int test_unityfs_lzma_requires_full_input(void) {
    /* Raw LZMA1 stream for "PAYLOAD", prefixed by Unity's five properties. */
    static const uint8_t valid_lzma[] = {
        0x5d, 0x00, 0x00, 0x80, 0x00,
        0x00, 0x28, 0x10, 0x47, 0x78, 0x3c, 0x78, 0x8e, 0x1b,
        0x28, 0x1b, 0x15, 0xff, 0xfd, 0xdf, 0x20, 0x00,
    };
    TestBundleBuilder bytes;
    CHECK(build_test_unityfs_block(
        &bytes, valid_lzma, sizeof(valid_lzma), sizeof(valid_lzma), 7, 1,
        false, false));
    BundleArchive archive;
    CHECK(bundle_open(&archive, bytes.bytes, bytes.size));
    size_t payload_size = 0;
    const uint8_t* payload = bundle_get_file(&archive, "file", &payload_size);
    CHECK(payload != NULL && payload_size == 7);
    CHECK(memcmp(payload, "PAYLOAD", payload_size) == 0);
    bundle_close(&archive);

    uint8_t with_trailing_byte[sizeof(valid_lzma) + 1];
    memcpy(with_trailing_byte, valid_lzma, sizeof(valid_lzma));
    with_trailing_byte[sizeof(valid_lzma)] = 0;
    CHECK(build_test_unityfs_block(
        &bytes, with_trailing_byte, sizeof(with_trailing_byte),
        sizeof(with_trailing_byte), 7, 1, false, false));
    CHECK(expect_unityfs_rejected(&bytes) == 0);
    return 0;
}

static int test_unityfs_empty_member_view(void) {
    TestBundleBuilder bytes;
    CHECK(build_test_unityfs_block(&bytes, NULL, 0, 0, 0, 0,
                                   false, false));
    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    BundleArchive archive;
    CHECK(bundle_open(&archive, bytes.bytes, bytes.size));

    const uint8_t* member_data = (const uint8_t*)(uintptr_t)1u;
    size_t member_size = 123u;
    CHECK(bundle_get_member_view(&archive, 0U, &member_data, &member_size));
    CHECK(member_data == NULL && member_size == 0U);

    member_data = (const uint8_t*)(uintptr_t)1u;
    member_size = 123u;
    CHECK(!bundle_get_member_view(&archive, 1U, &member_data, &member_size));
    CHECK(member_data == NULL && member_size == 0U);

    member_data = (const uint8_t*)(uintptr_t)1u;
    member_size = 123u;
    CHECK(bundle_get_file_view(&archive, "file", &member_data,
                               &member_size));
    CHECK(member_data == NULL && member_size == 0);

    member_data = (const uint8_t*)(uintptr_t)1u;
    member_size = 123u;
    CHECK(!bundle_get_file_view(&archive, "missing", &member_data,
                                &member_size));
    CHECK(member_data == NULL && member_size == 0);

    member_size = 123u;
    CHECK(bundle_get_file(&archive, "file", &member_size) == NULL);
    CHECK(member_size == 0);
    member_size = 123u;
    CHECK(bundle_get_file(&archive, "missing", &member_size) == NULL);
    CHECK(member_size == 0);

    bundle_close(&archive);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_typetree_scalar_fidelity(void) {
    TypeTreeNode node;
    TypeTreeType type;
    TypeTreeValue parsed;
    ByteStream stream;
    int node_index;
    memset(&node, 0, sizeof(node));
    memset(&type, 0, sizeof(type));
    type.node_count = 1;
    type.nodes = &node;
    node.level = 0;
    node.name_str = "value";

    uint8_t sint8_bytes[] = {0xff};
    node.byte_size = 1;
    node.type_str = "SInt8";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, sint8_bytes, sizeof(sint8_bytes));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &parsed));
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    CHECK(typetree_value_get_int(&parsed, &signed_value));
    CHECK(signed_value == -1);
    CHECK(!typetree_value_get_uint(&parsed, &unsigned_value));

    uint8_t uint32_bytes[] = {0xff, 0xff, 0xff, 0xff};
    node.byte_size = 4;
    node.type_str = "UInt32";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, uint32_bytes, sizeof(uint32_bytes));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &parsed));
    CHECK(typetree_value_get_uint(&parsed, &unsigned_value));
    CHECK(unsigned_value == UINT32_MAX);
    CHECK(typetree_value_get_int(&parsed, &signed_value));
    CHECK(signed_value == UINT32_MAX);

    uint8_t type_pointer_bytes[] = {0x78, 0x56, 0x34, 0x12};
    node.byte_size = 4;
    node.type_str = "Type*";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, type_pointer_bytes, sizeof(type_pointer_bytes));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &parsed));
    CHECK(typetree_value_get_uint(&parsed, &unsigned_value));
    CHECK(unsigned_value == 0x12345678u);

    uint8_t uint64_bytes[8];
    memset(uint64_bytes, 0xff, sizeof(uint64_bytes));
    node.byte_size = 8;
    node.type_str = "UInt64";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, uint64_bytes, sizeof(uint64_bytes));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &parsed));
    CHECK(typetree_value_get_uint(&parsed, &unsigned_value));
    CHECK(unsigned_value == UINT64_MAX);
    CHECK(!typetree_value_get_int(&parsed, &signed_value));

    uint32_t float_bits = 0x3fc00000u;
    node.byte_size = 4;
    node.type_str = "float";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, (const uint8_t*)&float_bits, sizeof(float_bits));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &parsed));
    CHECK(parsed.type == VAL_TYPE_FLOAT && parsed.float_val == 1.5);

    node.byte_size = 1;
    node.meta_flags = 0x4000;
    node.type_str = "UInt8";
    memset(&parsed, 0, sizeof(parsed));
    node_index = 0;
    stream_init(&stream, sint8_bytes, sizeof(sint8_bytes));
    CHECK(!typetree_parse_value(&type, &node_index, &stream, &parsed));

    const uint8_t local_strings[] = {'a', '\0', 'b'};
    CHECK(strcmp(typetree_resolve_string(local_strings,
                                         sizeof(local_strings), 0),
                 "a") == 0);
    CHECK(typetree_resolve_string(local_strings,
                                  sizeof(local_strings), 1) == NULL);
    CHECK(typetree_resolve_string(local_strings,
                                  sizeof(local_strings), 2) == NULL);
    CHECK(strcmp(typetree_resolve_string(NULL, 0, 0x80000000u),
                 "AABB") == 0);
    CHECK(typetree_resolve_string(NULL, 0, 0x80000001u) == NULL);
    return 0;
}

static bool append_serialized_file_header(TestBundleBuilder* output) {
    static const uint8_t legacy_size_words[8] = {0};
    static const uint8_t selector_and_opaque[8] = {0};
    return test_bundle_append_bytes(output, legacy_size_words, sizeof(legacy_size_words)) &&
        test_bundle_append_be32(output, 22) && test_bundle_append_be32(output, 0) &&
        test_bundle_append_be64(output, 0) && /* metadata size */
        test_bundle_append_be64(output, 0) && /* file size */
        test_bundle_append_be64(output, 0) && /* data origin */
        test_bundle_append_bytes(output, selector_and_opaque, sizeof(selector_and_opaque));
}

static bool build_minimal_serialized_file(TestBundleBuilder* output) {
    memset(output, 0, sizeof(*output));
    if (!append_serialized_file_header(output)) {
        return false;
    }
    const size_t metadata_start = output->size;
    uint8_t type_tree_enabled = 1;
    uint8_t empty_user_information = 0;
    if (!test_bundle_append_string(output, "2021.3.35f1") ||
        !test_bundle_append_le32(output, 19) ||
        !test_bundle_append_bytes(output, &type_tree_enabled, 1) ||
        !test_bundle_append_le32(output, 0) || /* types */
        !test_bundle_append_le32(output, 0) || /* objects */
        !test_bundle_append_le32(output, 0) || /* scripts */
        !test_bundle_append_le32(output, 0) || /* externals */
        !test_bundle_append_le32(output, 0) || /* reference types */
        !test_bundle_append_bytes(output, &empty_user_information, 1)) {
        return false;
    }
    const size_t metadata_size = output->size - metadata_start;
    if (!test_bundle_align(output, 16)) return false;
    test_bundle_patch_be64(output->bytes, 16, metadata_size);
    test_bundle_patch_be64(output->bytes, 24, output->size);
    test_bundle_patch_be64(output->bytes, 32, output->size);
    return true;
}

typedef struct {
    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;
} TestSerializedObject;

static bool build_serialized_file_with_objects(
    TestBundleBuilder* output, const TestSerializedObject* objects,
    size_t object_count, size_t data_size) {
    static const uint8_t type_hash[16] = {0};
    static const uint8_t type_strings[] = {
        'i', 'n', 't', 0,
        'B', 'a', 's', 'e', 0,
    };
    uint8_t type_tree_enabled = 1;
    uint8_t empty_user_information = 0;
    uint8_t zero_data[32] = {0};
    if (!output || (!objects && object_count != 0) ||
        object_count > UINT32_MAX || data_size > sizeof(zero_data)) {
        return false;
    }

    memset(output, 0, sizeof(*output));
    if (!append_serialized_file_header(output)) {
        return false;
    }

    const size_t metadata_start = output->size;
    if (!test_bundle_append_string(output, "2021.3.35f1") ||
        !test_bundle_append_le32(output, 19) ||
        !test_bundle_append_bytes(output, &type_tree_enabled, 1) ||
        !test_bundle_append_le32(output, 1) || /* one serialized type */
        !test_bundle_append_le32(output, 48) || /* Shader ClassID */
        !test_bundle_append_bytes(output, "\0", 1) || /* not stripped */
        !test_bundle_append_le16(output, UINT16_MAX) ||
        !test_bundle_append_bytes(output, type_hash, sizeof(type_hash)) ||
        !test_bundle_append_le32(output, 1) || /* one TypeTree node */
        !test_bundle_append_le32(output, (uint32_t)sizeof(type_strings)) ||
        !test_bundle_append_le16(output, 1) || /* node version */
        !test_bundle_append_bytes(output, "\0\0", 2) || /* level/flags */
        !test_bundle_append_le32(output, 0) || /* type string offset */
        !test_bundle_append_le32(output, 4) || /* name string offset */
        !test_bundle_append_le32(output, 4) || /* int byte size */
        !test_bundle_append_le32(output, 0) || /* node index */
        !test_bundle_append_le32(output, 0) || /* meta flags */
        !test_bundle_append_le64(output, 0) || /* ref type hash */
        !test_bundle_append_bytes(output, type_strings,
                                  sizeof(type_strings)) ||
        !test_bundle_append_le32(output, 0) || /* dependencies */
        !test_bundle_append_le32(output, (uint32_t)object_count)) {
        return false;
    }

    for (size_t i = 0; i < object_count; i++) {
        if (!test_bundle_align(output, 4) ||
            !test_bundle_append_le64(output, (uint64_t)objects[i].path_id) ||
            !test_bundle_append_le64(output, objects[i].byte_offset) ||
            !test_bundle_append_le32(output, objects[i].byte_size) ||
            !test_bundle_append_le32(output, 0)) { /* type-table index */
            return false;
        }
    }
    if (!test_bundle_append_le32(output, 0) || /* scripts */
        !test_bundle_append_le32(output, 0) || /* externals */
        !test_bundle_append_le32(output, 0) || /* reference types */
        !test_bundle_append_bytes(output, &empty_user_information, 1)) {
        return false;
    }

    const size_t metadata_size = output->size - metadata_start;
    if (!test_bundle_align(output, 16)) return false;
    const size_t data_offset = output->size;
    if (!test_bundle_append_bytes(output, zero_data, data_size)) return false;
    test_bundle_patch_be64(output->bytes, 16, metadata_size);
    test_bundle_patch_be64(output->bytes, 24, output->size);
    test_bundle_patch_be64(output->bytes, 32, data_offset);
    return true;
}

static int test_serialized_file_object_table_authority(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    TestBundleBuilder bytes;
    SerializedFile file;

    const TestSerializedObject valid[] = {
        {7, 0, 4},
        {8, 4, 4},
    };
    CHECK(build_serialized_file_with_objects(
        &bytes, valid, sizeof(valid) / sizeof(valid[0]), 8));
    CHECK(serialized_file_open(&file, bytes.bytes, bytes.size));
    serialized_file_close(&file);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    const TestSerializedObject duplicate_path_ids[] = {
        {7, 0, 4},
        {7, 4, 4},
    };
    CHECK(build_serialized_file_with_objects(
        &bytes, duplicate_path_ids,
        sizeof(duplicate_path_ids) / sizeof(duplicate_path_ids[0]), 8));
    CHECK(!serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    const TestSerializedObject overlapping_ranges[] = {
        {7, 0, 8},
        {8, 4, 4},
    };
    CHECK(build_serialized_file_with_objects(
        &bytes, overlapping_ranges,
        sizeof(overlapping_ranges) / sizeof(overlapping_ranges[0]), 8));
    CHECK(!serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_serialized_file_declared_boundaries(void) {
    TestBundleBuilder bytes;
    CHECK(build_minimal_serialized_file(&bytes));
    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    SerializedFile file;
    CHECK(serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(file.file_size == bytes.size);
    CHECK(file.data_offset == bytes.size);
    serialized_file_close(&file);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    /* The declared file size is an exact authority, not an upper bound. */
    uint8_t trailing = 0;
    CHECK(test_bundle_append_bytes(&bytes, &trailing, 1));
    CHECK(!serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    CHECK(build_minimal_serialized_file(&bytes));
    /* A metadata declaration may not cross into the data section. */
    test_bundle_patch_be64(bytes.bytes, 16, bytes.size - 48 + 1);
    CHECK(!serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    CHECK(build_minimal_serialized_file(&bytes));
    /* Even within data_offset, every declared metadata byte must be consumed. */
    uint64_t metadata_size = 0;
    for (size_t index = 16; index < 24; ++index) {
        metadata_size = (metadata_size << 8U) | bytes.bytes[index];
    }
    test_bundle_patch_be64(bytes.bytes, 16, metadata_size + 1);
    CHECK(!serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_serialized_file_v22_header_layout(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    TestBundleBuilder bytes;
    SerializedFile file;

    CHECK(build_minimal_serialized_file(&bytes));
    bytes.bytes[40] = 1;
    test_bundle_patch_be32(bytes.bytes, 60, UINT32_C(0x89abcdef));
    CHECK(serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(file.big_endian && file.target_platform == UINT32_C(0x89abcdef));
    CHECK(file.metadata_size == 38 && file.type_tree_enabled);
    serialized_file_close(&file);

    /* The writer clears these fields; the reader does not require zero. */
    CHECK(build_minimal_serialized_file(&bytes));
    const size_t raw_offsets[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 41, 42, 43, 44, 45, 46, 47};
    for (size_t index = 0; index < sizeof(raw_offsets) / sizeof(raw_offsets[0]); ++index) {
        bytes.bytes[raw_offsets[index]] = 0xd3;
    }
    CHECK(serialized_file_open(&file, bytes.bytes, bytes.size));
    CHECK(!file.big_endian && file.target_platform == 19 && file.metadata_size == 38);
    serialized_file_close(&file);

    /* Byte16 is metadata high bits, not endianness. A nonzero high word is
     * inconsistent with this small complete-file backing and must reject. */
    CHECK(build_minimal_serialized_file(&bytes));
    bytes.bytes[16] = 1;
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));
    CHECK(build_minimal_serialized_file(&bytes));
    bytes.bytes[17] = 1;
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_serialized_file_rejects_noncanonical_booleans(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    TestBundleBuilder bytes;
    SerializedFile file;

    CHECK(build_minimal_serialized_file(&bytes));
    bytes.bytes[40] = 2U; /* byte-order selector admits only zero or one */
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));

    CHECK(build_minimal_serialized_file(&bytes));
    bytes.bytes[40] = 255U; /* unknown byte-order selectors remain unsupported */
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));

    CHECK(build_minimal_serialized_file(&bytes));
    const size_t type_tree_enabled_offset =
        48U + sizeof("2021.3.35f1") + sizeof(uint32_t);
    bytes.bytes[type_tree_enabled_offset] = 2U;
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));

    const TestSerializedObject object = {7, 0, 4};
    CHECK(build_serialized_file_with_objects(&bytes, &object, 1U, 4U));
    const size_t stripped_offset = type_tree_enabled_offset + 1U +
        sizeof(uint32_t) + sizeof(int32_t);
    bytes.bytes[stripped_offset] = 2U;
    CHECK(!serialized_file_open_metadata(&file, bytes.bytes, bytes.size));

    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int test_serialized_file_rejects_every_truncation(void) {
    TestBundleBuilder bytes;
    CHECK(build_minimal_serialized_file(&bytes));

    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    for (size_t truncated_size = 0; truncated_size < bytes.size;
         ++truncated_size) {
        SerializedFile file;
        CHECK(!serialized_file_open(&file, bytes.bytes, truncated_size));
        CHECK(g_allocations_count == allocation_count);
        CHECK(g_allocated_bytes == allocated_bytes);
    }
    return 0;
}

static int test_bundle_member_classification(void) {
    BundleDirectoryInfo member;
    memset(&member, 0, sizeof(member));
    member.name = (char*)"mainData";
    member.flags = UNITYFS_NODE_SERIALIZED_FILE;
    CHECK(bundle_member_classify(&member) ==
          BUNDLE_MEMBER_SERIALIZED_FILE);
    member.flags = UNITYFS_NODE_SERIALIZED_FILE | UNITYFS_NODE_DIRECTORY;
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_INVALID);
    member.flags = UNITYFS_NODE_DIRECTORY;
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_DIRECTORY);
    member.flags = UNITYFS_NODE_DELETED;
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_DELETED);
    member.flags = 0U;
    member.name = (char*)"sharedassets0.assets.resS";
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_RESOURCE);
    member.name = (char*)"sharedassets0.assets.resources";
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_RESOURCE);
    member.name = (char*)"unknown.bin";
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_INVALID);
    member.flags = 8U;
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_INVALID);
    member.name = NULL;
    CHECK(bundle_member_classify(&member) == BUNDLE_MEMBER_INVALID);
    CHECK(bundle_member_classify(NULL) == BUNDLE_MEMBER_INVALID);
    return 0;
}

static int test_string_builder_format_failures(void) {
    StringBuilder builder;
    sb_init(&builder);
    sb_appendf(&builder, "%s:%d", "formatted", 17);
    CHECK(sb_ok(&builder));
    CHECK(builder.len == strlen("formatted:17"));
    CHECK(strcmp(builder.buf, "formatted:17") == 0);
    sb_free(&builder);

    sb_init(&builder);
    sb_appendf(&builder, NULL);
    CHECK(!sb_ok(&builder));
    sb_free(&builder);

#if !defined(_WIN32)
    sb_init(&builder);
    sb_append(&builder, "prefix");
    CHECK(sb_ok(&builder));
    CHECK(builder.len == 6U);

    /* In the C locale, U+0100 is not representable as a single multibyte
     * character.  The standard formatting API reports an encoding error;
     * the builder must preserve its valid prefix and become permanently
     * failed instead of treating that result as an empty append. */
    CHECK(setlocale(LC_CTYPE, "C") != NULL);
    sb_appendf(&builder, "%lc", (wint_t)0x100);
    CHECK(!sb_ok(&builder));
    CHECK(builder.len == 6U);
    CHECK(builder.buf != NULL && strcmp(builder.buf, "prefix") == 0);
    sb_append(&builder, "must-not-append");
    CHECK(builder.len == 6U && strcmp(builder.buf, "prefix") == 0);
    sb_free(&builder);
#endif
    return 0;
}

static int test_stream_float_endianness(void) {
    static const uint8_t little_endian[] = {
        0x00, 0x00, 0x80, 0x3f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xc0,
    };
    static const uint8_t big_endian[] = {
        0x3f, 0x80, 0x00, 0x00,
        0xc0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ByteStream stream;
    float single = 0.0f;
    double wide = 0.0;

    stream_init(&stream, little_endian, sizeof(little_endian));
    CHECK(stream_read_float(&stream, &single));
    CHECK(stream_read_double(&stream, &wide));
    CHECK(single == 1.0f);
    CHECK(wide == -2.5);
    CHECK(stream_remaining(&stream) == 0U);

    stream_init(&stream, big_endian, sizeof(big_endian));
    stream_set_endian(&stream, true);
    CHECK(stream_read_float(&stream, &single));
    CHECK(stream_read_double(&stream, &wide));
    CHECK(single == 1.0f);
    CHECK(wide == -2.5);
    CHECK(stream_remaining(&stream) == 0U);
    return 0;
}

static int sha256_expect_hex(const void* data, size_t size,
                             const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char digest_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(data, size, digest);
    common_sha256_digest_to_hex(digest, digest_hex);
    CHECK(strcmp(digest_hex, expected) == 0);
    return 0;
}

static int test_sha256_vectors_and_chunking(void) {
    static const char abc[] = "abc";
    static const char multiblock[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha256_expect_hex(
              NULL, 0U,
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855") == 0);
    CHECK(sha256_expect_hex(
              abc, sizeof(abc) - 1U,
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad") == 0);
    CHECK(sha256_expect_hex(
              multiblock, sizeof(multiblock) - 1U,
              "248d6a61d20638b8e5c026930c3e6039"
              "a33ce45964ff2167f6ecedd419db06c1") == 0);

    uint8_t pattern[1025];
    for (size_t index = 0U; index < sizeof(pattern); ++index) {
        pattern[index] = (uint8_t)(index * 37U + index / 7U);
    }
    uint8_t expected[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(pattern, sizeof(pattern), expected);
    static const size_t chunk_sizes[] = {
        1U, 2U, 7U, 31U, 55U, 56U, 63U, 64U, 65U, 127U, 255U,
    };
    for (size_t chunk_index = 0U;
         chunk_index < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);
         ++chunk_index) {
        CommonSha256Context context;
        uint8_t actual[COMMON_SHA256_DIGEST_SIZE];
        common_sha256_init(&context);
        common_sha256_update(&context, NULL, 0U);
        for (size_t offset = 0U; offset < sizeof(pattern);) {
            size_t amount = chunk_sizes[chunk_index];
            if (amount > sizeof(pattern) - offset) {
                amount = sizeof(pattern) - offset;
            }
            common_sha256_update(&context, pattern + offset, amount);
            offset += amount;
        }
        common_sha256_final(&context, actual);
        CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    }

    /* CommonSha256Context remains caller-owned value state: copying a live
     * context must preserve a clonable streaming prefix on every backend. */
    CommonSha256Context prefix_context;
    CommonSha256Context cloned_context;
    uint8_t prefix_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t cloned_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256_init(&prefix_context);
    common_sha256_update(&prefix_context, pattern, 513U);
    cloned_context = prefix_context;
    common_sha256_update(
        &prefix_context, pattern + 513U, sizeof(pattern) - 513U);
    common_sha256_update(
        &cloned_context, pattern + 513U, sizeof(pattern) - 513U);
    common_sha256_final(&prefix_context, prefix_digest);
    common_sha256_final(&cloned_context, cloned_digest);
    CHECK(memcmp(prefix_digest, expected, sizeof(prefix_digest)) == 0);
    CHECK(memcmp(cloned_digest, expected, sizeof(cloned_digest)) == 0);

    /* NIST's million-'a' vector exercises repeated streaming updates without
     * requiring a large test allocation. */
    uint8_t thousand_a[1000];
    memset(thousand_a, 'a', sizeof(thousand_a));
    CommonSha256Context million_context;
    uint8_t million_digest[COMMON_SHA256_DIGEST_SIZE];
    char million_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_init(&million_context);
    for (size_t index = 0U; index < 1000U; ++index) {
        common_sha256_update(
            &million_context, thousand_a, sizeof(thousand_a));
    }
    common_sha256_final(&million_context, million_digest);
    common_sha256_digest_to_hex(million_digest, million_hex);
    CHECK(strcmp(
              million_hex,
              "cdc76e5c9914fb9281a1c7e284d73e67"
              "f1809a48a497200e046d39ccc7112cd0") == 0);
    return 0;
}

int main(void) {
    CHECK(test_unityfs_layout_flags() == 0);
    CHECK(test_unityfs_rejects_unsupported_dialect() == 0);
    CHECK(test_unityfs_rejects_unsupported_block_flags() == 0);
    CHECK(test_unityfs_rejects_every_truncation() == 0);
    CHECK(test_unityfs_rejects_invalid_block_coverage() == 0);
    CHECK(test_unityfs_rejects_short_lz4_output() == 0);
    CHECK(test_lz4_rejects_malformed_sequences() == 0);
    CHECK(test_unityfs_lzma_requires_full_input() == 0);
    CHECK(test_unityfs_empty_member_view() == 0);
    CHECK(test_typetree_scalar_fidelity() == 0);
    CHECK(test_serialized_file_declared_boundaries() == 0);
    CHECK(test_serialized_file_v22_header_layout() == 0);
    CHECK(test_serialized_file_rejects_noncanonical_booleans() == 0);
    CHECK(test_serialized_file_rejects_every_truncation() == 0);
    CHECK(test_serialized_file_object_table_authority() == 0);
    CHECK(test_bundle_member_classification() == 0);
    CHECK(test_string_builder_format_failures() == 0);
    CHECK(test_stream_float_endianness() == 0);
    CHECK(test_sha256_vectors_and_chunking() == 0);
    const uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04, 0, 'o', 'k', 0};
    ByteStream stream;
    uint32_t value;
    size_t allocation_size = 0;

    stream_init(&stream, bytes, sizeof(bytes));
    CHECK(stream_remaining(&stream) == sizeof(bytes));
    CHECK(stream_read_uint32(&stream, &value));
    CHECK(value == 0x04030201u);
    CHECK(stream_align(&stream, 4));
    CHECK(stream.position == 4);
    CHECK(stream_read_uint8(&stream, (uint8_t*)&value));
    CHECK((value & 0xffu) == 0);
    char* text = stream_read_string_alloc(&stream, &allocation_size);
    CHECK(text != NULL);
    CHECK(strcmp(text, "ok") == 0);
    CHECK(allocation_size == 3);
    mem_free(text, allocation_size);
    CHECK(stream_remaining(&stream) == 0);
    CHECK(!stream_skip(&stream, 1));
    CHECK(!stream_skip(&stream, SIZE_MAX));
    CHECK(!stream_seek(&stream, sizeof(bytes) + 1));

    stream_init(&stream, bytes, sizeof(bytes));
    stream_set_endian(&stream, true);
    CHECK(stream_read_uint32(&stream, &value));
    CHECK(value == 0x01020304u);

    StringBuilder sb;
    sb_init_with_capacity(&sb, 2);
    sb_append(&sb, "common");
    sb_append_char(&sb, ':');
    sb_appendf(&sb, "%u", 47u);
    CHECK(sb.buf != NULL);
    CHECK(strcmp(sb.buf, "common:47") == 0);
    CHECK(sb.len == 9);
    sb_clear(&sb);
    CHECK(sb.len == 0 && strcmp(sb.buf, "") == 0);
    sb_free(&sb);

    StringBuilder overflow;
    sb_init(&overflow);
    overflow.len = SIZE_MAX;
    sb_append_char(&overflow, 'x');
    CHECK(!sb_ok(&overflow));
    sb_free(&overflow);

    uint8_t zero_digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    char zero_digest_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(zero_digest, zero_digest_hex);
    CHECK(strcmp(zero_digest_hex,
                 "0000000000000000000000000000000000000000000000000000000000000000") ==
          0);

    enum { BYTE_COUNT = 4096 };
    uint8_t byte_array_data[4 + BYTE_COUNT];
    byte_array_data[0] = (uint8_t)(BYTE_COUNT & 0xff);
    byte_array_data[1] = (uint8_t)((BYTE_COUNT >> 8) & 0xff);
    byte_array_data[2] = 0;
    byte_array_data[3] = 0;
    for (int i = 0; i < BYTE_COUNT; i++) {
        byte_array_data[4 + i] = (uint8_t)(i * 37);
    }

    TypeTreeNode byte_array_nodes[4];
    memset(byte_array_nodes, 0, sizeof(byte_array_nodes));
    byte_array_nodes[0].level = 0;
    byte_array_nodes[0].byte_size = -1;
    byte_array_nodes[0].type_str = "vector";
    byte_array_nodes[0].name_str = "compressedBlob";
    byte_array_nodes[1].level = 1;
    byte_array_nodes[1].byte_size = -1;
    byte_array_nodes[1].type_str = "Array";
    byte_array_nodes[1].name_str = "Array";
    byte_array_nodes[2].level = 2;
    byte_array_nodes[2].byte_size = 4;
    byte_array_nodes[2].type_str = "int";
    byte_array_nodes[2].name_str = "size";
    byte_array_nodes[3].level = 2;
    byte_array_nodes[3].byte_size = 1;
    byte_array_nodes[3].type_str = "UInt8";
    byte_array_nodes[3].name_str = "data";

    TypeTreeType byte_array_type;
    memset(&byte_array_type, 0, sizeof(byte_array_type));
    byte_array_type.node_count = 4;
    byte_array_type.nodes = byte_array_nodes;

    size_t baseline_bytes = g_allocated_bytes;
    size_t baseline_allocations = g_allocations_count;
    TypeTreeValue legacy_array;
    memset(&legacy_array, 0, sizeof(legacy_array));
    int byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data));
    CHECK(typetree_parse_value(&byte_array_type, &byte_node_index, &stream,
                               &legacy_array));
    CHECK(byte_node_index == 4);
    CHECK(legacy_array.array_val.storage == TYPETREE_ARRAY_VALUES);
    CHECK(legacy_array.array_val.count == BYTE_COUNT);
    CHECK(g_allocated_bytes - baseline_bytes ==
          BYTE_COUNT * sizeof(TypeTreeValue));
    int64_t byte_value = -1;
    CHECK(typetree_array_get_int(&legacy_array, 13, &byte_value));
    CHECK(byte_value == byte_array_data[4 + 13]);
    uint8_t copied_bytes[17];
    CHECK(typetree_array_copy_bytes(&legacy_array, 101, copied_bytes,
                                    sizeof(copied_bytes)));
    CHECK(memcmp(copied_bytes, byte_array_data + 4 + 101,
                 sizeof(copied_bytes)) == 0);
    const uint8_t* borrowed_bytes = NULL;
    size_t borrowed_size = 0;
    CHECK(!typetree_get_byte_span(&legacy_array, &borrowed_bytes,
                                  &borrowed_size));
    typetree_free_value(&legacy_array);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    TypeTreeValue packed_array;
    memset(&packed_array, 0, sizeof(packed_array));
    byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data));
    CHECK(typetree_parse_value_ex(
        &byte_array_type, &byte_node_index, &stream, &packed_array,
        TYPETREE_PARSE_PACK_COMPRESSED_BLOB));
    CHECK(byte_node_index == 4);
    CHECK(packed_array.array_val.storage == TYPETREE_ARRAY_PACKED_BYTES);
    CHECK(packed_array.array_val.elements == NULL);
    CHECK(packed_array.array_val.count == BYTE_COUNT);
    CHECK(g_allocated_bytes - baseline_bytes == BYTE_COUNT);
    CHECK(typetree_get_byte_span(&packed_array, &borrowed_bytes,
                                 &borrowed_size));
    CHECK(borrowed_size == BYTE_COUNT);
    CHECK(memcmp(borrowed_bytes, byte_array_data + 4, BYTE_COUNT) == 0);
    CHECK(typetree_array_get_int(&packed_array, BYTE_COUNT - 1,
                                 &byte_value));
    CHECK(byte_value == byte_array_data[4 + BYTE_COUNT - 1]);
    CHECK(typetree_array_copy_bytes(&packed_array, 101, copied_bytes,
                                    sizeof(copied_bytes)));
    CHECK(memcmp(copied_bytes, byte_array_data + 4 + 101,
                 sizeof(copied_bytes)) == 0);
    CHECK(!typetree_array_copy_bytes(&packed_array, BYTE_COUNT - 1,
                                     copied_bytes, 2));
    typetree_free_value(&packed_array);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    TypeTreeValue borrowed_array;
    memset(&borrowed_array, 0, sizeof(borrowed_array));
    byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data));
    CHECK(typetree_parse_value_ex(
        &byte_array_type, &byte_node_index, &stream, &borrowed_array,
        TYPETREE_PARSE_BORROW_BYTE_ARRAYS));
    CHECK(byte_node_index == 4);
    CHECK(borrowed_array.array_val.storage ==
          TYPETREE_ARRAY_PACKED_BYTES);
    CHECK(!borrowed_array.array_val.packed_bytes_owned);
    CHECK(borrowed_array.array_val.packed_bytes == byte_array_data + 4);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    CHECK(typetree_get_byte_span(&borrowed_array, &borrowed_bytes,
                                 &borrowed_size));
    CHECK(borrowed_size == BYTE_COUNT);
    CHECK(typetree_array_copy_bytes(&borrowed_array, 101, copied_bytes,
                                    sizeof(copied_bytes)));
    typetree_free_value(&borrowed_array);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    TypeTreeValue conflicting_array;
    memset(&conflicting_array, 0, sizeof(conflicting_array));
    byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data));
    CHECK(!typetree_parse_value_ex(
        &byte_array_type, &byte_node_index, &stream, &conflicting_array,
        TYPETREE_PARSE_PACK_BYTE_ARRAYS |
        TYPETREE_PARSE_BORROW_BYTE_ARRAYS));
    CHECK(byte_node_index == 0);

    uint8_t impossible_array_count[] = {0xff, 0xff, 0xff, 0x7f};
    TypeTreeValue impossible_array;
    memset(&impossible_array, 0xa5, sizeof(impossible_array));
    byte_node_index = 0;
    stream_init(&stream, impossible_array_count,
                sizeof(impossible_array_count));
    CHECK(!typetree_parse_value(
        &byte_array_type, &byte_node_index, &stream, &impossible_array));
    CHECK(impossible_array.type == VAL_TYPE_NONE);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    byte_array_nodes[0].name_str = "m_KeywordFlags";
    TypeTreeValue second_byte_array;
    memset(&second_byte_array, 0, sizeof(second_byte_array));
    byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data));
    CHECK(typetree_parse_value_ex(
        &byte_array_type, &byte_node_index, &stream, &second_byte_array,
        TYPETREE_PARSE_PACK_BYTE_ARRAYS));
    CHECK(second_byte_array.array_val.storage ==
          TYPETREE_ARRAY_PACKED_BYTES);
    CHECK(typetree_get_byte_span(&second_byte_array, &borrowed_bytes,
                                 &borrowed_size));
    CHECK(borrowed_size == BYTE_COUNT);
    typetree_free_value(&second_byte_array);
    byte_array_nodes[0].name_str = "compressedBlob";
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    TypeTreeValue truncated_array;
    memset(&truncated_array, 0, sizeof(truncated_array));
    byte_node_index = 0;
    stream_init(&stream, byte_array_data, sizeof(byte_array_data) - 1);
    CHECK(!typetree_parse_value_ex(
        &byte_array_type, &byte_node_index, &stream, &truncated_array,
        TYPETREE_PARSE_PACK_COMPRESSED_BLOB));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    CHECK(g_allocations_count == 0);
    CHECK(g_allocated_bytes == 0);
    return 0;
}
