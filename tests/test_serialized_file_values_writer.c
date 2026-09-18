#include "io/serialized_file_values.h"

#include "common/common.h"
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
    MAX_FILE_BYTES = 8192,
    COMMON_BYTES = 1170,
    NODE_BYTES = 32,
    TEXT_NODES = 9
};

typedef struct WireSpan {
    size_t offset;
    size_t size;
} WireSpan;

typedef struct WireType {
    WireSpan source;
    WireSpan type_hash;
    WireSpan node_count;
    WireSpan string_count;
    WireSpan nodes;
    WireSpan strings;
    WireSpan dependency_count;
    WireSpan dependencies;
} WireType;

typedef struct WireString {
    WireSpan source;
    WireSpan length;
    WireSpan bytes;
    WireSpan padding;
} WireString;

typedef struct Observation {
    const uint8_t* bytes;
    size_t size;
    size_t metadata_end;
    size_t data_offset;
    bool tree;
    size_t type_ordinal;
    WireType type;
    WireSpan object;
    WireSpan payload;
    WireString strings[2];
    uint64_t profile_work;
    /* Address-only copy checks after independent raw field/name comparison. */
    const uint8_t* parent_name_bytes[TEXT_NODES][2];
} Observation;

typedef struct Fixture {
    size_t bytes;
    bool tree;
    size_t type_ordinal;
    size_t payload_offset;
    const char* sha256;
} Fixture;

/* Complete official-file pins, never product allowlists. Both writers place
 * TextAsset first in the object table, although its type ordinal differs. */
static const Fixture fixtures[] = {
    {4456U, true, 1U, 4288U, "58955a4e1cb8c769315fab4a96483094263adaa0bd37771dd7e8468d552cf7f6"},
    {4456U, false, 1U, 4288U, "de05057a639dcac017f1f816b6830f767a3977cc736aa1228533df4931f1b0c8"},
    {4336U, true, 0U, 4096U, "0a2dbcb88979c18afb5331524fe5184f44af13c847279f9cefb514d6f585fea0"},
    {4336U, false, 0U, 4096U, "8e462110aa97dd2979c10ea20834e16ceb4c7fa35b41210ce913e86fca942e2e"}};

static uint64_t wire_integer(const uint8_t* bytes, size_t width, bool big_endian) {
    uint64_t value = 0U;
    for (size_t index = 0U; index < width; ++index) {
        value = (value << 8U) | bytes[big_endian ? index : width - 1U - index];
    }
    return value;
}

static bool digest_matches(const CommonFileBytes* file, const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hexadecimal[COMMON_SHA256_HEX_SIZE];
    common_sha256(file->data, file->size, digest);
    common_sha256_digest_to_hex(digest, hexadecimal);
    CHECK(strcmp(hexadecimal, expected) == 0);
    return true;
}

static bool take(Observation* observation, size_t* position, size_t size, WireSpan* span) {
    CHECK(*position <= observation->metadata_end && size <= observation->metadata_end - *position);
    if (span) {
        *span = (WireSpan){*position, size};
    }
    *position += size;
    return true;
}

static bool observe_type(Observation* observation, size_t* position, WireType* output) {
    WireType type = {0};
    type.source.offset = *position;
    CHECK(take(observation, position, 7U, NULL));
    const uint8_t* raw = observation->bytes + type.source.offset;
    const uint32_t class_id = (uint32_t)wire_integer(raw, 4U, false);
    const uint16_t script = (uint16_t)wire_integer(raw + 5U, 2U, false);
    if (class_id == 114U || (script & UINT16_C(0x8000)) == 0U) {
        CHECK(take(observation, position, 16U, NULL));
    }
    CHECK(take(observation, position, 16U, &type.type_hash));
    if (observation->tree) {
        CHECK(take(observation, position, 4U, &type.node_count));
        CHECK(take(observation, position, 4U, &type.string_count));
        const uint64_t nodes = wire_integer(observation->bytes + type.node_count.offset, 4U, false);
        const uint64_t strings =
            wire_integer(observation->bytes + type.string_count.offset, 4U, false);
        CHECK(nodes && nodes <= 128U && strings <= MAX_FILE_BYTES);
        CHECK(take(observation, position, (size_t)nodes * NODE_BYTES, &type.nodes));
        CHECK(take(observation, position, (size_t)strings, &type.strings));
        CHECK(take(observation, position, 4U, &type.dependency_count));
        const uint64_t dependencies =
            wire_integer(observation->bytes + type.dependency_count.offset, 4U, false);
        CHECK(dependencies <= 128U);
        CHECK(take(observation, position, (size_t)dependencies * 4U, &type.dependencies));
    }
    type.source.size = *position - type.source.offset;
    *output = type;
    return true;
}

static bool observe_file(const CommonFileBytes* file, const Fixture* fixture, Observation* output) {
    CHECK(file->size == fixture->bytes && file->size >= 69U);
    const uint8_t* bytes = file->data;
    CHECK(wire_integer(bytes + 8U, 4U, true) == 22U && bytes[40U] == 0U);
    CHECK(wire_integer(bytes + 24U, 8U, true) == file->size);
    CHECK(memcmp(bytes + 48U, "2021.3.35f1\0", 12U) == 0);
    CHECK(wire_integer(bytes + 60U, 4U, false) == 19U);
    CHECK(bytes[64U] == (fixture->tree ? 1U : 0U));
    Observation observation = {.bytes = bytes,
        .size = file->size,
        .metadata_end = 48U + (size_t)wire_integer(bytes + 16U, 8U, true),
        .data_offset = (size_t)wire_integer(bytes + 32U, 8U, true),
        .tree = fixture->tree,
        .type_ordinal = fixture->type_ordinal};
    CHECK(observation.metadata_end <= observation.data_offset &&
        observation.data_offset <= file->size);
    const uint64_t type_count = wire_integer(bytes + 65U, 4U, false);
    CHECK(type_count <= 4U && fixture->type_ordinal < type_count);
    size_t position = 69U;
    for (size_t ordinal = 0U; ordinal < type_count; ++ordinal) {
        WireType type;
        CHECK(observe_type(&observation, &position, &type));
        if (ordinal == fixture->type_ordinal) {
            observation.type = type;
        }
    }
    const size_t object_count_offset = position;
    CHECK(take(&observation, &position, 4U, NULL));
    const uint64_t object_count = wire_integer(bytes + object_count_offset, 4U, false);
    CHECK(object_count && object_count <= 5U);
    CHECK(take(&observation, &position, (4U - position % 4U) % 4U, NULL));
    CHECK(take(&observation, &position, 24U, &observation.object));
    const uint8_t* object = bytes + observation.object.offset;
    const uint64_t relative = wire_integer(object + 8U, 8U, false);
    const uint64_t payload_size = wire_integer(object + 16U, 4U, false);
    CHECK(wire_integer(object + 20U, 4U, false) == fixture->type_ordinal);
    CHECK(relative <= file->size - observation.data_offset);
    observation.payload =
        (WireSpan){observation.data_offset + (size_t)relative, (size_t)payload_size};
    CHECK(observation.payload.offset == fixture->payload_offset && payload_size == 52U);
    CHECK(payload_size <= file->size - observation.payload.offset);
    const uint8_t* type = bytes + observation.type.source.offset;
    CHECK(wire_integer(type, 4U, false) == 49U && type[4U] == 0U &&
        wire_integer(type + 5U, 2U, false) == UINT16_MAX);
    position = observation.payload.offset;
    const size_t end = position + observation.payload.size;
    for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
        WireString* string = &observation.strings[ordinal];
        string->source.offset = position;
        CHECK(end - position >= 4U);
        string->length = (WireSpan){position, 4U};
        const uint64_t length = wire_integer(bytes + position, 4U, false);
        position += 4U;
        CHECK(length <= end - position);
        string->bytes = (WireSpan){position, (size_t)length};
        position += (size_t)length;
        const size_t padding = (4U - position % 4U) % 4U;
        CHECK(padding <= end - position);
        string->padding = (WireSpan){position, padding};
        position += padding;
        string->source.size = position - string->source.offset;
    }
    CHECK(position == end && observation.strings[0].bytes.size == 16U &&
        observation.strings[1].bytes.size == 25U && observation.strings[0].padding.size == 0U &&
        observation.strings[1].padding.size == 3U);
    if (fixture->tree) {
        CHECK(observation.type.nodes.size == TEXT_NODES * NODE_BYTES &&
            !observation.type.strings.size);
    }
    *output = observation;
    return true;
}

static bool span_matches(
    SerializedFilePrefixSpan actual, const Observation* expected, WireSpan span) {
    CHECK(span.offset <= expected->size && span.size <= expected->size - span.offset);
    CHECK(actual.data == expected->bytes + span.offset && actual.offset == span.offset &&
        actual.size == span.size);
    return true;
}

static bool fixed_span(
    SerializedFilePrefixSpan actual, const Observation* expected, size_t offset, size_t size) {
    return span_matches(actual, expected, (WireSpan){offset, size});
}

static bool absent_span(SerializedFilePrefixSpan actual) {
    CHECK(!actual.data && actual.offset == UINT64_MAX && !actual.size);
    return true;
}

static bool prefix_matches(const SerializedFilePrefixView* actual, const Observation* expected) {
    const SerializedFileHeaderView* header = &actual->header;
    CHECK(fixed_span(header->header_source, expected, 0U, 48U));
    CHECK(fixed_span(header->legacy_metadata_size_source, expected, 0U, 4U));
    CHECK(fixed_span(header->legacy_file_size_source, expected, 4U, 4U));
    CHECK(fixed_span(header->format_version_source, expected, 8U, 4U));
    CHECK(fixed_span(header->legacy_data_offset_source, expected, 12U, 4U));
    CHECK(fixed_span(header->metadata_size_source, expected, 16U, 8U));
    CHECK(fixed_span(header->file_size_source, expected, 24U, 8U));
    CHECK(fixed_span(header->data_offset_source, expected, 32U, 8U));
    CHECK(fixed_span(header->endian_selector_source, expected, 40U, 1U));
    CHECK(fixed_span(header->opaque_source, expected, 41U, 7U));
    CHECK(header->format_version == 22U && header->metadata_size == expected->metadata_end - 48U &&
        header->file_size == expected->size && header->data_offset == expected->data_offset &&
        !header->endian_selector);
    CHECK(header->metadata.offset == 48U && header->metadata.size == expected->metadata_end - 48U);
    CHECK(header->metadata_to_data_gap.offset == expected->metadata_end &&
        header->metadata_to_data_gap.size == expected->data_offset - expected->metadata_end);
    CHECK(header->data.offset == expected->data_offset &&
        header->data.size == expected->size - expected->data_offset);
    CHECK(fixed_span(actual->metadata_prefix_source, expected, 48U, 17U));
    CHECK(fixed_span(actual->version_source, expected, 48U, 11U));
    CHECK(fixed_span(actual->version_terminator_source, expected, 59U, 1U));
    CHECK(fixed_span(actual->target_platform_source, expected, 60U, 4U));
    CHECK(fixed_span(actual->type_tree_source, expected, 64U, 1U));
    CHECK(actual->target_platform == 19U && actual->type_tree_enabled_raw == 1U);
    CHECK(actual->remaining_metadata.offset == 65U &&
        actual->remaining_metadata.size == expected->metadata_end - 65U);
    return true;
}

static bool tree_matches(const SerializedFileDirectoryTree* actual, const Observation* expected) {
    const WireType* type = &expected->type;
    CHECK(span_matches(actual->node_count_source, expected, type->node_count));
    CHECK(span_matches(actual->string_count_source, expected, type->string_count));
    CHECK(span_matches(actual->nodes_source, expected, type->nodes));
    CHECK(span_matches(actual->strings_source, expected, type->strings));
    CHECK(actual->node_count == TEXT_NODES && actual->string_byte_count == 0U);
    return true;
}

static bool schema_matches(
    const SerializedFileSchemaView* actual, const Observation* expected, size_t retained) {
    const WireType* type = &expected->type;
    CHECK(actual->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 &&
        actual->row_kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE &&
        actual->type_ordinal == expected->type_ordinal);
    CHECK(prefix_matches(&actual->prefix, expected) &&
        span_matches(actual->type_entry_source, expected, type->source));
    CHECK(tree_matches(&actual->tree, expected));
    CHECK(actual->class_id_bits == 49U && actual->script_index_bits == UINT16_MAX &&
        !actual->stripped_raw && !actual->has_script_hash &&
        absent_span(actual->script_hash_source));
    CHECK(span_matches(actual->type_hash_source, expected, type->type_hash));
    CHECK(absent_span(actual->class_name_source) && absent_span(actual->namespace_source) &&
        absent_span(actual->assembly_name_source));
    CHECK(span_matches(actual->dependency_count_source, expected, type->dependency_count));
    CHECK(span_matches(actual->dependency_words_source, expected, type->dependencies));
    CHECK(actual->dependency_count ==
        wire_integer(expected->bytes + type->dependency_count.offset, 4U, false));
    CHECK(actual->node_count == TEXT_NODES && actual->maximum_depth == 3U &&
        actual->retained_bytes == retained);
    return true;
}

static bool context_matches(
    const SerializedFileSchemaContext* actual, const Observation* expected) {
    CHECK(actual->kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT &&
        actual->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 &&
        actual->row_kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE &&
        actual->type_ordinal == expected->type_ordinal);
    CHECK(fixed_span(actual->header_source, expected, 0U, 48U));
    CHECK(actual->file_size == expected->size && !actual->endian_selector);
    CHECK(span_matches(actual->type_entry_source, expected, expected->type.source));
    CHECK(tree_matches(&actual->tree, expected));
    CHECK(actual->root == 0U && actual->first_child == 1U && actual->traversal_end == TEXT_NODES &&
        actual->registry == SIZE_MAX && actual->registry_end == SIZE_MAX &&
        !actual->registry_omitted);
    return true;
}

static uint8_t raw_level(const Observation* expected, size_t ordinal) {
    return expected->bytes[expected->type.nodes.offset + ordinal * NODE_BYTES + 2U];
}

static bool name_matches(
    const SerializedFileSchemaString* actual, const CommonFileBytes* common, uint32_t encoded) {
    CHECK((encoded & UINT32_C(0x80000000)) != 0U);
    const size_t offset = encoded & UINT32_C(0x7fffffff);
    CHECK(offset < common->size && (offset == 0U || common->data[offset - 1U] == 0U));
    const uint8_t* end = memchr(common->data + offset, 0, common->size - offset);
    CHECK(end);
    const size_t length = (size_t)(end - (common->data + offset));
    CHECK(actual->space == SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35 &&
        actual->encoded_offset == encoded && actual->source_offset == offset &&
        actual->byte_count == length && actual->bytes);
    CHECK(memcmp(actual->bytes, common->data + offset, length + 1U) == 0);
    return true;
}

static bool node_matches(const SerializedFileSchemaNode* actual,
    size_t ordinal,
    const Observation* expected,
    const CommonFileBytes* common) {
    const size_t offset = expected->type.nodes.offset + ordinal * NODE_BYTES;
    const uint8_t* raw = expected->bytes + offset;
    CHECK(actual->ordinal == ordinal && fixed_span(actual->source, expected, offset, NODE_BYTES));
    CHECK(actual->version == wire_integer(raw, 2U, false) && actual->level == raw[2U] &&
        actual->type_flags == raw[3U]);
    CHECK(actual->type_offset_bits == wire_integer(raw + 4U, 4U, false) &&
        actual->name_offset_bits == wire_integer(raw + 8U, 4U, false));
    CHECK(actual->byte_size_bits == wire_integer(raw + 12U, 4U, false) &&
        actual->index_bits == wire_integer(raw + 16U, 4U, false) &&
        actual->meta_flags == wire_integer(raw + 20U, 4U, false) &&
        memcmp(actual->opaque_tail, raw + 24U, 8U) == 0);
    CHECK(name_matches(&actual->type_name, common, (uint32_t)wire_integer(raw + 4U, 4U, false)));
    CHECK(name_matches(&actual->field_name, common, (uint32_t)wire_integer(raw + 8U, 4U, false)));
    if (expected->parent_name_bytes[ordinal][0]) {
        CHECK(actual->type_name.bytes == expected->parent_name_bytes[ordinal][0] &&
            actual->field_name.bytes == expected->parent_name_bytes[ordinal][1]);
    }
    /* Independent bounded raw scans, not the owner's ancestor-stack algorithm. */
    const uint8_t level = raw[2U];
    size_t parent = SIZE_MAX;
    for (size_t previous = ordinal; previous > 0U; --previous) {
        if (raw_level(expected, previous - 1U) < level) {
            parent = previous - 1U;
            break;
        }
    }
    size_t end = ordinal + 1U;
    while (end < TEXT_NODES && raw_level(expected, end) > level) {
        ++end;
    }
    size_t child_count = 0U;
    size_t first_child = SIZE_MAX;
    for (size_t child = ordinal + 1U; child < end; ++child) {
        if (raw_level(expected, child) == level + 1U) {
            if (first_child == SIZE_MAX) {
                first_child = child;
            }
            ++child_count;
        }
    }
    const size_t sibling = end < TEXT_NODES && raw_level(expected, end) == level ? end : SIZE_MAX;
    CHECK(actual->parent == parent && actual->first_child == first_child &&
        actual->next_sibling == sibling && actual->child_count == child_count &&
        actual->subtree_end == end);
    return true;
}

static bool observe_profile_work(Observation* observation, const CommonFileBytes* common) {
    uint64_t work = TEXT_NODES + 2U; /* Context admission, every raw node, publication. */
    for (size_t ordinal = 0U; ordinal < TEXT_NODES; ++ordinal) {
        const uint8_t* node =
            observation->bytes + observation->type.nodes.offset + ordinal * NODE_BYTES;
        ++work;
        for (size_t word = 4U; word <= 8U; word += 4U) {
            const uint32_t encoded = (uint32_t)wire_integer(node + word, 4U, false);
            CHECK(encoded & UINT32_C(0x80000000));
            const size_t offset = encoded & UINT32_C(0x7fffffff);
            CHECK(offset < common->size);
            const uint8_t* terminator = memchr(common->data + offset, 0, common->size - offset);
            CHECK(terminator);
            work += 1U + (size_t)(terminator - (common->data + offset));
        }
    }
    CHECK(work == 127U);
    observation->profile_work = work;
    return true;
}

static bool value_matches(const SerializedFileValue* actual,
    size_t ordinal,
    const Observation* expected,
    const CommonFileBytes* common) {
    static const size_t schema_ordinals[] = {0U, 1U, 5U};
    CHECK(actual->ordinal == ordinal &&
        actual->kind ==
            (ordinal == 0U ? SERIALIZED_FILE_VALUE_CONTAINER : SERIALIZED_FILE_VALUE_BYTE_STRING));
    CHECK(node_matches(&actual->schema_node, schema_ordinals[ordinal], expected, common));
    CHECK(actual->integer_bits == 0U && absent_span(actual->integer_source));
    CHECK(actual->parent == (ordinal == 0U ? SIZE_MAX : 0U) &&
        actual->first_child == (ordinal == 0U ? 1U : SIZE_MAX) &&
        actual->next_sibling == (ordinal == 1U ? 2U : SIZE_MAX) &&
        actual->child_count == (ordinal == 0U ? 2U : 0U) &&
        actual->subtree_end == (ordinal == 0U ? 3U : ordinal + 1U));
    if (ordinal == 0U) {
        CHECK(span_matches(actual->source, expected, expected->payload));
        CHECK(actual->array_schema_ordinal == SIZE_MAX && actual->size_schema_ordinal == SIZE_MAX &&
            actual->data_schema_ordinal == SIZE_MAX && actual->length_bits == 0U);
        CHECK(absent_span(actual->array_schema_source) && absent_span(actual->size_schema_source) &&
            absent_span(actual->data_schema_source) && absent_span(actual->length_source) &&
            absent_span(actual->bytes_source) && absent_span(actual->padding_source));
        return true;
    }
    const size_t array = schema_ordinals[ordinal] + 1U;
    const WireString* string = &expected->strings[ordinal - 1U];
    CHECK(actual->array_schema_ordinal == array && actual->size_schema_ordinal == array + 1U &&
        actual->data_schema_ordinal == array + 2U);
    CHECK(fixed_span(actual->array_schema_source,
        expected,
        expected->type.nodes.offset + array * NODE_BYTES,
        NODE_BYTES));
    CHECK(fixed_span(actual->size_schema_source,
        expected,
        expected->type.nodes.offset + (array + 1U) * NODE_BYTES,
        NODE_BYTES));
    CHECK(fixed_span(actual->data_schema_source,
        expected,
        expected->type.nodes.offset + (array + 2U) * NODE_BYTES,
        NODE_BYTES));
    CHECK(span_matches(actual->source, expected, string->source) &&
        span_matches(actual->length_source, expected, string->length));
    CHECK(span_matches(actual->bytes_source, expected, string->bytes) &&
        span_matches(actual->padding_source, expected, string->padding));
    CHECK(actual->length_bits == wire_integer(expected->bytes + string->length.offset, 4U, false));
    return true;
}

static bool view_matches(const SerializedFileValuesView* actual,
    const Observation* expected,
    size_t schema_bytes,
    size_t values_bytes) {
    const uint8_t* object = expected->bytes + expected->object.offset;
    CHECK(actual->object.ordinal == 0U &&
        span_matches(actual->object.source, expected, expected->object));
    CHECK(actual->object.path_id_bits == wire_integer(object, 8U, false) &&
        actual->object.relative_data_offset == wire_integer(object + 8U, 8U, false) &&
        actual->object.byte_size == wire_integer(object + 16U, 4U, false) &&
        actual->object.type_ordinal == expected->type_ordinal &&
        actual->object.payload.offset == expected->payload.offset &&
        actual->object.payload.size == expected->payload.size);
    CHECK(schema_matches(&actual->schema, expected, schema_bytes) &&
        context_matches(&actual->context, expected));
    CHECK(actual->value_count == 3U && actual->maximum_depth == 1U &&
        actual->consumed_bytes == 52U && actual->string_bytes == 41U &&
        actual->padding_bytes == 3U && actual->retained_bytes == values_bytes &&
        actual->integer_bytes == 0U);
    return true;
}

static bool context_result_matches(
    const SerializedFileSchemaContextResult* actual, bool attempted, size_t type) {
    CHECK(actual->status == SERIALIZED_FILE_SCHEMA_CONTEXT_OK &&
        actual->field == SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE &&
        actual->type_ordinal == (attempted ? type : SIZE_MAX) && actual->node_ordinal == SIZE_MAX &&
        actual->error_offset == UINT64_MAX && actual->work_used == (attempted ? 11U : 0U));
    return true;
}

static bool check_fixture(const char* path, const Fixture* fixture, const CommonFileBytes* common) {
    CommonFileBytes file = {0};
    SerializedFileDirectory directory = {0};
    SerializedFileSchema schema = {0};
    SerializedFileValues values = {0};
    bool passed = false;
    Observation expected;
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 4U, 5U, 128U, MAX_FILE_BYTES, 128U, MAX_FILE_BYTES, 1048576U, 0U, 1048576U};
    const SerializedFileSchemaLimits schema_limits = {
        TEXT_NODES, 0U, 3U, 1048576U, 1048576U, 1048576U};
    const SerializedFileValuesLimits limits = {.max_payload_bytes = 52U,
        .max_values = 3U,
        .max_depth = 1U,
        .max_string_bytes = 25U,
        .max_total_string_bytes = 41U,
        .max_padding_bytes = 3U,
        .max_retained_bytes = 1048576U,
        .max_work = 1048576U};
    if (common_file_read_regular(path, MAX_FILE_BYTES, &file) != COMMON_FILE_OK ||
        !digest_matches(&file, fixture->sha256) || !observe_file(&file, fixture, &expected)) {
        goto cleanup_fixture;
    }
    const SerializedFileDirectoryResult directory_result =
        serialized_file_directory_create(file.data,
            file.size,
            file.size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &directory_limits,
            &directory);
    if (directory_result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        goto cleanup_fixture;
    }
    size_t schema_bytes = 0U;
    if (fixture->tree) {
        if (!observe_profile_work(&expected, common)) {
            goto cleanup_fixture;
        }
        const SerializedFileSchemaResult schema_result = serialized_file_schema_create_ordinary(
            &directory, fixture->type_ordinal, &schema_limits, &schema);
        if (schema_result.status != SERIALIZED_FILE_SCHEMA_OK) {
            goto cleanup_fixture;
        }
        schema_bytes = schema_result.required_retained_bytes;
        if (schema_bytes <
                sizeof(SerializedFileSchemaView) + TEXT_NODES * sizeof(SerializedFileSchemaNode) ||
            schema_bytes != schema_result.peak_retained_bytes ||
            schema_bytes > schema_limits.max_retained_bytes) {
            goto cleanup_fixture;
        }
        for (size_t ordinal = 0U; ordinal < TEXT_NODES; ++ordinal) {
            const SerializedFileSchemaNode* node = serialized_file_schema_node(&schema, ordinal);
            if (!node || !node_matches(node, ordinal, &expected, common)) {
                goto cleanup_fixture;
            }
            expected.parent_name_bytes[ordinal][0] = node->type_name.bytes;
            expected.parent_name_bytes[ordinal][1] = node->field_name.bytes;
        }
    }
    const SerializedFileValuesResult result = serialized_file_values_create_text_asset(
        &directory, &schema, 0U, file.data, file.size, &limits, &values);
    if (!fixture->tree) {
        passed = result.status == SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA &&
            result.limit == SERIALIZED_FILE_VALUES_LIMIT_NONE &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_TYPE && result.object_ordinal == 0U &&
            result.type_ordinal == fixture->type_ordinal && result.node_ordinal == SIZE_MAX &&
            result.error_offset == expected.type.source.offset && result.work_used == 1U &&
            !result.required_retained_bytes && !result.peak_retained_bytes &&
            !result.context_attempted &&
            context_result_matches(&result.context_result, false, fixture->type_ordinal) &&
            !values.implementation && !serialized_file_values_view(&values) &&
            !serialized_file_values_value(&values, 0U) && digest_matches(&file, fixture->sha256);
        goto cleanup_fixture;
    }
    const size_t retained = result.required_retained_bytes;
    const uint64_t work =
        retained + expected.profile_work + 2U * (expected.payload.size + 6U + 3U) + 3U;
    if (result.status != SERIALIZED_FILE_VALUES_OK ||
        result.limit != SERIALIZED_FILE_VALUES_LIMIT_NONE ||
        result.field != SERIALIZED_FILE_VALUES_FIELD_NONE || result.object_ordinal != 0U ||
        result.type_ordinal != fixture->type_ordinal || result.node_ordinal != SIZE_MAX ||
        result.error_offset != UINT64_MAX || result.work_used != work ||
        result.peak_retained_bytes != retained || !result.context_attempted ||
        retained < sizeof(SerializedFileValuesView) + 3U * sizeof(SerializedFileValue) ||
        retained > limits.max_retained_bytes ||
        !context_result_matches(&result.context_result, true, fixture->type_ordinal)) {
        goto cleanup_fixture;
    }
    SerializedFileValuesView copied_view;
    SerializedFileValue copied_values[3];
    for (size_t pass = 0U; pass < 2U; ++pass) {
        const SerializedFileValuesView* view = serialized_file_values_view(&values);
        if (!view || !view_matches(view, &expected, schema_bytes, retained)) {
            goto cleanup_fixture;
        }
        copied_view = *view;
        for (size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
            const SerializedFileValue* value = serialized_file_values_value(&values, ordinal);
            if (!value || !value_matches(value, ordinal, &expected, common)) {
                goto cleanup_fixture;
            }
            copied_values[ordinal] = *value;
        }
        serialized_file_schema_dispose(&schema);
        serialized_file_directory_dispose(&directory);
    }
    if (serialized_file_values_value(&values, 3U) ||
        serialized_file_values_value(&values, SIZE_MAX)) {
        goto cleanup_fixture;
    }
    serialized_file_values_dispose(&values);
    if (serialized_file_values_view(&values) || serialized_file_values_value(&values, 0U) ||
        !view_matches(&copied_view, &expected, schema_bytes, retained)) {
        goto cleanup_fixture;
    }
    for (size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
        if (!value_matches(&copied_values[ordinal], ordinal, &expected, common)) {
            goto cleanup_fixture;
        }
    }
    passed = digest_matches(&file, fixture->sha256);
cleanup_fixture:
    serialized_file_values_dispose(&values);
    serialized_file_schema_dispose(&schema);
    serialized_file_directory_dispose(&directory);
    common_file_bytes_dispose(&file);
    CHECK(passed);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr,
            "usage: %s COMMON DIRECTORY_TREE DIRECTORY_NO_TREE TAIL_TREE TAIL_NO_TREE\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    CommonFileBytes common = {0};
    bool passed = common_file_read_regular(argv[1], COMMON_BYTES, &common) == COMMON_FILE_OK &&
        common.size == COMMON_BYTES &&
        digest_matches(&common, "4a6ece766a82003fcb86398159b54e5ae84de95b33752950c89b34ea6465444e");
    for (size_t index = 0U; passed && index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        passed = check_fixture(argv[index + 2U], &fixtures[index], &common);
    }
    if (passed) {
        passed = digest_matches(
            &common, "4a6ece766a82003fcb86398159b54e5ae84de95b33752950c89b34ea6465444e");
    }
    common_file_bytes_dispose(&common);
    if (!passed || atomic_load(&g_allocations_count) || atomic_load(&g_allocated_bytes)) {
        return EXIT_FAILURE;
    }
    puts(
        "Official TextAsset values: two exact 52-byte objects, all fields/lifetimes and two tree-free absences pass.");
    return EXIT_SUCCESS;
}
