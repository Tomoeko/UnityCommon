#include "io/serialized_file_schema.h"

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
    WRITER_MAX_BYTES = 8192,
    WRITER_MAX_TREES = 8,
    WRITER_MAX_NODES = 128,
    WRITER_NODE_BYTES = 32,
    OFFICIAL_COMMON_BYTES = 1170
};

typedef struct WriterSpan {
    size_t offset;
    size_t size;
} WriterSpan;

typedef struct WriterTree {
    bool reference;
    size_t ordinal;
    WriterSpan source;
    uint32_t class_bits;
    uint16_t script_bits;
    uint8_t stripped;
    bool has_script_hash;
    WriterSpan script_hash;
    WriterSpan type_hash;
    WriterSpan node_count_source;
    WriterSpan string_count_source;
    WriterSpan nodes;
    WriterSpan strings;
    uint32_t node_count;
    uint32_t string_count;
    WriterSpan names[3];
    WriterSpan dependency_count_source;
    WriterSpan dependency_words;
    uint32_t dependency_count;
} WriterTree;

typedef struct WriterObservation {
    const uint8_t* bytes;
    size_t size;
    size_t metadata_end;
    size_t data_offset;
    size_t directory_end;
    bool has_tree;
    size_t ordinary_count;
    size_t reference_count;
    WriterTree trees[WRITER_MAX_TREES];
} WriterObservation;

typedef struct WriterCursor {
    const uint8_t* bytes;
    size_t offset;
    size_t end;
} WriterCursor;

typedef struct WriterFixture {
    size_t bytes;
    bool has_tree;
    size_t ordinary_count;
    size_t reference_count;
    const char* sha256;
} WriterFixture;

/* Whole-file pins precede independent metadata observation. These fixtures
 * cover every ordinary/reference tree in three controlled official writers. */
static const WriterFixture fixtures[] = {
    {4456U, true, 4U, 0U, "58955a4e1cb8c769315fab4a96483094263adaa0bd37771dd7e8468d552cf7f6"},
    {4456U, false, 4U, 0U, "de05057a639dcac017f1f816b6830f767a3977cc736aa1228533df4931f1b0c8"},
    {4336U, true, 2U, 1U, "0a2dbcb88979c18afb5331524fe5184f44af13c847279f9cefb514d6f585fea0"},
    {4336U, false, 2U, 1U, "8e462110aa97dd2979c10ea20834e16ceb4c7fa35b41210ce913e86fca942e2e"},
    {6528U, true, 1U, 2U, "0165c78b5ad8bbc458db995641964ae915adc61fa29e2de83709f29c9094d191"},
    {6192U, false, 1U, 2U, "d65b64b67b5f78f34a4e1729724a0b6d94ad76a1e38cafb5fea4d61b6e789b4f"}};

static uint64_t wire_unsigned(const uint8_t* bytes, size_t width, bool big_endian) {
    uint64_t value = 0U;
    for (size_t index = 0U; index < width; ++index) {
        const size_t source_index = big_endian ? index : width - index - 1U;
        value = (value << 8U) | bytes[source_index];
    }
    return value;
}

static bool digest_matches(const uint8_t* bytes, size_t size, const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hexadecimal[COMMON_SHA256_HEX_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hexadecimal);
    CHECK(strcmp(hexadecimal, expected) == 0);
    return true;
}

static bool take_span(WriterCursor* cursor, size_t size, WriterSpan* out_span) {
    CHECK(cursor->offset <= cursor->end && size <= cursor->end - cursor->offset);
    if (out_span) {
        out_span->offset = cursor->offset;
        out_span->size = size;
    }
    cursor->offset += size;
    return true;
}

static bool take_count(WriterCursor* cursor, size_t maximum, size_t* out_count) {
    const size_t offset = cursor->offset;
    CHECK(take_span(cursor, 4U, NULL));
    const uint64_t count = wire_unsigned(cursor->bytes + offset, 4U, false);
    CHECK(count <= maximum);
    *out_count = (size_t)count;
    return true;
}

static bool take_terminated(WriterCursor* cursor, WriterSpan* out_span) {
    CHECK(cursor->offset <= cursor->end);
    const uint8_t* start = cursor->bytes + cursor->offset;
    const uint8_t* terminator = memchr(start, 0, cursor->end - cursor->offset);
    CHECK(terminator);
    return take_span(cursor, (size_t)(terminator - start) + 1U, out_span);
}

static bool take_alignment(WriterCursor* cursor) {
    return take_span(cursor, (4U - cursor->offset % 4U) % 4U, NULL);
}

static void initialize_tree(WriterTree* tree) {
    memset(tree, 0, sizeof(*tree));
    tree->script_hash.offset = SIZE_MAX;
    tree->node_count_source.offset = SIZE_MAX;
    tree->string_count_source.offset = SIZE_MAX;
    tree->nodes.offset = SIZE_MAX;
    tree->strings.offset = SIZE_MAX;
    tree->dependency_count_source.offset = SIZE_MAX;
    tree->dependency_words.offset = SIZE_MAX;
    for (size_t index = 0U; index < 3U; ++index) {
        tree->names[index].offset = SIZE_MAX;
    }
}

static bool observe_tree(WriterCursor* cursor, bool has_tree, WriterTree* tree) {
    tree->source.offset = cursor->offset;
    const size_t prefix = cursor->offset;
    CHECK(take_span(cursor, 7U, NULL));
    tree->class_bits = (uint32_t)wire_unsigned(cursor->bytes + prefix, 4U, false);
    tree->stripped = cursor->bytes[prefix + 4U];
    tree->script_bits = (uint16_t)wire_unsigned(cursor->bytes + prefix + 5U, 2U, false);
    tree->has_script_hash = tree->class_bits == 114U || (tree->script_bits & 0x8000U) == 0U;
    if (tree->has_script_hash) {
        CHECK(take_span(cursor, 16U, &tree->script_hash));
    }
    CHECK(take_span(cursor, 16U, &tree->type_hash));
    if (!has_tree) {
        tree->source.size = cursor->offset - tree->source.offset;
        return true;
    }

    CHECK(take_span(cursor, 4U, &tree->node_count_source));
    CHECK(take_span(cursor, 4U, &tree->string_count_source));
    tree->node_count =
        (uint32_t)wire_unsigned(cursor->bytes + tree->node_count_source.offset, 4U, false);
    tree->string_count =
        (uint32_t)wire_unsigned(cursor->bytes + tree->string_count_source.offset, 4U, false);
    CHECK(tree->node_count && tree->node_count <= WRITER_MAX_NODES);
    CHECK(tree->string_count <= WRITER_MAX_BYTES);
    CHECK(take_span(cursor, (size_t)tree->node_count * WRITER_NODE_BYTES, &tree->nodes));
    CHECK(take_span(cursor, tree->string_count, &tree->strings));
    if (tree->reference) {
        for (size_t index = 0U; index < 3U; ++index) {
            CHECK(take_terminated(cursor, &tree->names[index]));
        }
    } else {
        CHECK(take_span(cursor, 4U, &tree->dependency_count_source));
        tree->dependency_count = (uint32_t)wire_unsigned(
            cursor->bytes + tree->dependency_count_source.offset, 4U, false);
        CHECK(tree->dependency_count <= WRITER_MAX_NODES);
        CHECK(take_span(cursor, (size_t)tree->dependency_count * 4U, &tree->dependency_words));
    }
    tree->source.size = cursor->offset - tree->source.offset;
    return true;
}

static bool observe_metadata(
    const CommonFileBytes* file, const WriterFixture* fixture, WriterObservation* observation) {
    memset(observation, 0, sizeof(*observation));
    observation->bytes = file->data;
    observation->size = file->size;
    CHECK(file->size == fixture->bytes && file->size >= 48U);
    CHECK(digest_matches(file->data, file->size, fixture->sha256));
    CHECK(wire_unsigned(file->data + 8U, 4U, true) == 22U && !file->data[40]);
    const uint64_t metadata_size = wire_unsigned(file->data + 16U, 8U, true);
    const uint64_t data_offset = wire_unsigned(file->data + 32U, 8U, true);
    CHECK(wire_unsigned(file->data + 24U, 8U, true) == file->size);
    CHECK(metadata_size <= file->size - 48U && data_offset <= file->size);
    observation->metadata_end = 48U + (size_t)metadata_size;
    observation->data_offset = (size_t)data_offset;
    CHECK(observation->metadata_end <= observation->data_offset);

    WriterCursor cursor = {file->data, 48U, observation->metadata_end};
    WriterSpan version;
    CHECK(take_terminated(&cursor, &version));
    CHECK(version.size == 12U && memcmp(file->data + version.offset, "2021.3.35f1", 12U) == 0);
    CHECK(take_span(&cursor, 5U, NULL));
    CHECK(wire_unsigned(file->data + 60U, 4U, false) == 19U);
    observation->has_tree = file->data[64] == 1U;
    CHECK(file->data[64] <= 1U && observation->has_tree == fixture->has_tree);
    CHECK(take_count(&cursor, WRITER_MAX_TREES, &observation->ordinary_count));
    CHECK(observation->ordinary_count == fixture->ordinary_count);
    for (size_t ordinal = 0U; ordinal < observation->ordinary_count; ++ordinal) {
        WriterTree* tree = &observation->trees[ordinal];
        initialize_tree(tree);
        tree->ordinal = ordinal;
        CHECK(observe_tree(&cursor, observation->has_tree, tree));
    }

    size_t object_count;
    CHECK(take_count(&cursor, 32U, &object_count));
    CHECK(take_alignment(&cursor));
    CHECK(take_span(&cursor, object_count * 24U, NULL));
    observation->directory_end = cursor.offset;
    size_t script_count;
    CHECK(take_count(&cursor, 32U, &script_count));
    for (size_t ordinal = 0U; ordinal < script_count; ++ordinal) {
        CHECK(take_span(&cursor, 4U, NULL));
        CHECK(take_alignment(&cursor));
        CHECK(take_span(&cursor, 8U, NULL));
    }
    size_t external_count;
    CHECK(take_count(&cursor, 32U, &external_count));
    for (size_t ordinal = 0U; ordinal < external_count; ++ordinal) {
        CHECK(take_terminated(&cursor, NULL));
        CHECK(take_span(&cursor, 20U, NULL));
        CHECK(take_terminated(&cursor, NULL));
    }
    CHECK(take_count(
        &cursor, WRITER_MAX_TREES - observation->ordinary_count, &observation->reference_count));
    CHECK(observation->reference_count == fixture->reference_count);
    for (size_t ordinal = 0U; ordinal < observation->reference_count; ++ordinal) {
        WriterTree* tree = &observation->trees[observation->ordinary_count + ordinal];
        initialize_tree(tree);
        tree->reference = true;
        tree->ordinal = ordinal;
        CHECK(observe_tree(&cursor, observation->has_tree, tree));
    }
    CHECK(take_terminated(&cursor, NULL));
    CHECK(cursor.offset == cursor.end);
    return true;
}

static bool span_matches(
    SerializedFilePrefixSpan actual, const WriterObservation* observation, WriterSpan expected) {
    if (expected.offset == SIZE_MAX) {
        CHECK(!actual.data && actual.offset == UINT64_MAX && !actual.size);
        return true;
    }
    CHECK(expected.offset <= observation->size &&
        expected.size <= observation->size - expected.offset);
    CHECK(actual.data == observation->bytes + expected.offset && actual.offset == expected.offset &&
        actual.size == expected.size);
    return true;
}

static bool fixed_span_matches(SerializedFilePrefixSpan actual,
    const WriterObservation* observation,
    size_t offset,
    size_t size) {
    const WriterSpan expected = {offset, size};
    return span_matches(actual, observation, expected);
}

static bool prefix_matches(
    const SerializedFilePrefixView* prefix, const WriterObservation* expected) {
    const SerializedFileHeaderView* header = &prefix->header;
    CHECK(fixed_span_matches(header->header_source, expected, 0U, 48U));
    CHECK(fixed_span_matches(header->legacy_metadata_size_source, expected, 0U, 4U));
    CHECK(fixed_span_matches(header->legacy_file_size_source, expected, 4U, 4U));
    CHECK(fixed_span_matches(header->format_version_source, expected, 8U, 4U));
    CHECK(fixed_span_matches(header->legacy_data_offset_source, expected, 12U, 4U));
    CHECK(fixed_span_matches(header->metadata_size_source, expected, 16U, 8U));
    CHECK(fixed_span_matches(header->file_size_source, expected, 24U, 8U));
    CHECK(fixed_span_matches(header->data_offset_source, expected, 32U, 8U));
    CHECK(fixed_span_matches(header->endian_selector_source, expected, 40U, 1U));
    CHECK(fixed_span_matches(header->opaque_source, expected, 41U, 7U));
    CHECK(header->format_version == 22U && header->metadata_size == expected->metadata_end - 48U &&
        header->file_size == expected->size && header->data_offset == expected->data_offset &&
        !header->endian_selector);
    CHECK(header->metadata.offset == 48U && header->metadata.size == expected->metadata_end - 48U);
    CHECK(header->metadata_to_data_gap.offset == expected->metadata_end &&
        header->metadata_to_data_gap.size == expected->data_offset - expected->metadata_end);
    CHECK(header->data.offset == expected->data_offset &&
        header->data.size == expected->size - expected->data_offset);
    CHECK(fixed_span_matches(prefix->metadata_prefix_source, expected, 48U, 17U));
    CHECK(fixed_span_matches(prefix->version_source, expected, 48U, 11U));
    CHECK(fixed_span_matches(prefix->version_terminator_source, expected, 59U, 1U));
    CHECK(fixed_span_matches(prefix->target_platform_source, expected, 60U, 4U));
    CHECK(fixed_span_matches(prefix->type_tree_source, expected, 64U, 1U));
    CHECK(prefix->target_platform == 19U &&
        prefix->type_tree_enabled_raw == (expected->has_tree ? 1U : 0U));
    CHECK(prefix->remaining_metadata.offset == 65U &&
        prefix->remaining_metadata.size == expected->metadata_end - 65U);
    return true;
}

static bool string_matches(const SerializedFileSchemaString* actual,
    uint32_t encoded_offset,
    const WriterTree* tree,
    const WriterObservation* observation,
    const CommonFileBytes* common) {
    const bool is_common = (encoded_offset & UINT32_C(0x80000000)) != 0U;
    const size_t offset = encoded_offset & UINT32_C(0x7fffffff);
    const uint8_t* table = is_common ? common->data : observation->bytes + tree->strings.offset;
    const size_t table_size = is_common ? common->size : tree->strings.size;
    CHECK(offset < table_size && (!offset || !table[offset - 1U]));
    const uint8_t* terminator = memchr(table + offset, 0, table_size - offset);
    CHECK(terminator);
    const size_t size = (size_t)(terminator - (table + offset));
    CHECK(actual->space ==
        (is_common ? SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35
                   : SERIALIZED_FILE_SCHEMA_STRING_LOCAL));
    CHECK(actual->encoded_offset == encoded_offset && actual->byte_count == size && actual->bytes);
    CHECK(actual->source_offset == (is_common ? offset : tree->strings.offset + offset));
    CHECK(memcmp(actual->bytes, table + offset, size + 1U) == 0);
    if (!is_common) {
        CHECK(actual->bytes == table + offset);
    }
    return true;
}

static uint8_t node_level(
    const WriterObservation* observation, const WriterTree* tree, size_t ordinal) {
    return observation->bytes[tree->nodes.offset + ordinal * WRITER_NODE_BYTES + 2U];
}

static bool node_matches(const SerializedFileSchemaNode* actual,
    size_t ordinal,
    const WriterTree* tree,
    const WriterObservation* observation,
    const CommonFileBytes* common) {
    const size_t offset = tree->nodes.offset + ordinal * WRITER_NODE_BYTES;
    const uint8_t* raw = observation->bytes + offset;
    CHECK(actual && actual->ordinal == ordinal);
    CHECK(fixed_span_matches(actual->source, observation, offset, WRITER_NODE_BYTES));
    CHECK(actual->version == wire_unsigned(raw, 2U, false) && actual->level == raw[2] &&
        actual->type_flags == raw[3]);
    CHECK(actual->type_offset_bits == wire_unsigned(raw + 4U, 4U, false));
    CHECK(actual->name_offset_bits == wire_unsigned(raw + 8U, 4U, false));
    CHECK(actual->byte_size_bits == wire_unsigned(raw + 12U, 4U, false));
    CHECK(actual->index_bits == wire_unsigned(raw + 16U, 4U, false));
    CHECK(actual->meta_flags == wire_unsigned(raw + 20U, 4U, false));
    CHECK(memcmp(actual->opaque_tail, raw + 24U, 8U) == 0);
    CHECK(string_matches(&actual->type_name,
        (uint32_t)wire_unsigned(raw + 4U, 4U, false),
        tree,
        observation,
        common));
    CHECK(string_matches(&actual->field_name,
        (uint32_t)wire_unsigned(raw + 8U, 4U, false),
        tree,
        observation,
        common));

    /* Deliberately derive each relation from bounded raw preorder scans, not
     * the production stack/link algorithm. Official trees have <=128 nodes. */
    const uint8_t level = raw[2];
    CHECK((ordinal == 0U) == (level == 0U));
    size_t parent = SIZE_MAX;
    for (size_t previous = ordinal; previous > 0U; --previous) {
        if (node_level(observation, tree, previous - 1U) < level) {
            parent = previous - 1U;
            CHECK(node_level(observation, tree, parent) + 1U == level);
            break;
        }
    }
    size_t end = ordinal + 1U;
    while (end < tree->node_count && node_level(observation, tree, end) > level) {
        ++end;
    }
    const size_t first_child = end > ordinal + 1U ? ordinal + 1U : SIZE_MAX;
    const size_t sibling =
        end < tree->node_count && node_level(observation, tree, end) == level ? end : SIZE_MAX;
    size_t child_count = 0U;
    for (size_t child = ordinal + 1U; child < end; ++child) {
        if (node_level(observation, tree, child) == level + 1U) {
            ++child_count;
        }
    }
    CHECK(actual->parent == parent && actual->first_child == first_child &&
        actual->next_sibling == sibling && actual->child_count == child_count &&
        actual->subtree_end == end);
    return true;
}

static bool schema_matches(const SerializedFileSchema* schema,
    const WriterTree* tree,
    const WriterObservation* observation,
    const CommonFileBytes* common,
    size_t retained_bytes) {
    const SerializedFileSchemaView* view = serialized_file_schema_view(schema);
    CHECK(view && view->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1);
    CHECK(view->row_kind ==
        (tree->reference ? SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE
                         : SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE));
    CHECK(view->type_ordinal == tree->ordinal && view->class_id_bits == tree->class_bits &&
        view->script_index_bits == tree->script_bits && view->stripped_raw == tree->stripped &&
        view->has_script_hash == tree->has_script_hash);
    CHECK(prefix_matches(&view->prefix, observation));
    CHECK(span_matches(view->type_entry_source, observation, tree->source));
    CHECK(span_matches(view->script_hash_source, observation, tree->script_hash));
    CHECK(span_matches(view->type_hash_source, observation, tree->type_hash));
    CHECK(span_matches(view->tree.node_count_source, observation, tree->node_count_source));
    CHECK(span_matches(view->tree.string_count_source, observation, tree->string_count_source));
    CHECK(span_matches(view->tree.nodes_source, observation, tree->nodes));
    CHECK(span_matches(view->tree.strings_source, observation, tree->strings));
    CHECK(view->tree.node_count == tree->node_count &&
        view->tree.string_byte_count == tree->string_count);
    CHECK(span_matches(view->class_name_source, observation, tree->names[0]));
    CHECK(span_matches(view->namespace_source, observation, tree->names[1]));
    CHECK(span_matches(view->assembly_name_source, observation, tree->names[2]));
    CHECK(span_matches(view->dependency_count_source, observation, tree->dependency_count_source));
    CHECK(span_matches(view->dependency_words_source, observation, tree->dependency_words));
    CHECK(view->dependency_count == tree->dependency_count &&
        view->node_count == tree->node_count && view->retained_bytes == retained_bytes);
    size_t maximum_depth = 0U;
    for (size_t ordinal = 0U; ordinal < tree->node_count; ++ordinal) {
        const uint8_t level = node_level(observation, tree, ordinal);
        if (level > maximum_depth) {
            maximum_depth = level;
        }
        CHECK(node_matches(
            serialized_file_schema_node(schema, ordinal), ordinal, tree, observation, common));
    }
    CHECK(view->maximum_depth == maximum_depth);
    CHECK(!serialized_file_schema_node(schema, tree->node_count));
    CHECK(!serialized_file_schema_node(schema, SIZE_MAX));
    return true;
}

static bool result_matches(const SerializedFileSchemaResult* result,
    const WriterObservation* observation,
    const WriterTree* tree,
    const SerializedFileSchemaLimits* limits,
    const SerializedFileSchema* schema) {
    CHECK(result->status ==
        (observation->has_tree ? SERIALIZED_FILE_SCHEMA_OK
                               : SERIALIZED_FILE_SCHEMA_NO_EMBEDDED_TREE));
    CHECK(result->limit == SERIALIZED_FILE_SCHEMA_LIMIT_NONE &&
        result->field == SERIALIZED_FILE_SCHEMA_FIELD_NONE &&
        result->error_space == SERIALIZED_FILE_SCHEMA_ERROR_NO_SOURCE &&
        result->type_ordinal == tree->ordinal && result->node_ordinal == SIZE_MAX &&
        result->error_offset == UINT64_MAX);
    CHECK(result->work_used && result->work_used <= limits->max_work);
    CHECK(result->required_retained_bytes == result->peak_retained_bytes &&
        result->required_scratch_bytes == result->peak_scratch_bytes);
    CHECK(result->peak_retained_bytes <= limits->max_retained_bytes &&
        result->peak_scratch_bytes <= limits->max_scratch_bytes);
    if (observation->has_tree) {
        CHECK(result->required_retained_bytes && serialized_file_schema_view(schema));
    } else {
        CHECK(!result->required_retained_bytes && !result->required_scratch_bytes &&
            !serialized_file_schema_view(schema) && !serialized_file_schema_node(schema, 0U));
    }
    return true;
}

static bool compare_owned_schemas(
    const WriterObservation* observation, const CommonFileBytes* common) {
    const SerializedFileDirectoryLimits directory_limits = {12U,
        WRITER_MAX_TREES,
        32U,
        1024U,
        WRITER_MAX_BYTES,
        128U,
        WRITER_MAX_BYTES,
        65536U,
        0U,
        1048576U};
    const SerializedFileMetadataTailLimits tail_limits = {32U,
        32U,
        WRITER_MAX_TREES,
        1024U,
        WRITER_MAX_BYTES,
        WRITER_MAX_BYTES,
        WRITER_MAX_BYTES,
        65536U,
        0U,
        1048576U};
    const SerializedFileSchemaLimits schema_limits = {
        WRITER_MAX_NODES, WRITER_MAX_BYTES, 255U, 1048576U, 1048576U, 16777216U};
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
    SerializedFileSchema schemas[WRITER_MAX_TREES];
    SerializedFileSchemaResult results[WRITER_MAX_TREES];
    const SerializedFileSchemaView* original_views[WRITER_MAX_TREES] = {0};
    serialized_file_directory_init(&directory);
    serialized_file_metadata_tail_init(&tail);
    for (size_t index = 0U; index < WRITER_MAX_TREES; ++index) {
        serialized_file_schema_init(&schemas[index]);
    }
    bool passed = false;
    const SerializedFileDirectoryResult directory_result =
        serialized_file_directory_create(observation->bytes,
            observation->directory_end,
            observation->size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &directory_limits,
            &directory);
    if (directory_result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        fprintf(
            stderr, "Expected-valid official directory rejected: %d\n", directory_result.status);
        goto cleanup_owners;
    }
    const SerializedFileMetadataTailResult tail_result =
        serialized_file_metadata_tail_create(&directory,
            observation->bytes,
            observation->metadata_end,
            observation->size,
            &tail_limits,
            &tail);
    if (tail_result.status != SERIALIZED_FILE_METADATA_TAIL_OK) {
        fprintf(stderr, "Expected-valid official tail rejected: %d\n", tail_result.status);
        goto cleanup_owners;
    }

    const size_t count = observation->ordinary_count + observation->reference_count;
    for (size_t index = 0U; index < count; ++index) {
        const WriterTree* tree = &observation->trees[index];
        unsigned char empty_output[sizeof(schemas[index])];
        memcpy(empty_output, &schemas[index], sizeof(empty_output));
        if (tree->reference) {
            results[index] = serialized_file_schema_create_reference(
                &tail, tree->ordinal, &schema_limits, &schemas[index]);
        } else {
            results[index] = serialized_file_schema_create_ordinary(
                &directory, tree->ordinal, &schema_limits, &schemas[index]);
        }
        if (!result_matches(&results[index], observation, tree, &schema_limits, &schemas[index])) {
            goto cleanup_owners;
        }
        original_views[index] = serialized_file_schema_view(&schemas[index]);
        if (observation->has_tree) {
            if (!schema_matches(&schemas[index],
                    tree,
                    observation,
                    common,
                    results[index].required_retained_bytes)) {
                goto cleanup_owners;
            }
        } else if (memcmp(empty_output, &schemas[index], sizeof(empty_output))) {
            fprintf(stderr, "Tree-free construction changed its empty output\n");
            goto cleanup_owners;
        }
    }

    serialized_file_directory_dispose(&directory);
    serialized_file_metadata_tail_dispose(&tail);
    for (size_t index = 0U; index < count; ++index) {
        if (serialized_file_schema_view(&schemas[index]) != original_views[index]) {
            fprintf(stderr, "Schema view changed after raw-parent disposal\n");
            goto cleanup_owners;
        }
        if (observation->has_tree &&
            !schema_matches(&schemas[index],
                &observation->trees[index],
                observation,
                common,
                results[index].required_retained_bytes)) {
            goto cleanup_owners;
        }
    }
    passed = true;

cleanup_owners:
    for (size_t index = 0U; index < WRITER_MAX_TREES; ++index) {
        serialized_file_schema_dispose(&schemas[index]);
    }
    serialized_file_metadata_tail_dispose(&tail);
    serialized_file_directory_dispose(&directory);
    CHECK(passed);
    return true;
}

static bool check_fixture(
    const char* path, const WriterFixture* fixture, const CommonFileBytes* common) {
    CommonFileBytes file = {0};
    CHECK(common_file_read_regular(path, WRITER_MAX_BYTES, &file) == COMMON_FILE_OK);
    WriterObservation observation;
    const bool passed = observe_metadata(&file, fixture, &observation) &&
        compare_owned_schemas(&observation, common) &&
        digest_matches(file.data, file.size, fixture->sha256);
    common_file_bytes_dispose(&file);
    CHECK(passed);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 8) {
        fprintf(stderr,
            "usage: %s COMMON DIR_TREE DIR_NO_TREE TAIL_TREE TAIL_NO_TREE "
            "REGISTRY_TREE REGISTRY_NO_TREE\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    CommonFileBytes common = {0};
    if (common_file_read_regular(argv[1], OFFICIAL_COMMON_BYTES, &common) != COMMON_FILE_OK) {
        fprintf(stderr, "Could not read official common-string oracle\n");
        return EXIT_FAILURE;
    }
    bool passed = common.size == OFFICIAL_COMMON_BYTES &&
        digest_matches(common.data,
            common.size,
            "4a6ece766a82003fcb86398159b54e5ae84de95b33752950c89b34ea6465444e");
    for (size_t index = 0U; passed && index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        passed = check_fixture(argv[index + 2U], &fixtures[index], &common);
    }
    common_file_bytes_dispose(&common);
    if (!passed) {
        return EXIT_FAILURE;
    }
    puts("Official schema writers: all raw nodes, tagged names and links match "
         "before/after parents.");
    return fflush(stdout) == 0 && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}
