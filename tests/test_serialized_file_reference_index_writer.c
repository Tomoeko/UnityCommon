#include "io/serialized_file_reference_index.h"

#include "common/file_io.h"
#include "common/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum {
    FIXTURE_MAX_BYTES = 8192,
    REFERENCE_COUNT = 2
};

typedef struct WriterFile {
    size_t size;
    size_t metadata_end;
    size_t data_offset;
    size_t directory_end;
    size_t reference_count_offset;
    const char* digest;
} WriterFile;

typedef struct WriterReference {
    size_t offset;
    size_t size;
    uint32_t node_count;
    uint32_t string_bytes;
    size_t class_offset;
    size_t namespace_offset;
    size_t assembly_offset;
    const char* namespace_name;
    size_t namespace_bytes;
} WriterReference;

/* Exact writer files and original offsets come from the independent retained
 * raw-observations.json. Parent/product accessors do not supply expectations. */
static const WriterFile files[] = {
    {6528U,
        4425U,
        4432U,
        2024U,
        2098U,
        "0165c78b5ad8bbc458db995641964ae915adc61fa29e2de83709f29c9094d191"},
    {6192U,
        485U,
        4096U,
        328U,
        402U,
        "d65b64b67b5f78f34a4e1729724a0b6d94ad76a1e38cafb5fea4d61b6e789b4f"}};

static const WriterReference references[REFERENCE_COUNT] = {
    {2102U, 1209U, 29U, 177U, 3254U, 3262U, 3295U, "UnityRecoverManagedFixture.Alpha", 33U},
    {3311U, 1113U, 26U, 178U, 4368U, 4376U, 4408U, "UnityRecoverManagedFixture.Beta", 32U}};

static uint64_t wire_unsigned(const uint8_t* bytes, size_t width, bool big_endian) {
    uint64_t value = 0U;
    for (size_t index = 0U; index < width; ++index) {
        const size_t source_index = big_endian ? index : width - index - 1U;
        value = (value << 8U) | bytes[source_index];
    }
    return value;
}

static bool authenticate_file(const CommonFileBytes* file, const WriterFile* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hexadecimal[COMMON_SHA256_HEX_SIZE];
    CHECK(file->size == expected->size);
    common_sha256(file->data, file->size, digest);
    common_sha256_digest_to_hex(digest, hexadecimal);
    CHECK(strcmp(hexadecimal, expected->digest) == 0);
    CHECK(wire_unsigned(file->data + 8U, 4U, true) == 22U && file->data[40U] == 0U);
    CHECK(wire_unsigned(file->data + 16U, 8U, true) == expected->metadata_end - 48U);
    CHECK(wire_unsigned(file->data + 24U, 8U, true) == expected->size);
    CHECK(wire_unsigned(file->data + 32U, 8U, true) == expected->data_offset);
    CHECK(memcmp(file->data + 48U, "2021.3.35f1", 12U) == 0);
    CHECK(wire_unsigned(file->data + 60U, 4U, false) == 19U);
    CHECK(
        wire_unsigned(file->data + expected->reference_count_offset, 4U, false) == REFERENCE_COUNT);
    return true;
}

static bool span_matches(
    SerializedFilePrefixSpan span, const CommonFileBytes* file, size_t offset, size_t size) {
    CHECK(offset <= file->size && size <= file->size - offset);
    CHECK(span.offset == offset && span.size == size && span.data == file->data + offset);
    return true;
}

static bool prefix_matches(const SerializedFilePrefixView* prefix,
    const CommonFileBytes* file,
    const WriterFile* expected) {
    const SerializedFileHeaderView* header = &prefix->header;

    const struct SpanExpectation {
        SerializedFilePrefixSpan actual;
        size_t offset;
        size_t size;
    } spans[] = {{header->header_source, 0U, 48U},
        {header->legacy_metadata_size_source, 0U, 4U},
        {header->legacy_file_size_source, 4U, 4U},
        {header->format_version_source, 8U, 4U},
        {header->legacy_data_offset_source, 12U, 4U},
        {header->metadata_size_source, 16U, 8U},
        {header->file_size_source, 24U, 8U},
        {header->data_offset_source, 32U, 8U},
        {header->endian_selector_source, 40U, 1U},
        {header->opaque_source, 41U, 7U},
        {prefix->metadata_prefix_source, 48U, 17U},
        {prefix->version_source, 48U, 11U},
        {prefix->version_terminator_source, 59U, 1U},
        {prefix->target_platform_source, 60U, 4U},
        {prefix->type_tree_source, 64U, 1U}};

    for (size_t index = 0U; index < sizeof(spans) / sizeof(spans[0]); ++index) {
        CHECK(span_matches(spans[index].actual, file, spans[index].offset, spans[index].size));
    }
    CHECK(header->format_version == 22U && header->endian_selector == 0U &&
        header->metadata_size == expected->metadata_end - 48U &&
        header->file_size == expected->size && header->data_offset == expected->data_offset);
    CHECK(header->metadata.offset == 48U && header->metadata.size == expected->metadata_end - 48U);
    CHECK(header->metadata_to_data_gap.offset == expected->metadata_end &&
        header->metadata_to_data_gap.size == expected->data_offset - expected->metadata_end);
    CHECK(header->data.offset == expected->data_offset &&
        header->data.size == expected->size - expected->data_offset);
    CHECK(prefix->target_platform == 19U && prefix->type_tree_enabled_raw == 1U &&
        prefix->remaining_metadata.offset == 65U &&
        prefix->remaining_metadata.size == expected->metadata_end - 65U);
    return true;
}

static bool reference_matches(const SerializedFileMetadataTailReferenceTypeRow* row,
    const CommonFileBytes* file,
    size_t ordinal) {
    CHECK(row && ordinal < REFERENCE_COUNT);
    const WriterReference* expected = &references[ordinal];
    const size_t offset = expected->offset;
    CHECK(row->ordinal == ordinal && row->class_id_bits == UINT32_MAX &&
        row->script_index_bits == ordinal && row->stripped_raw == 0U && row->has_script_hash &&
        row->has_tree);
    CHECK(wire_unsigned(file->data + offset, 4U, false) == UINT32_MAX &&
        wire_unsigned(file->data + offset + 5U, 2U, false) == ordinal &&
        file->data[offset + 4U] == 0U);
    CHECK(span_matches(row->source, file, offset, expected->size));
    CHECK(span_matches(row->class_id_source, file, offset, 4U));
    CHECK(span_matches(row->stripped_source, file, offset + 4U, 1U));
    CHECK(span_matches(row->script_index_source, file, offset + 5U, 2U));
    CHECK(span_matches(row->script_hash_source, file, offset + 7U, 16U));
    CHECK(span_matches(row->type_hash_source, file, offset + 23U, 16U));
    static const uint8_t empty_hashes[32] = {0};
    CHECK(memcmp(file->data + offset + 7U, empty_hashes, sizeof(empty_hashes)) == 0);
    CHECK(span_matches(row->tree.node_count_source, file, offset + 39U, 4U));
    CHECK(span_matches(row->tree.string_count_source, file, offset + 43U, 4U));
    CHECK(row->tree.node_count == expected->node_count &&
        row->tree.string_byte_count == expected->string_bytes);
    CHECK(wire_unsigned(file->data + offset + 39U, 4U, false) == expected->node_count &&
        wire_unsigned(file->data + offset + 43U, 4U, false) == expected->string_bytes);
    const size_t nodes_size = (size_t)expected->node_count * 32U;
    CHECK(span_matches(row->tree.nodes_source, file, offset + 47U, nodes_size));
    CHECK(span_matches(
        row->tree.strings_source, file, offset + 47U + nodes_size, expected->string_bytes));
    CHECK(span_matches(row->class_name_source, file, expected->class_offset, 8U));
    CHECK(span_matches(
        row->namespace_source, file, expected->namespace_offset, expected->namespace_bytes));
    CHECK(span_matches(row->assembly_name_source, file, expected->assembly_offset, 16U));
    CHECK(memcmp(file->data + expected->class_offset, "Payload", 8U) == 0);
    CHECK(memcmp(file->data + expected->namespace_offset,
              expected->namespace_name,
              expected->namespace_bytes) == 0);
    CHECK(memcmp(file->data + expected->assembly_offset, "Assembly-CSharp", 16U) == 0);
    return true;
}

static bool view_matches(const SerializedFileReferenceIndexView* view,
    const CommonFileBytes* file,
    size_t retained_bytes) {
    const WriterFile* expected = &files[0];
    CHECK(view && view->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 &&
        view->reference_type_count == REFERENCE_COUNT && view->identity_source_bytes == 113U &&
        view->retained_bytes == retained_bytes);
    CHECK(prefix_matches(&view->prefix, file, expected));
    CHECK(span_matches(view->tail_source, file, 2024U, 2401U));
    CHECK(span_matches(view->reference_type_count_source, file, 2098U, 4U));
    CHECK(span_matches(view->reference_type_rows_source, file, 2102U, 2322U));
    return true;
}

static SerializedFileReferenceKeyPart key_part(const char* text) {
    const SerializedFileReferenceKeyPart part = {(const uint8_t*)text, strlen(text)};
    return part;
}

static bool query_matches(const SerializedFileReferenceIndex* index,
    const SerializedFileReferenceKey* key,
    size_t ordinal,
    uint64_t work) {
    const SerializedFileReferenceQueryLimits limits = {64U, 128U, 1024U};
    SerializedFileReferenceMatch match = {0};
    const SerializedFileReferenceQueryResult result =
        serialized_file_reference_index_query(index, key, &limits, &match);
    const bool found = ordinal != SIZE_MAX;
    CHECK(result.status == SERIALIZED_FILE_REFERENCE_INDEX_OK &&
        result.limit == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE &&
        result.component == SERIALIZED_FILE_REFERENCE_IDENTITY_NONE &&
        result.row_ordinal == SIZE_MAX && result.work_used == work);
    CHECK(match.kind ==
            (found ? SERIALIZED_FILE_REFERENCE_UNIQUE : SERIALIZED_FILE_REFERENCE_MISSING) &&
        match.match_count == (found ? 1U : 0U) && match.first_ordinal == ordinal &&
        match.second_ordinal == SIZE_MAX);
    return true;
}

static bool queries_match(const SerializedFileReferenceIndex* index, const CommonFileBytes* file) {
    for (size_t ordinal = 0U; ordinal < REFERENCE_COUNT; ++ordinal) {
        const WriterReference* expected = &references[ordinal];
        SerializedFileReferenceKey key = {
            key_part("Payload"), key_part(expected->namespace_name), key_part("Assembly-CSharp")};
        const uint64_t work = ordinal == 0U ? 70U : 69U;
        CHECK(query_matches(index, &key, ordinal, work));
        /* Retained physical source spans are deliberately not consulted. These
         * raw file offsets also exercise permitted read-only key aliasing. */
        key.class_name = (SerializedFileReferenceKeyPart){file->data + expected->class_offset, 7U};
        key.namespace_name = (SerializedFileReferenceKeyPart){
            file->data + expected->namespace_offset, expected->namespace_bytes - 1U};
        key.assembly_name =
            (SerializedFileReferenceKeyPart){file->data + expected->assembly_offset, 15U};
        CHECK(query_matches(index, &key, ordinal, work));
    }
    SerializedFileReferenceKey key = {
        key_part("payload"), key_part(references[0].namespace_name), key_part("Assembly-CSharp")};
    CHECK(query_matches(index, &key, SIZE_MAX, 20U));
    key.class_name = key_part("PayloadX");
    CHECK(query_matches(index, &key, SIZE_MAX, 6U));
    key.class_name = key_part("Payload");
    key.namespace_name = key_part("UnityRecoverManagedFixture.alpha");
    CHECK(query_matches(index, &key, SIZE_MAX, 54U));
    key.namespace_name = key_part(references[0].namespace_name);
    key.assembly_name = key_part("Assembly-CSharpX");
    CHECK(query_matches(index, &key, SIZE_MAX, 55U));
    const SerializedFileReferenceKey empty = {0};
    CHECK(query_matches(index, &empty, SIZE_MAX, 6U));
    return true;
}

static bool compare_index(const CommonFileBytes* file, bool has_tree) {
    const WriterFile* expected = &files[has_tree ? 0U : 1U];
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 8U, 32U, 1024U, 8192U, 128U, 8192U, 65536U, 0U, 1048576U};
    const SerializedFileMetadataTailLimits tail_limits = {
        32U, 32U, 8U, 1024U, 8192U, 8192U, 8192U, 65536U, 0U, 1048576U};
    const SerializedFileReferenceIndexLimits index_limits = {2U, 113U, 65536U, 1048576U};
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
    SerializedFileReferenceIndex index;
    serialized_file_directory_init(&directory);
    serialized_file_metadata_tail_init(&tail);
    serialized_file_reference_index_init(&index);
    bool passed = false;
    const SerializedFileDirectoryResult directory_result =
        serialized_file_directory_create(file->data,
            expected->directory_end,
            file->size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &directory_limits,
            &directory);
    if (directory_result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        fprintf(
            stderr, "Expected-valid official directory rejected: %d\n", directory_result.status);
        goto cleanup_owners;
    }
    const SerializedFileMetadataTailResult tail_result = serialized_file_metadata_tail_create(
        &directory, file->data, expected->metadata_end, file->size, &tail_limits, &tail);
    if (tail_result.status != SERIALIZED_FILE_METADATA_TAIL_OK) {
        fprintf(stderr, "Expected-valid official tail rejected: %d\n", tail_result.status);
        goto cleanup_owners;
    }
    const SerializedFileReferenceIndexResult result =
        serialized_file_reference_index_create(&tail, &index_limits, &index);
    if (!has_tree) {
        passed = result.status == SERIALIZED_FILE_REFERENCE_INDEX_IDENTITIES_UNAVAILABLE &&
            result.reference_type_count == REFERENCE_COUNT &&
            result.limit == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE &&
            result.component == SERIALIZED_FILE_REFERENCE_IDENTITY_NONE &&
            result.row_ordinal == SIZE_MAX && result.error_offset == UINT64_MAX &&
            result.work_used == 1U && result.required_retained_bytes == 0U &&
            result.peak_retained_bytes == 0U && !index.implementation &&
            !serialized_file_reference_index_view(&index) &&
            !serialized_file_reference_index_row(&index, 0U);
        goto cleanup_owners;
    }
    if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_OK ||
        result.reference_type_count != REFERENCE_COUNT ||
        result.limit != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE ||
        result.component != SERIALIZED_FILE_REFERENCE_IDENTITY_NONE ||
        result.row_ordinal != SIZE_MAX || result.error_offset != UINT64_MAX ||
        !result.required_retained_bytes ||
        result.peak_retained_bytes != result.required_retained_bytes ||
        result.work_used != result.required_retained_bytes + 7U) {
        fprintf(stderr, "Official index result differs from its complete-owner contract\n");
        goto cleanup_owners;
    }

    for (size_t phase = 0U; phase < 2U; ++phase) {
        if (!view_matches(serialized_file_reference_index_view(&index),
                file,
                result.required_retained_bytes) ||
            !queries_match(&index, file)) {
            goto cleanup_owners;
        }
        for (size_t ordinal = 0U; ordinal < REFERENCE_COUNT; ++ordinal) {
            if (!reference_matches(
                    serialized_file_reference_index_row(&index, ordinal), file, ordinal)) {
                goto cleanup_owners;
            }
        }
        if (serialized_file_reference_index_row(&index, REFERENCE_COUNT) ||
            serialized_file_reference_index_row(&index, SIZE_MAX)) {
            fprintf(stderr, "Index exposed an absent original occurrence\n");
            goto cleanup_owners;
        }
        serialized_file_directory_dispose(&directory);
        serialized_file_metadata_tail_dispose(&tail);
    }
    const SerializedFileReferenceIndexView copied_view =
        *serialized_file_reference_index_view(&index);
    SerializedFileMetadataTailReferenceTypeRow copied_rows[REFERENCE_COUNT];
    for (size_t ordinal = 0U; ordinal < REFERENCE_COUNT; ++ordinal) {
        copied_rows[ordinal] = *serialized_file_reference_index_row(&index, ordinal);
    }
    serialized_file_reference_index_dispose(&index);
    if (!view_matches(&copied_view, file, result.required_retained_bytes)) {
        goto cleanup_owners;
    }
    for (size_t ordinal = 0U; ordinal < REFERENCE_COUNT; ++ordinal) {
        if (!reference_matches(&copied_rows[ordinal], file, ordinal)) {
            goto cleanup_owners;
        }
    }
    passed = true;

cleanup_owners:
    serialized_file_reference_index_dispose(&index);
    serialized_file_metadata_tail_dispose(&tail);
    serialized_file_directory_dispose(&directory);
    CHECK(passed);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s REGISTRY_TREE REGISTRY_NO_TREE\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (size_t fixture = 0U; fixture < sizeof(files) / sizeof(files[0]); ++fixture) {
        CommonFileBytes file = {0};
        if (common_file_read_regular(argv[fixture + 1U], FIXTURE_MAX_BYTES, &file) !=
            COMMON_FILE_OK) {
            fprintf(stderr, "Could not read official registry fixture\n");
            return EXIT_FAILURE;
        }
        const bool passed = authenticate_file(&file, &files[fixture]) &&
            compare_index(&file, fixture == 0U) && authenticate_file(&file, &files[fixture]);
        common_file_bytes_dispose(&file);
        if (!passed) {
            return EXIT_FAILURE;
        }
    }
    puts(
        "Official reference index: exact identities, original rows, unavailable names and lifetimes match.");
    return fflush(stdout) == 0 && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}
