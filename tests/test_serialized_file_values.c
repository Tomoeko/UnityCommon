#include "io/serialized_file_values.h"

#define SERIALIZED_VALUES_ALLOCATION_DECLARATIONS_ONLY
#include "serialized_values_allocation_test.h"

#include "common/common.h"

#include <stdio.h>
#include <stdlib.h>
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
    FIXTURE_CAPACITY = 4096,
    PAYLOAD_START = 2048,
    TYPE_COUNT = 2,
    NODE_COUNT = 9,
    NODE_BYTES = 32,
    PROFILE_WORK = 127
};

typedef struct ValuesFixture {
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t type_start[TYPE_COUNT];
    size_t nodes_start[TYPE_COUNT];
    size_t strings_start[TYPE_COUNT];
    size_t reference_nodes;
    size_t object_start;
    size_t metadata_end;
    size_t payload_start;
    size_t file_size;
    bool big_endian;
    bool tree_enabled;
    bool exact29;
    bool reference;
    bool common_names;
} ValuesFixture;

typedef struct ValuesParents {
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
    SerializedFileSchema schema;
} ValuesParents;

static const SerializedFileValuesLimits generous_limits = {.max_payload_bytes = FIXTURE_CAPACITY,
    .max_values = 3U,
    .max_depth = 1U,
    .max_string_bytes = FIXTURE_CAPACITY,
    .max_total_string_bytes = FIXTURE_CAPACITY,
    .max_padding_bytes = 6U,
    .max_retained_bytes = 65536U,
    .max_work = UINT64_C(1000000),
    .max_integer_bytes = 0U,
    .max_total_integer_bytes = 0U};
static const SerializedFileSchemaLimits schema_limits = {
    100U, FIXTURE_CAPACITY, 255U, 1048576U, 1048576U, UINT64_C(1000000)};

static size_t allocation_calls;
static size_t failing_allocation;
static size_t live_allocations;
static size_t last_allocation_bytes;

void* values_test_allocate(size_t size) {
    ++allocation_calls;
    last_allocation_bytes = size;
    if (allocation_calls == failing_allocation) {
        return NULL;
    }
    void* allocation = malloc(size);
    if (allocation) {
        ++live_allocations;
    }
    return allocation;
}

void values_test_release(void* allocation) {
    if (allocation) {
        --live_allocations;
        free(allocation);
    }
}

static void write_integer(uint8_t* bytes, size_t width, uint64_t bits, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        bytes[big_endian ? width - index - 1U : index] = (uint8_t)bits;
        bits >>= 8U;
    }
}

static size_t write_type(ValuesFixture* fixture, size_t offset, size_t ordinal, bool reference) {
    /* This independent wire table is the complete observed TextAsset profile.
     * Common offsets below come from the pinned 1170-byte official fixture. */
    static const char* const types[NODE_COUNT] = {
        "TextAsset", "string", "Array", "int", "char", "string", "Array", "int", "char"};
    static const char* const fields[NODE_COUNT] = {
        "Base", "m_Name", "Array", "size", "data", "m_Script", "Array", "size", "data"};
    static const uint32_t common_types[NODE_COUNT] = {
        847U, 840U, 49U, 222U, 81U, 840U, 49U, 222U, 81U};
    static const uint32_t common_fields[NODE_COUNT] = {
        55U, 427U, 49U, 795U, 106U, 490U, 49U, 795U, 106U};
    static const uint8_t levels[NODE_COUNT] = {0U, 1U, 2U, 3U, 3U, 1U, 2U, 3U, 3U};
    static const uint32_t widths[NODE_COUNT] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, 4U, 1U, UINT32_MAX, UINT32_MAX, 4U, 1U};
    static const uint32_t flags[NODE_COUNT] = {0x8000U,
        0x88001U,
        0x84001U,
        0x80001U,
        0x80001U,
        0x4008001U,
        0x4004001U,
        0x4000001U,
        0x4000001U};
    uint8_t* bytes = fixture->bytes;
    if (!reference) {
        fixture->type_start[ordinal] = offset;
    }
    write_integer(bytes + offset, 4U, reference ? UINT32_MAX : 49U, fixture->big_endian);
    write_integer(bytes + offset + 5U, 2U, reference ? 0U : UINT16_MAX, fixture->big_endian);
    offset += 7U;
    const size_t hash_size = reference ? 32U : 16U;
    for (size_t index = 0U; index < hash_size; ++index) {
        bytes[offset + index] = (uint8_t)(0xc0U + index + ordinal);
    }
    offset += hash_size;
    if (!fixture->tree_enabled) {
        return offset;
    }
    write_integer(bytes + offset, 4U, NODE_COUNT, fixture->big_endian);
    const size_t string_count_word = offset + 4U;
    const size_t nodes_start = offset + 8U;
    const size_t strings_start = nodes_start + NODE_COUNT * NODE_BYTES;
    size_t string_bytes = 0U;
    if (reference) {
        fixture->reference_nodes = nodes_start;
    } else {
        fixture->nodes_start[ordinal] = nodes_start;
        fixture->strings_start[ordinal] = strings_start;
    }
    for (size_t node_index = 0U; node_index < NODE_COUNT; ++node_index) {
        uint8_t* node = bytes + nodes_start + node_index * NODE_BYTES;
        write_integer(node, 2U, 1U, fixture->big_endian);
        node[2] = levels[node_index];
        node[3] = node_index == 2U || node_index == 6U ? 1U : 0U;
        const char* const names[] = {types[node_index], fields[node_index]};
        for (size_t name_index = 0U; name_index < 2U; ++name_index) {
            uint32_t encoded;
            if (fixture->common_names) {
                encoded = UINT32_C(0x80000000) |
                    (name_index == 0U ? common_types[node_index] : common_fields[node_index]);
            } else {
                encoded = (uint32_t)string_bytes;
                const size_t length = strlen(names[name_index]) + 1U;
                memcpy(bytes + strings_start + string_bytes, names[name_index], length);
                string_bytes += length;
            }
            write_integer(node + 4U + name_index * 4U, 4U, encoded, fixture->big_endian);
        }
        write_integer(node + 12U, 4U, widths[node_index], fixture->big_endian);
        write_integer(node + 16U, 4U, node_index, fixture->big_endian);
        write_integer(node + 20U, 4U, flags[node_index], fixture->big_endian);
    }
    write_integer(bytes + string_count_word, 4U, string_bytes, fixture->big_endian);
    offset = strings_start + string_bytes;
    if (reference) {
        static const char identity[] = "TextAsset\0Example\0Assembly\0";
        memcpy(bytes + offset, identity, sizeof(identity) - 1U);
        offset += sizeof(identity) - 1U;
    } else {
        write_integer(bytes + offset, 4U, 0U, fixture->big_endian);
        offset += 4U;
    }
    return offset;
}

static size_t write_string(
    ValuesFixture* fixture, size_t offset, const uint8_t* bytes, size_t size) {
    write_integer(fixture->bytes + offset, 4U, size, fixture->big_endian);
    offset += 4U;
    if (size != 0U) {
        memcpy(fixture->bytes + offset, bytes, size);
    }
    offset += size;
    while (offset % 4U != 0U) {
        fixture->bytes[offset++] = 0xa5U; /* Preserve nonzero padding verbatim. */
    }
    return offset;
}

static void fixture_payload(ValuesFixture* fixture,
    const uint8_t* first,
    size_t first_size,
    const uint8_t* second,
    size_t second_size) {
    size_t end = write_string(fixture, fixture->payload_start, first, first_size);
    end = write_string(fixture, end, second, second_size);
    fixture->file_size = end;
    write_integer(fixture->bytes + fixture->object_start + 16U,
        4U,
        end - fixture->payload_start,
        fixture->big_endian);
    write_integer(fixture->bytes + 24U, 8U, end, true);
}

static void fixture_init(ValuesFixture* fixture,
    bool big_endian,
    bool tree_enabled,
    bool exact29,
    bool reference,
    bool common_names) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->big_endian = big_endian;
    fixture->tree_enabled = tree_enabled;
    fixture->exact29 = exact29;
    fixture->reference = reference;
    fixture->common_names = common_names;
    fixture->payload_start = PAYLOAD_START;
    uint8_t* bytes = fixture->bytes;
    memcpy(bytes + 48U, exact29 ? "2021.3.29f1" : "2021.3.35f1", 12U);
    bytes[64U] = tree_enabled ? 1U : 0U;
    write_integer(bytes + 65U, 4U, TYPE_COUNT, big_endian);
    size_t offset = 69U;
    for (size_t ordinal = 0U; ordinal < TYPE_COUNT; ++ordinal) {
        offset = write_type(fixture, offset, ordinal, false);
    }
    write_integer(bytes + offset, 4U, 1U, big_endian);
    offset += 4U;
    while (offset % 4U != 0U) {
        bytes[offset++] = 0xdbU;
    }
    fixture->object_start = offset;
    write_integer(bytes + offset, 8U, UINT64_C(0xfedcba9876543210), big_endian);
    offset += 24U;
    write_integer(bytes + offset, 4U, 0U, big_endian);
    write_integer(bytes + offset + 4U, 4U, 0U, big_endian);
    write_integer(bytes + offset + 8U, 4U, reference ? 1U : 0U, big_endian);
    offset += 12U;
    if (reference) {
        offset = write_type(fixture, offset, 0U, true);
    }
    bytes[offset++] = 0U;
    fixture->metadata_end = offset;
    write_integer(bytes + 8U, 4U, 22U, true);
    write_integer(bytes + 16U, 8U, offset - 48U, true);
    write_integer(bytes + 32U, 8U, fixture->payload_start, true);
    bytes[40U] = big_endian ? 1U : 0U;
    static const uint8_t first[] = {'A', 0U, 0xffU};
    static const uint8_t second[] = {'b', 'c', 'd', 'e', 'f'};
    fixture_payload(fixture, first, sizeof(first), second, sizeof(second));
}

static bool prepare_parents_from_bytes(const ValuesFixture* fixture,
    const uint8_t* bytes,
    size_t schema_type,
    bool reference,
    ValuesParents* parents) {
    const SerializedFileDirectoryLimits directory_limits = {12U,
        TYPE_COUNT,
        1U,
        100U,
        FIXTURE_CAPACITY,
        0U,
        FIXTURE_CAPACITY,
        1048576U,
        0U,
        UINT64_C(1000000)};
    const SerializedFileDirectoryEngineVersion engine = fixture->exact29
        ? SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1
        : SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1;
    CHECK(serialized_file_directory_create(bytes,
              fixture->metadata_end,
              fixture->file_size,
              engine,
              &directory_limits,
              &parents->directory)
              .status == SERIALIZED_FILE_DIRECTORY_OK);
    if (!fixture->tree_enabled || fixture->exact29) {
        return true;
    }
    if (reference) {
        const SerializedFileMetadataTailLimits tail_limits = {0U,
            0U,
            1U,
            100U,
            FIXTURE_CAPACITY,
            FIXTURE_CAPACITY,
            FIXTURE_CAPACITY,
            1048576U,
            0U,
            UINT64_C(1000000)};
        CHECK(serialized_file_metadata_tail_create(&parents->directory,
                  bytes,
                  fixture->metadata_end,
                  fixture->file_size,
                  &tail_limits,
                  &parents->tail)
                  .status == SERIALIZED_FILE_METADATA_TAIL_OK);
        CHECK(serialized_file_schema_create_reference(
                  &parents->tail, schema_type, &schema_limits, &parents->schema)
                  .status == SERIALIZED_FILE_SCHEMA_OK);
    } else {
        CHECK(serialized_file_schema_create_ordinary(
                  &parents->directory, schema_type, &schema_limits, &parents->schema)
                  .status == SERIALIZED_FILE_SCHEMA_OK);
    }
    return true;
}

static bool prepare_parents(
    const ValuesFixture* fixture, size_t schema_type, bool reference, ValuesParents* parents) {
    return prepare_parents_from_bytes(fixture, fixture->bytes, schema_type, reference, parents);
}

static void dispose_parents(ValuesParents* parents) {
    serialized_file_schema_dispose(&parents->schema);
    serialized_file_metadata_tail_dispose(&parents->tail);
    serialized_file_directory_dispose(&parents->directory);
}

static SerializedFileValuesResult create_values(const ValuesFixture* fixture,
    const ValuesParents* parents,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* output) {
    return serialized_file_values_create_text_asset(&parents->directory,
        &parents->schema,
        0U,
        fixture->bytes,
        fixture->file_size,
        limits,
        output);
}

static bool span_is(
    const ValuesFixture* fixture, SerializedFilePrefixSpan span, size_t offset, size_t size) {
    return span.data == fixture->bytes + offset && span.offset == offset && span.size == size;
}

static bool span_absent(SerializedFilePrefixSpan span) {
    return span.data == NULL && span.offset == UINT64_MAX && span.size == 0U;
}

static bool check_values(const ValuesFixture* fixture, const SerializedFileValues* values) {
    const SerializedFileValuesView* view = serialized_file_values_view(values);
    CHECK(
        view && view->value_count == 3U && view->maximum_depth == 1U && view->integer_bytes == 0U);
    CHECK(view->object.ordinal == 0U && view->object.type_ordinal == 0U &&
        view->object.path_id_bits == UINT64_C(0xfedcba9876543210));
    CHECK(span_is(fixture, view->object.source, fixture->object_start, 24U));
    CHECK(view->object.payload.offset == PAYLOAD_START && view->object.payload.size == 20U &&
        view->consumed_bytes == 20U && view->string_bytes == 8U && view->padding_bytes == 4U);
    CHECK(view->schema.type_ordinal == 0U && view->schema.node_count == NODE_COUNT &&
        view->schema.tree.nodes_source.data == fixture->bytes + fixture->nodes_start[0]);
    CHECK(view->context.kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT &&
        view->context.row_kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE &&
        view->context.root == 0U && view->context.first_child == 1U &&
        view->context.traversal_end == NODE_COUNT && view->context.registry == SIZE_MAX &&
        view->context.registry_end == SIZE_MAX && !view->context.registry_omitted);
    const size_t source_offsets[] = {PAYLOAD_START, PAYLOAD_START, PAYLOAD_START + 8U};
    const size_t source_sizes[] = {20U, 8U, 12U};
    const size_t schema_ordinals[] = {0U, 1U, 5U};
    for (size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
        const SerializedFileValue* value = serialized_file_values_value(values, ordinal);
        CHECK(value && value->ordinal == ordinal &&
            value->schema_node.ordinal == schema_ordinals[ordinal]);
        CHECK(value->integer_bits == 0U && span_absent(value->integer_source));
        CHECK(span_is(fixture, value->source, source_offsets[ordinal], source_sizes[ordinal]));
        CHECK(span_is(fixture,
            value->schema_node.source,
            fixture->nodes_start[0] + schema_ordinals[ordinal] * NODE_BYTES,
            NODE_BYTES));
        CHECK(value->parent == (ordinal == 0U ? SIZE_MAX : 0U) &&
            value->first_child == (ordinal == 0U ? 1U : SIZE_MAX) &&
            value->next_sibling == (ordinal == 1U ? 2U : SIZE_MAX) &&
            value->child_count == (ordinal == 0U ? 2U : 0U) &&
            value->subtree_end == (ordinal == 0U ? 3U : ordinal + 1U));
        if (ordinal == 0U) {
            CHECK(value->kind == SERIALIZED_FILE_VALUE_CONTAINER && value->length_bits == 0U &&
                value->array_schema_ordinal == SIZE_MAX && value->size_schema_ordinal == SIZE_MAX &&
                value->data_schema_ordinal == SIZE_MAX && span_absent(value->array_schema_source) &&
                span_absent(value->size_schema_source) && span_absent(value->data_schema_source) &&
                span_absent(value->length_source) && span_absent(value->bytes_source) &&
                span_absent(value->padding_source));
            continue;
        }
        const size_t string_length = ordinal == 1U ? 3U : 5U;
        CHECK(value->kind == SERIALIZED_FILE_VALUE_BYTE_STRING &&
            value->length_bits == string_length &&
            value->array_schema_ordinal == schema_ordinals[ordinal] + 1U &&
            value->size_schema_ordinal == schema_ordinals[ordinal] + 2U &&
            value->data_schema_ordinal == schema_ordinals[ordinal] + 3U);
        CHECK(span_is(fixture, value->length_source, source_offsets[ordinal], 4U) &&
            span_is(fixture, value->bytes_source, source_offsets[ordinal] + 4U, string_length) &&
            span_is(fixture,
                value->padding_source,
                source_offsets[ordinal] + 4U + string_length,
                ordinal == 1U ? 1U : 3U));
        CHECK(value->padding_source.data[0] == 0xa5U);
    }
    CHECK(!serialized_file_values_value(values, 3U) &&
        !serialized_file_values_value(values, SIZE_MAX));
    return true;
}

static bool success_and_lifetime(void) {
    for (size_t common = 0U; common < 2U; ++common) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, common != 0U);
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues values = {0};
        allocation_calls = 0U;
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &generous_limits, &values);
        CHECK(result.status == SERIALIZED_FILE_VALUES_OK && result.object_ordinal == 0U &&
            result.type_ordinal == 0U && result.node_ordinal == SIZE_MAX &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_NONE && result.error_offset == UINT64_MAX);
        CHECK(result.context_attempted &&
            result.context_result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_OK &&
            result.context_result.work_used == 11U && result.context_result.type_ordinal == 0U);
        CHECK(result.required_retained_bytes == result.peak_retained_bytes &&
            allocation_calls == 1U && live_allocations == 1U);
        CHECK(result.work_used ==
            result.required_retained_bytes + PROFILE_WORK + 2U * (20U + 9U) + 3U);
        CHECK(check_values(&fixture, &values));
        SerializedFileValuesView saved_view = *serialized_file_values_view(&values);
        SerializedFileValue saved_value = *serialized_file_values_value(&values, 1U);
        dispose_parents(&parents);
        CHECK(check_values(&fixture, &values));
        serialized_file_values_dispose(&values);
        CHECK(saved_view.object.source.data == fixture.bytes + fixture.object_start &&
            saved_value.bytes_source.data[1] == 0U && saved_value.bytes_source.data[2] == 0xffU);
        CHECK(live_allocations == 0U && !serialized_file_values_view(&values));
        serialized_file_values_dispose(&values);
    }
    serialized_file_values_init(NULL);
    serialized_file_values_dispose(NULL);
    CHECK(!serialized_file_values_view(NULL) && !serialized_file_values_value(NULL, 0U));
    return true;
}

static bool same_context_result(const SerializedFileSchemaContextResult* first,
    const SerializedFileSchemaContextResult* second) {
    return first->status == second->status && first->field == second->field &&
        first->type_ordinal == second->type_ordinal &&
        first->node_ordinal == second->node_ordinal &&
        first->error_offset == second->error_offset && first->work_used == second->work_used;
}

typedef struct ExpectedWorkStep {
    uint64_t units;
    SerializedFileValuesField field;
    size_t node;
    uint64_t offset;
} ExpectedWorkStep;

typedef struct ExpectedWork {
    ExpectedWorkStep steps[384];
    size_t count;
    size_t planning_step;
    size_t allocation_step;
} ExpectedWork;

static void append_work(ExpectedWork* work,
    uint64_t units,
    SerializedFileValuesField field,
    size_t node,
    uint64_t offset) {
    work->steps[work->count++] = (ExpectedWorkStep){units, field, node, offset};
}

static void append_payload_work(ExpectedWork* work) {
    /* Two observed strings: (length3,padding1) then (length5,padding3).
     * Record completion follows that record's final admitted span. */
    for (size_t string = 0U; string < 2U; ++string) {
        const size_t node = string == 0U ? 1U : 5U;
        const uint64_t start = PAYLOAD_START + (string == 0U ? 0U : 8U);
        append_work(work, 5U, SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH, node + 2U, start);
        append_work(work,
            string == 0U ? 4U : 6U,
            SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES,
            node + 3U,
            start + 4U);
        append_work(work,
            string == 0U ? 2U : 4U,
            SERIALIZED_FILE_VALUES_FIELD_PADDING,
            node,
            start + (string == 0U ? 7U : 9U));
        append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, node, start);
    }
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 0U, PAYLOAD_START);
}

static ExpectedWork expected_work(const ValuesFixture* fixture, size_t retained_bytes) {
    static const size_t name_lengths[NODE_COUNT][2] = {
        {9U, 4U}, {6U, 6U}, {5U, 5U}, {3U, 4U}, {4U, 4U}, {6U, 8U}, {5U, 5U}, {3U, 4U}, {4U, 4U}};
    ExpectedWork work = {0};
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < NODE_COUNT; ++node) {
        append_work(&work,
            1U,
            SERIALIZED_FILE_VALUES_FIELD_CONTEXT,
            node,
            fixture->nodes_start[0] + node * NODE_BYTES + 3U);
    }
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < NODE_COUNT; ++node) {
        const uint64_t source = fixture->nodes_start[0] + node * NODE_BYTES;
        append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, node, source);
        for (size_t name = 0U; name < 2U; ++name) {
            const SerializedFileValuesField field = name == 0U
                ? SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME
                : SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME;
            for (size_t unit = 0U; unit <= name_lengths[node][name]; ++unit) {
                append_work(&work, 1U, field, node, source + 4U + name * 4U);
            }
        }
    }
    append_payload_work(&work);
    work.planning_step = work.count;
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    work.allocation_step = work.count;
    append_work(&work, retained_bytes, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_payload_work(&work);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    return work;
}

static bool work_and_allocation_boundaries(void) {
    ValuesFixture fixture;
    fixture_init(&fixture, false, true, false, false, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    const SerializedFileValuesResult success =
        create_values(&fixture, &parents, &generous_limits, &output);
    CHECK(success.status == SERIALIZED_FILE_VALUES_OK);
    serialized_file_values_dispose(&output);
    const ExpectedWork schedule = expected_work(&fixture, success.required_retained_bytes);
    uint64_t scheduled_total = 0U;
    for (size_t step = 0U; step < schedule.count; ++step) {
        scheduled_total += schedule.steps[step].units;
    }
    CHECK(scheduled_total == success.work_used);
    for (uint64_t cap = 0U; cap < success.work_used; ++cap) {
        SerializedFileValuesLimits limits = generous_limits;
        limits.max_work = cap;
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED &&
            result.limit == SERIALIZED_FILE_VALUES_LIMIT_WORK && result.work_used <= cap &&
            output.implementation == NULL && live_allocations == 0U);
        uint64_t expected_used = 0U;
        size_t pending = 0U;
        while (schedule.steps[pending].units <= cap - expected_used) {
            expected_used += schedule.steps[pending++].units;
        }
        const ExpectedWorkStep* step = &schedule.steps[pending];
        CHECK(result.work_used == expected_used && result.field == step->field &&
            result.node_ordinal == step->node && result.error_offset == step->offset &&
            result.required_retained_bytes ==
                (pending > schedule.planning_step ? success.required_retained_bytes : 0U) &&
            result.peak_retained_bytes ==
                (pending > schedule.allocation_step ? success.required_retained_bytes : 0U));
        if (cap != 0U && cap < 12U) {
            SerializedFileSchemaContext context;
            const SerializedFileSchemaContextResult nested =
                serialized_file_schema_context_query(&parents.schema,
                    SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT,
                    cap - 1U,
                    &context);
            CHECK(result.context_attempted && same_context_result(&result.context_result, &nested));
        }
        if (cap + 1U == success.work_used) {
            CHECK(result.work_used == cap &&
                result.peak_retained_bytes == success.required_retained_bytes &&
                result.field == SERIALIZED_FILE_VALUES_FIELD_NONE &&
                result.error_offset == UINT64_MAX);
        }
    }
    SerializedFileValuesLimits exact = generous_limits;
    exact.max_work = success.work_used;
    CHECK(create_values(&fixture, &parents, &exact, &output).status == SERIALIZED_FILE_VALUES_OK);
    serialized_file_values_dispose(&output);
    allocation_calls = 0U;
    failing_allocation = 1U;
    const SerializedFileValuesResult failed =
        create_values(&fixture, &parents, &generous_limits, &output);
    failing_allocation = 0U;
    CHECK(failed.status == SERIALIZED_FILE_VALUES_ALLOCATION_FAILED && allocation_calls == 1U &&
        failed.required_retained_bytes == success.required_retained_bytes &&
        failed.peak_retained_bytes == 0U && !output.implementation && live_allocations == 0U);
    dispose_parents(&parents);
    return true;
}

static bool capacity_boundaries(void) {
    ValuesFixture fixture;
    fixture_init(&fixture, false, true, false, false, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    const SerializedFileValuesResult success =
        create_values(&fixture, &parents, &generous_limits, &output);
    CHECK(success.status == SERIALIZED_FILE_VALUES_OK);
    serialized_file_values_dispose(&output);
    const uint64_t exact_caps[] = {20U, 3U, 1U, 5U, 8U, 4U, success.required_retained_bytes};
    for (size_t kind = 0U; kind < sizeof(exact_caps) / sizeof(exact_caps[0]); ++kind) {
        for (size_t bound = 0U; bound < 3U; ++bound) {
            uint64_t cap = exact_caps[kind];
            if (bound == 0U) {
                cap = 0U;
            } else if (bound == 1U) {
                --cap;
            }
            SerializedFileValuesLimits limits = generous_limits;
            SerializedFileValuesLimit expected = SERIALIZED_FILE_VALUES_LIMIT_NONE;
            const bool exact = bound == 2U;
            switch (kind) {
            case 0U:
                limits.max_payload_bytes = cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_PAYLOAD_BYTES;
                break;
            case 1U:
                limits.max_values = (size_t)cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_VALUES;
                break;
            case 2U:
                limits.max_depth = (size_t)cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_DEPTH;
                break;
            case 3U:
                limits.max_string_bytes = cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_STRING_BYTES;
                break;
            case 4U:
                limits.max_total_string_bytes = cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_TOTAL_STRING_BYTES;
                break;
            case 5U:
                limits.max_padding_bytes = cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_PADDING_BYTES;
                break;
            default:
                limits.max_retained_bytes = (size_t)cap;
                expected = SERIALIZED_FILE_VALUES_LIMIT_RETAINED_BYTES;
                break;
            }
            const SerializedFileValuesResult result =
                create_values(&fixture, &parents, &limits, &output);
            CHECK(result.status ==
                (exact ? SERIALIZED_FILE_VALUES_OK : SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED));
            CHECK(result.limit == (exact ? SERIALIZED_FILE_VALUES_LIMIT_NONE : expected));
            if (!exact) {
                CHECK(!output.implementation && result.peak_retained_bytes == 0U);
            }
            serialized_file_values_dispose(&output);
        }
    }
    dispose_parents(&parents);
    return true;
}

static bool string_extents_and_empty(void) {
    for (size_t cut = 0U; cut < 20U; ++cut) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &generous_limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT &&
            result.error_offset == PAYLOAD_START + cut && !output.implementation);
        SerializedFileValuesField expected = SERIALIZED_FILE_VALUES_FIELD_PADDING;
        if (cut < 4U || (cut >= 8U && cut < 12U)) {
            expected = SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH;
        } else if (cut < 7U || (cut >= 12U && cut < 17U)) {
            expected = SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES;
        }
        CHECK(result.field == expected && result.peak_retained_bytes == 0U);
        dispose_parents(&parents);
    }
    for (size_t length_word = 0U; length_word < 2U; ++length_word) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, false);
        const size_t offset = PAYLOAD_START + (length_word == 0U ? 0U : 8U);
        write_integer(fixture.bytes + offset, 4U, UINT32_C(0x80000000), false);
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &generous_limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_STRING_LENGTH &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH &&
            result.error_offset == offset && result.node_ordinal == (length_word == 0U ? 3U : 7U) &&
            !output.implementation);
        dispose_parents(&parents);
    }
    ValuesFixture fixture;
    fixture_init(&fixture, false, true, false, false, false);
    fixture_payload(&fixture, NULL, 0U, NULL, 0U);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValuesLimits limits = generous_limits;
    limits.max_string_bytes = 0U;
    limits.max_total_string_bytes = 0U;
    limits.max_padding_bytes = 0U;
    SerializedFileValues output = {0};
    CHECK(create_values(&fixture, &parents, &limits, &output).status == SERIALIZED_FILE_VALUES_OK);
    for (size_t ordinal = 1U; ordinal < 3U; ++ordinal) {
        const SerializedFileValue* value = serialized_file_values_value(&output, ordinal);
        const size_t end = PAYLOAD_START + ordinal * 4U;
        CHECK(value->length_bits == 0U && span_is(&fixture, value->bytes_source, end, 0U) &&
            span_is(&fixture, value->padding_source, end, 0U));
    }
    serialized_file_values_dispose(&output);
    dispose_parents(&parents);
    fixture_init(&fixture, false, true, false, false, false);
    fixture.bytes[fixture.file_size++] = 0x77U;
    write_integer(fixture.bytes + 24U, 8U, fixture.file_size, true);
    write_integer(fixture.bytes + fixture.object_start + 16U, 4U, 21U, false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult trailing =
        create_values(&fixture, &parents, &generous_limits, &output);
    CHECK(trailing.status == SERIALIZED_FILE_VALUES_TRAILING_OBJECT_BYTES &&
        trailing.error_offset == PAYLOAD_START + 20U && !output.implementation);
    dispose_parents(&parents);
    return true;
}

static bool object_cursor_domain(void) {
    const uint32_t payload_sizes[] = {
        (uint32_t)INT32_MAX - 1U, (uint32_t)INT32_MAX, (uint32_t)INT32_MAX + 1U};
    for (size_t sample = 0U; sample < sizeof(payload_sizes) / sizeof(payload_sizes[0]); ++sample) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, false);
        const uint32_t payload_size = payload_sizes[sample];
        fixture.file_size = PAYLOAD_START + (size_t)payload_size;
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, payload_size, false);
        write_integer(fixture.bytes + 24U, 8U, fixture.file_size, true);
        const ValuesFixture original = fixture;
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        /* Only the small real metadata prefix is mapped. The directory holds
         * genuine logical ranges; no large backing allocation or file exists. */
        const SerializedFileValuesResult result =
            serialized_file_values_create_text_asset(&parents.directory,
                &parents.schema,
                0U,
                fixture.bytes,
                fixture.metadata_end,
                &generous_limits,
                &output);
        const bool unsupported = payload_size > INT32_MAX;
        const SerializedFileValuesStatus expected = unsupported
            ? SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_SIZE
            : SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING;
        CHECK(result.status == expected && result.limit == SERIALIZED_FILE_VALUES_LIMIT_NONE &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_OBJECT && result.object_ordinal == 0U &&
            result.type_ordinal == 0U && result.node_ordinal == SIZE_MAX &&
            result.work_used == 1U &&
            result.error_offset ==
                (unsupported ? fixture.object_start + 16U : fixture.metadata_end) &&
            !result.context_attempted && result.required_retained_bytes == 0U &&
            result.peak_retained_bytes == 0U && !output.implementation);
        if (unsupported) {
            SerializedFileSchema empty_schema = {0};
            SerializedFileValuesLimits limits = generous_limits;
            limits.max_payload_bytes = 0U;
            const SerializedFileValuesResult before_schema_and_caps =
                serialized_file_values_create_text_asset(&parents.directory,
                    &empty_schema,
                    0U,
                    fixture.bytes,
                    fixture.metadata_end,
                    &limits,
                    &output);
            CHECK(before_schema_and_caps.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_SIZE &&
                before_schema_and_caps.limit == SERIALIZED_FILE_VALUES_LIMIT_NONE &&
                before_schema_and_caps.work_used == 1U && !output.implementation);
            const SerializedFileValuesResult wrong_backing =
                serialized_file_values_create_text_asset(&parents.directory,
                    &parents.schema,
                    0U,
                    fixture.bytes + 1U,
                    fixture.metadata_end - 1U,
                    &generous_limits,
                    &output);
            CHECK(wrong_backing.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH &&
                wrong_backing.work_used == 1U && !output.implementation);
        }
        CHECK(memcmp(&fixture, &original, sizeof(fixture)) == 0);
        dispose_parents(&parents);
    }
    return true;
}

static bool string_alignment_residues(void) {
    static const uint8_t first[] = {0U, 0xffU, 0x80U, 'A', 'B', 'C', 'D'};
    static const uint8_t second[] = {'z', 0U, 0xfeU, 0x81U, 'Y', 'X', 'W'};
    for (size_t first_size = 0U; first_size <= sizeof(first); ++first_size) {
        for (size_t second_size = 0U; second_size <= sizeof(second); ++second_size) {
            ValuesFixture fixture;
            fixture_init(&fixture, false, true, false, false, false);
            fixture_payload(&fixture, first, first_size, second, second_size);
            const ValuesFixture original = fixture;
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            const SerializedFileValuesResult result =
                create_values(&fixture, &parents, &generous_limits, &output);
            CHECK(result.status == SERIALIZED_FILE_VALUES_OK);
            const size_t lengths[] = {first_size, second_size};
            const uint8_t* const expected_bytes[] = {first, second};
            size_t expected_offset = PAYLOAD_START;
            size_t expected_padding = 0U;
            for (size_t index = 0U; index < 2U; ++index) {
                const size_t length = lengths[index];
                const size_t padding = (4U - length % 4U) % 4U;
                const SerializedFileValue* value =
                    serialized_file_values_value(&output, index + 1U);
                CHECK(value && value->length_bits == length &&
                    span_is(&fixture, value->length_source, expected_offset, 4U) &&
                    span_is(&fixture, value->bytes_source, expected_offset + 4U, length) &&
                    span_is(
                        &fixture, value->padding_source, expected_offset + 4U + length, padding) &&
                    span_is(&fixture, value->source, expected_offset, 4U + length + padding));
                CHECK(memcmp(value->bytes_source.data, expected_bytes[index], length) == 0);
                for (size_t byte = 0U; byte < padding; ++byte) {
                    CHECK(value->padding_source.data[byte] == 0xa5U);
                }
                expected_offset += 4U + length + padding;
                expected_padding += padding;
            }
            const SerializedFileValuesView* view = serialized_file_values_view(&output);
            CHECK(view && view->consumed_bytes == expected_offset - PAYLOAD_START &&
                view->string_bytes == first_size + second_size &&
                view->padding_bytes == expected_padding);
            CHECK(expected_offset == fixture.file_size &&
                result.work_used ==
                    result.required_retained_bytes + PROFILE_WORK +
                        2U * (view->consumed_bytes + 9U) + 3U);
            CHECK(memcmp(&fixture, &original, sizeof(fixture)) == 0);
            serialized_file_values_dispose(&output);
            dispose_parents(&parents);
        }
    }
    return true;
}

static bool object_end_guard(void) {
#ifndef _WIN32
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > PAYLOAD_START + 20);
    for (size_t cut = 0U; cut <= 20U; ++cut) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        const size_t mapped_size = PAYLOAD_START + cut;
        const size_t mapping_size = 2U * (size_t)page_size;
        uint8_t* mapping =
            mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(mapping != MAP_FAILED);
        uint8_t* bytes = mapping + (size_t)page_size - mapped_size;
        memcpy(bytes, fixture.bytes, mapped_size);
        CHECK(mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) == 0);
        CHECK(mprotect(mapping, (size_t)page_size, PROT_READ) == 0);
        ValuesParents parents = {0};
        CHECK(prepare_parents_from_bytes(&fixture, bytes, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result = serialized_file_values_create_text_asset(
            &parents.directory, &parents.schema, 0U, bytes, mapped_size, &generous_limits, &output);
        const SerializedFileValuesStatus expected =
            cut == 20U ? SERIALIZED_FILE_VALUES_OK : SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT;
        CHECK(result.status == expected);
        if (cut != 20U) {
            CHECK(result.error_offset == mapped_size && !output.implementation);
        }
        serialized_file_values_dispose(&output);
        dispose_parents(&parents);
        CHECK(munmap(mapping, mapping_size) == 0);
    }
#endif
    return true;
}

static bool profile_refusals(void) {
    /* Each mutation precedes construction of both genuine immutable parents. */
    for (size_t mutation = 0U; mutation < 14U; ++mutation) {
        ValuesFixture fixture;
        fixture_init(&fixture, false, true, false, false, false);
        uint8_t* node = fixture.bytes + fixture.nodes_start[0] + NODE_BYTES;
        SerializedFileValuesStatus expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA;
        switch (mutation) {
        case 0U:
            node[0] = 2U;
            break;
        case 1U:
            node[3] = 0x10U;
            break;
        case 2U:
            node[12] = 4U;
            break;
        case 3U:
            node[16] = 99U;
            break;
        case 4U:
            node[20] ^= 1U;
            break;
        case 5U:
            node[24] = 1U;
            break;
        case 6U:
            fixture.bytes[fixture.strings_start[0]] = 'x';
            break;
        case 7U:
            write_integer(node + 8U, 4U, UINT32_C(0x80000491), false);
            break; /* The exact common table's terminal empty name. */
        case 8U:
            fixture.bytes[fixture.type_start[0]] = 50U;
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE;
            break;
        case 9U:
            fixture.bytes[fixture.type_start[0] + 4U] = 1U;
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE;
            break;
        case 10U:
            node[3] = 5U;
            expected = SERIALIZED_FILE_VALUES_CONTEXT_REJECTED;
            break;
        case 11U:
            node[3] = 4U;
            expected = SERIALIZED_FILE_VALUES_CONTEXT_REJECTED;
            break;
        case 12U:
            /* A structurally admitted final-child registry remains outside
             * this TextAsset value profile even when context query succeeds. */
            fixture.bytes[fixture.nodes_start[0] + 5U * NODE_BYTES + 3U] = 4U;
            break;
        default:
            /* Valid preorder, different size-node parent and array boundary. */
            fixture.bytes[fixture.nodes_start[0] + 3U * NODE_BYTES + 2U] = 2U;
            break;
        }
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &generous_limits, &output);
        CHECK(result.status == expected && !output.implementation &&
            result.peak_retained_bytes == 0U);
        if (expected == SERIALIZED_FILE_VALUES_CONTEXT_REJECTED) {
            SerializedFileSchemaContext context;
            const SerializedFileSchemaContextResult nested =
                serialized_file_schema_context_query(&parents.schema,
                    SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT,
                    generous_limits.max_work - 1U,
                    &context);
            CHECK(result.context_attempted && same_context_result(&result.context_result, &nested));
        }
        dispose_parents(&parents);
    }
    return true;
}

static bool aliases_leave_inputs_unchanged(void) {
    ValuesFixture fixture;
    fixture_init(&fixture, false, true, false, false, true);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const ValuesFixture original = fixture;
    const SerializedFileDirectoryView directory_view =
        *serialized_file_directory_view(&parents.directory);
    const SerializedFileSchemaView schema_view = *serialized_file_schema_view(&parents.schema);
    const SerializedFileSchemaNode schema_node = *serialized_file_schema_node(&parents.schema, 0U);
    void* const directory_storage = parents.directory.implementation;
    void* const schema_storage = parents.schema.implementation;
    const uint8_t* common_name = serialized_file_schema_node(&parents.schema, 0U)->type_name.bytes;
    const size_t output_alignment = _Alignof(SerializedFileValues);
    const size_t common_adjustment =
        (output_alignment - (uintptr_t)common_name % output_alignment) % output_alignment;
    const void* const aliases[] = {&parents.directory,
        &parents.schema,
        serialized_file_directory_view(&parents.directory),
        serialized_file_schema_view(&parents.schema),
        serialized_file_schema_node(&parents.schema, NODE_COUNT - 1U),
        common_name + common_adjustment,
        &generous_limits,
        fixture.bytes + 128U,
        fixture.bytes + PAYLOAD_START + 8U};
    for (size_t alias = 0U; alias < sizeof(aliases) / sizeof(aliases[0]); ++alias) {
        /* Common promises to reject intersecting accessible ranges before an
         * output dereference; these are never treated as forged live owners. */
        SerializedFileValues* output = (SerializedFileValues*)(void*)aliases[alias];
        const SerializedFileValuesResult result =
            create_values(&fixture, &parents, &generous_limits, output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT && result.work_used == 0U &&
            result.peak_retained_bytes == 0U &&
            parents.directory.implementation == directory_storage &&
            parents.schema.implementation == schema_storage);
        CHECK(memcmp(&fixture, &original, sizeof(fixture)) == 0 &&
            memcmp(&directory_view,
                serialized_file_directory_view(&parents.directory),
                sizeof(directory_view)) == 0 &&
            memcmp(&schema_view,
                serialized_file_schema_view(&parents.schema),
                sizeof(schema_view)) == 0 &&
            memcmp(&schema_node,
                serialized_file_schema_node(&parents.schema, 0U),
                sizeof(schema_node)) == 0);
    }
    SerializedFileValues output = {0};
    const SerializedFileValuesLimits* overlapping_limits =
        (const SerializedFileValuesLimits*)(const void*)(fixture.bytes + PAYLOAD_START);
    const SerializedFileValuesResult result =
        create_values(&fixture, &parents, overlapping_limits, &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT && result.work_used == 0U &&
        !output.implementation && memcmp(&fixture, &original, sizeof(fixture)) == 0);
    dispose_parents(&parents);
    return true;
}

static bool disposal_does_not_read_backing(void) {
#ifndef _WIN32
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    const size_t mapping_size =
        ((sizeof(ValuesFixture) + (size_t)page_size - 1U) / (size_t)page_size) * (size_t)page_size;
    void* mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapping != MAP_FAILED);
    ValuesFixture* fixture = mapping;
    fixture_init(fixture, false, true, false, false, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    CHECK(create_values(fixture, &parents, &generous_limits, &output).status ==
        SERIALIZED_FILE_VALUES_OK);
    CHECK(mprotect(mapping, mapping_size, PROT_NONE) == 0);
    dispose_parents(&parents);
    serialized_file_values_dispose(&output);
    serialized_file_values_dispose(&output);
    CHECK(!output.implementation && live_allocations == 0U);
    CHECK(munmap(mapping, mapping_size) == 0);
#endif
    return true;
}

static bool sources_and_states(void) {
    ValuesFixture fixture;
    fixture_init(&fixture, false, true, false, true, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    ValuesFixture other_fixture = fixture;
    ValuesParents other_parents = {0};
    CHECK(prepare_parents(&other_fixture, 0U, false, &other_parents));
    SerializedFileValuesResult result = serialized_file_values_create_text_asset(&parents.directory,
        &other_parents.schema,
        0U,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH && !result.context_attempted);
    result = serialized_file_values_create_text_asset(&parents.directory,
        &parents.schema,
        0U,
        other_fixture.bytes,
        other_fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH && !output.implementation);
    dispose_parents(&other_parents);
    CHECK(prepare_parents(&fixture, 1U, false, &other_parents));
    result = serialized_file_values_create_text_asset(&parents.directory,
        &other_parents.schema,
        0U,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH && !result.context_attempted);
    dispose_parents(&other_parents);
    CHECK(prepare_parents(&fixture, 0U, true, &other_parents));
    result = serialized_file_values_create_text_asset(&parents.directory,
        &other_parents.schema,
        0U,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH && !result.context_attempted);
    dispose_parents(&other_parents);
    for (size_t mapped_size = 0U; mapped_size < fixture.file_size; ++mapped_size) {
        result = serialized_file_values_create_text_asset(&parents.directory,
            &parents.schema,
            0U,
            fixture.bytes,
            mapped_size,
            &generous_limits,
            &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING &&
            result.error_offset == mapped_size && !output.implementation);
    }
    result = serialized_file_values_create_text_asset(&parents.directory,
        &parents.schema,
        0U,
        fixture.bytes,
        fixture.file_size + 1U,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    result = serialized_file_values_create_text_asset(&parents.directory,
        &parents.schema,
        SIZE_MAX,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT &&
        result.object_ordinal == SIZE_MAX);
    SerializedFileDirectory empty_directory = {0};
    SerializedFileSchema empty_schema = {0};
    result = serialized_file_values_create_text_asset(&empty_directory,
        &parents.schema,
        0U,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_STATE && result.work_used == 0U);
    result = serialized_file_values_create_text_asset(&parents.directory,
        &empty_schema,
        0U,
        fixture.bytes,
        fixture.file_size,
        &generous_limits,
        &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_STATE && result.work_used == 1U);
    for (size_t missing = 0U; missing < 5U; ++missing) {
        result = serialized_file_values_create_text_asset(missing == 0U ? NULL : &parents.directory,
            missing == 1U ? NULL : &parents.schema,
            0U,
            missing == 2U ? NULL : fixture.bytes,
            fixture.file_size,
            missing == 3U ? NULL : &generous_limits,
            missing == 4U ? NULL : &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT && result.work_used == 0U);
    }
    CHECK(create_values(&fixture, &parents, &generous_limits, &output).status ==
        SERIALIZED_FILE_VALUES_OK);
    void* live = output.implementation;
    const SerializedFileValuesView saved_view = *serialized_file_values_view(&output);
    const SerializedFileValue saved_value = *serialized_file_values_value(&output, 1U);
    result = create_values(&fixture, &parents, &generous_limits, &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_STATE && result.work_used == 0U &&
        output.implementation == live &&
        memcmp(&saved_view, serialized_file_values_view(&output), sizeof(saved_view)) == 0 &&
        memcmp(&saved_value, serialized_file_values_value(&output, 1U), sizeof(saved_value)) == 0);
    serialized_file_values_dispose(&output);
    dispose_parents(&parents);
    for (size_t alternative = 0U; alternative < 3U; ++alternative) {
        fixture_init(
            &fixture, alternative == 0U, alternative != 2U, alternative == 1U, false, false);
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        result = create_values(&fixture, &parents, &generous_limits, &output);
        const SerializedFileValuesStatus expected_statuses[] = {
            SERIALIZED_FILE_VALUES_UNSUPPORTED_ENDIAN,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_ENGINE,
            SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA};
        const SerializedFileValuesStatus expected = expected_statuses[alternative];
        CHECK(result.status == expected && result.work_used == 1U && !result.context_attempted &&
            !output.implementation);
        dispose_parents(&parents);
    }
    fixture_init(&fixture, false, true, false, false, false);
    write_integer(fixture.bytes + fixture.object_start + 8U, 8U, 1U, false);
    write_integer(fixture.bytes + fixture.object_start + 16U, 4U, 19U, false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    result = create_values(&fixture, &parents, &generous_limits, &output);
    CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_ALIGNMENT &&
        result.error_offset == PAYLOAD_START + 1U && !output.implementation);
    dispose_parents(&parents);
    return true;
}

enum {
    ORDINARY_NODE_COUNT = 13,
    ORDINARY_VALUE_COUNT = 10,
    ORDINARY_PROFILE_WORK = 227
};

static const char* const ordinary_types[ORDINARY_NODE_COUNT] = {"MonoBehaviour",
    "PPtr<GameObject>",
    "int",
    "SInt64",
    "UInt8",
    "PPtr<MonoScript>",
    "int",
    "SInt64",
    "string",
    "Array",
    "int",
    "char",
    "int"};
static const char* const ordinary_fields[ORDINARY_NODE_COUNT] = {"Base",
    "m_GameObject",
    "m_FileID",
    "m_PathID",
    "m_Enabled",
    "m_Script",
    "m_FileID",
    "m_PathID",
    "m_Name",
    "Array",
    "size",
    "data",
    NULL};
static const uint8_t ordinary_levels[ORDINARY_NODE_COUNT] = {
    0U, 1U, 2U, 2U, 1U, 1U, 2U, 2U, 1U, 2U, 3U, 3U, 1U};
static const uint32_t ordinary_widths[ORDINARY_NODE_COUNT] = {
    UINT32_MAX, 12U, 4U, 8U, 1U, 12U, 4U, 8U, UINT32_MAX, UINT32_MAX, 4U, 1U, 4U};
static const uint32_t ordinary_flags[ORDINARY_NODE_COUNT] = {0x8000U,
    0x41U,
    0x41U,
    0x41U,
    0x4101U,
    0U,
    0x800001U,
    0x800001U,
    0x88001U,
    0x84001U,
    0x80001U,
    0x80001U,
    0U};

/* Synthetic policy fixtures reuse only the physical header/tail plumbing. Their
 * independent thirteen-row wire table is not a generated production profile. */
static size_t write_ordinary_type(
    ValuesFixture* fixture, size_t offset, size_t ordinal, const char* field) {
    fixture->type_start[ordinal] = offset;
    write_integer(fixture->bytes + offset, 4U, 114U, fixture->big_endian);
    write_integer(
        fixture->bytes + offset + 5U, 2U, ordinal == 0U ? 17U : 31999U, fixture->big_endian);
    offset += 7U;
    for (size_t byte = 0U; byte < 32U; ++byte) {
        fixture->bytes[offset + byte] = (uint8_t)(0xe0U + byte + ordinal);
    }
    offset += 32U;
    if (!fixture->tree_enabled) {
        return offset;
    }
    write_integer(fixture->bytes + offset, 4U, ORDINARY_NODE_COUNT, fixture->big_endian);
    const size_t string_count = offset + 4U;
    fixture->nodes_start[ordinal] = offset + 8U;
    fixture->strings_start[ordinal] = offset + 8U + ORDINARY_NODE_COUNT * NODE_BYTES;
    size_t string_bytes = 0U;
    for (size_t index = 0U; index < ORDINARY_NODE_COUNT; ++index) {
        uint8_t* node = fixture->bytes + fixture->nodes_start[ordinal] + index * NODE_BYTES;
        write_integer(node, 2U, 1U, fixture->big_endian);
        node[2U] = ordinary_levels[index];
        node[3U] = index == 9U ? 1U : 0U;
        const char* names[] = {
            ordinary_types[index], index == 12U ? field : ordinary_fields[index]};
        for (size_t name = 0U; name < 2U; ++name) {
            write_integer(node + 4U + name * 4U, 4U, string_bytes, fixture->big_endian);
            const size_t length = strlen(names[name]) + 1U;
            memcpy(fixture->bytes + fixture->strings_start[ordinal] + string_bytes,
                names[name],
                length);
            string_bytes += length;
        }
        write_integer(node + 12U, 4U, ordinary_widths[index], fixture->big_endian);
        write_integer(node + 16U, 4U, index, fixture->big_endian);
        write_integer(node + 20U, 4U, ordinary_flags[index], fixture->big_endian);
    }
    write_integer(fixture->bytes + string_count, 4U, string_bytes, fixture->big_endian);
    offset = fixture->strings_start[ordinal] + string_bytes;
    write_integer(fixture->bytes + offset, 4U, 0U, fixture->big_endian);
    return offset + 4U;
}

static void ordinary_payload(
    ValuesFixture* fixture, const uint64_t bits[6], const uint8_t* name, size_t name_size) {
    static const size_t offsets[] = {0U, 4U, 12U, 16U, 20U};
    static const size_t widths[] = {4U, 8U, 1U, 4U, 8U};
    for (size_t index = 0U; index < 5U; ++index) {
        write_integer(fixture->bytes + fixture->payload_start + offsets[index],
            widths[index],
            bits[index],
            fixture->big_endian);
    }
    memset(fixture->bytes + fixture->payload_start + 13U, 0xa5, 3U);
    size_t end = write_string(fixture, fixture->payload_start + 28U, name, name_size);
    write_integer(fixture->bytes + end, 4U, bits[5], fixture->big_endian);
    end += 4U;
    fixture->file_size = end;
    write_integer(fixture->bytes + fixture->object_start + 16U,
        4U,
        end - fixture->payload_start,
        fixture->big_endian);
    write_integer(fixture->bytes + 24U, 8U, end, true);
}

static void ordinary_fixture_init(
    ValuesFixture* fixture, const char* field, bool big_endian, bool tree, bool exact29) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->big_endian = big_endian;
    fixture->tree_enabled = tree;
    fixture->exact29 = exact29;
    fixture->payload_start = PAYLOAD_START;
    uint8_t* bytes = fixture->bytes;
    memcpy(bytes + 48U, exact29 ? "2021.3.29f1" : "2021.3.35f1", 12U);
    bytes[64U] = tree ? 1U : 0U;
    write_integer(bytes + 65U, 4U, TYPE_COUNT, big_endian);
    size_t offset = 69U;
    for (size_t ordinal = 0U; ordinal < TYPE_COUNT; ++ordinal) {
        offset = write_ordinary_type(fixture, offset, ordinal, field);
    }
    write_integer(bytes + offset, 4U, 1U, big_endian);
    offset += 4U;
    while (offset % 4U != 0U) {
        bytes[offset] = 0xdbU;
        ++offset;
    }
    fixture->object_start = offset;
    write_integer(bytes + offset, 8U, UINT64_C(0x8123456789abcdef), big_endian);
    offset += 24U;
    offset += 12U; /* Zero script/external/reference counts in this synthetic file. */
    bytes[offset] = 0U;
    ++offset;
    fixture->metadata_end = offset;
    write_integer(bytes + 8U, 4U, 22U, true);
    write_integer(bytes + 16U, 8U, offset - 48U, true);
    write_integer(bytes + 32U, 8U, PAYLOAD_START, true);
    bytes[40U] = big_endian ? 1U : 0U;
    const uint64_t bits[] = {0U, 1U, 0xbeU, 2U, UINT64_C(0x0123456789abcdef), UINT32_C(0xfedcba98)};
    static const uint8_t name[] = {0xffU, 0U, 0x80U};
    ordinary_payload(fixture, bits, name, sizeof(name));
}

static SerializedFileValuesLimits ordinary_limits(void) {
    SerializedFileValuesLimits limits = generous_limits;
    limits.max_values = ORDINARY_VALUE_COUNT;
    limits.max_depth = 2U;
    limits.max_integer_bytes = 8U;
    limits.max_total_integer_bytes = 29U;
    return limits;
}

static SerializedFileValuesResult create_ordinary(const ValuesFixture* fixture,
    const ValuesParents* parents,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* output) {
    return serialized_file_values_create_ordinary(&parents->directory,
        &parents->schema,
        0U,
        fixture->bytes,
        fixture->file_size,
        limits,
        output);
}

static bool check_ordinary(const ValuesFixture* fixture,
    const SerializedFileValues* values,
    const uint64_t bits[6],
    const uint8_t* name,
    size_t name_size,
    const char* final_field) {
    const SerializedFileValuesView* view = serialized_file_values_view(values);
    const size_t string_size = 4U + name_size + (4U - name_size % 4U) % 4U;
    const size_t payload_size = 32U + string_size;
    CHECK(view && view->value_count == ORDINARY_VALUE_COUNT && view->maximum_depth == 2U &&
        view->integer_bytes == 29U && view->string_bytes == name_size &&
        view->consumed_bytes == payload_size &&
        view->padding_bytes == 3U + (4U - name_size % 4U) % 4U);
    CHECK(view->object.path_id_bits == UINT64_C(0x8123456789abcdef) &&
        view->schema.class_id_bits == 114U && view->schema.has_script_hash &&
        view->schema.script_index_bits == 17U && view->schema.node_count == ORDINARY_NODE_COUNT);
    CHECK(view->context.kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT &&
        view->context.registry == SIZE_MAX && view->context.registry_end == SIZE_MAX &&
        !view->context.registry_omitted && view->context.traversal_end == ORDINARY_NODE_COUNT);
    const size_t parents[] = {SIZE_MAX, 0U, 1U, 1U, 0U, 0U, 5U, 5U, 0U, 0U};
    const size_t children[] = {
        1U, 2U, SIZE_MAX, SIZE_MAX, SIZE_MAX, 6U, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    const size_t siblings[] = {SIZE_MAX, 4U, 3U, SIZE_MAX, 5U, 8U, 7U, SIZE_MAX, 9U, SIZE_MAX};
    const size_t ends[] = {10U, 4U, 3U, 4U, 5U, 8U, 7U, 8U, 9U, 10U};
    const size_t offsets[] = {0U, 0U, 0U, 4U, 12U, 16U, 16U, 20U, 28U, 28U + string_size};
    const size_t sizes[] = {payload_size, 12U, 4U, 8U, 4U, 12U, 4U, 8U, string_size, 4U};
    const size_t bit_indices[] = {SIZE_MAX, SIZE_MAX, 0U, 1U, 2U, SIZE_MAX, 3U, 4U, SIZE_MAX, 5U};
    for (size_t ordinal = 0U; ordinal < ORDINARY_VALUE_COUNT; ++ordinal) {
        const size_t node = ordinal == 9U ? 12U : ordinal;
        const SerializedFileValue* value = serialized_file_values_value(values, ordinal);
        CHECK(value && value->ordinal == ordinal && value->schema_node.ordinal == node &&
            value->parent == parents[ordinal] && value->first_child == children[ordinal] &&
            value->next_sibling == siblings[ordinal] && value->subtree_end == ends[ordinal]);
        size_t child_count = 0U;
        if (ordinal == 0U) {
            child_count = 5U;
        } else if (ordinal == 1U || ordinal == 5U) {
            child_count = 2U;
        }
        CHECK(value->child_count == child_count &&
            span_is(fixture, value->source, PAYLOAD_START + offsets[ordinal], sizes[ordinal]));
        CHECK(span_is(fixture,
            value->schema_node.source,
            fixture->nodes_start[0] + node * NODE_BYTES,
            NODE_BYTES));
        if (ordinal == 8U) {
            CHECK(value->kind == SERIALIZED_FILE_VALUE_BYTE_STRING &&
                value->length_bits == name_size && value->array_schema_ordinal == 9U &&
                value->size_schema_ordinal == 10U && value->data_schema_ordinal == 11U &&
                value->integer_bits == 0U && span_absent(value->integer_source));
            CHECK(span_is(fixture, value->length_source, PAYLOAD_START + 28U, 4U) &&
                span_is(fixture, value->bytes_source, PAYLOAD_START + 32U, name_size) &&
                span_is(fixture,
                    value->padding_source,
                    PAYLOAD_START + 32U + name_size,
                    string_size - name_size - 4U));
            CHECK(!name_size || memcmp(value->bytes_source.data, name, name_size) == 0);
            continue;
        }
        CHECK(value->array_schema_ordinal == SIZE_MAX && value->size_schema_ordinal == SIZE_MAX &&
            value->data_schema_ordinal == SIZE_MAX && span_absent(value->array_schema_source) &&
            span_absent(value->size_schema_source) && span_absent(value->data_schema_source) &&
            value->length_bits == 0U && span_absent(value->length_source) &&
            span_absent(value->bytes_source));
        if (bit_indices[ordinal] == SIZE_MAX) {
            CHECK(value->kind == SERIALIZED_FILE_VALUE_CONTAINER && value->integer_bits == 0U &&
                span_absent(value->integer_source) && span_absent(value->padding_source));
        } else {
            const size_t width = ordinal == 4U ? 1U : sizes[ordinal];
            CHECK(value->kind ==
                (ordinal == 4U ? SERIALIZED_FILE_VALUE_UNSIGNED_INTEGER
                               : SERIALIZED_FILE_VALUE_SIGNED_INTEGER));
            CHECK(value->integer_bits == bits[bit_indices[ordinal]] &&
                span_is(fixture, value->integer_source, PAYLOAD_START + offsets[ordinal], width));
            if (ordinal == 4U) {
                CHECK(span_is(fixture, value->padding_source, PAYLOAD_START + 13U, 3U));
                CHECK(value->padding_source.data[0] == 0xa5U &&
                    value->padding_source.data[1] == 0xa5U &&
                    value->padding_source.data[2] == 0xa5U);
            } else {
                CHECK(span_absent(value->padding_source));
            }
        }
    }
    const SerializedFileValue* authored = serialized_file_values_value(values, 9U);
    CHECK(authored->schema_node.field_name.byte_count == strlen(final_field) &&
        memcmp(authored->schema_node.field_name.bytes, final_field, strlen(final_field) + 1U) == 0);
    CHECK(!serialized_file_values_value(values, ORDINARY_VALUE_COUNT) &&
        !serialized_file_values_value(values, SIZE_MAX));
    return true;
}

static bool ordinary_scalar_bits_and_lifetime(void) {
    const char* fields[] = {"counter", "score_total", "\xce\xb1", "", "\xff"};
    static const uint64_t samples[][6] = {{0U, 0U, 0U, 0U, 0U, 0U},
        {1U, 1U, 1U, 1U, 1U, 1U},
        {UINT32_C(0x7fffffff),
            UINT64_C(0x7fffffffffffffff),
            0x7fU,
            UINT32_C(0x7fffffff),
            UINT64_C(0x7fffffffffffffff),
            UINT32_C(0x7fffffff)},
        {UINT32_C(0x80000000),
            UINT64_C(0x8000000000000000),
            0x80U,
            UINT32_C(0x80000000),
            UINT64_C(0x8000000000000000),
            UINT32_C(0x80000000)},
        {UINT32_MAX, UINT64_MAX, 0xffU, UINT32_MAX, UINT64_MAX, UINT32_MAX},
        {UINT32_C(0xfedcba98),
            UINT64_C(0xfedcba9876543210),
            0xbeU,
            UINT32_C(0x89abcdef),
            UINT64_C(0x0123456789abcdef),
            UINT32_C(0xa5a55a5a)}};
    static const uint8_t name[] = {0xffU, 0U, 0x80U};
    const SerializedFileValuesLimits limits = ordinary_limits();
    for (size_t field = 0U; field < sizeof(fields) / sizeof(fields[0]); ++field) {
        for (size_t sample = 0U; sample < sizeof(samples) / sizeof(samples[0]); ++sample) {
            ValuesFixture fixture;
            ordinary_fixture_init(&fixture, fields[field], false, true, false);
            ordinary_payload(&fixture, samples[sample], name, sizeof(name));
            const ValuesFixture original = fixture;
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            allocation_calls = 0U;
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            CHECK(result.status == SERIALIZED_FILE_VALUES_OK &&
                result.work_used ==
                    result.required_retained_bytes + ORDINARY_PROFILE_WORK + 2U * (40U + 20U) +
                        3U &&
                result.context_attempted && result.context_result.work_used == 15U &&
                allocation_calls == 1U && live_allocations == 1U);
            const SerializedFileValuesView* view = serialized_file_values_view(&output);
            CHECK(view && result.required_retained_bytes == last_allocation_bytes &&
                result.peak_retained_bytes == last_allocation_bytes &&
                view->retained_bytes == last_allocation_bytes);
            CHECK(check_ordinary(
                &fixture, &output, samples[sample], name, sizeof(name), fields[field]));
            const SerializedFileValue copied = *serialized_file_values_value(&output, 7U);
            dispose_parents(&parents);
            CHECK(check_ordinary(
                &fixture, &output, samples[sample], name, sizeof(name), fields[field]));
            serialized_file_values_dispose(&output);
            CHECK(copied.integer_bits == samples[sample][4] &&
                span_is(&fixture, copied.integer_source, PAYLOAD_START + 20U, 8U));
            CHECK(memcmp(&fixture, &original, sizeof(fixture)) == 0 && live_allocations == 0U);
        }
    }
    return true;
}

static void append_ordinary_prefix_work(ExpectedWork* work) {
    const size_t nodes[] = {2U, 3U, 4U, 6U, 7U};
    const size_t offsets[] = {0U, 4U, 12U, 16U, 20U};
    const size_t widths[] = {4U, 8U, 1U, 4U, 8U};
    for (size_t index = 0U; index < 5U; ++index) {
        const size_t node = nodes[index];
        append_work(work,
            widths[index] + 1U,
            SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES,
            node,
            PAYLOAD_START + offsets[index]);
        if (node == 4U) {
            append_work(work, 4U, SERIALIZED_FILE_VALUES_FIELD_PADDING, node, PAYLOAD_START + 13U);
        }
        append_work(work,
            1U,
            SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
            node,
            PAYLOAD_START + offsets[index]);
        if (node == 3U || node == 7U) {
            append_work(work,
                1U,
                SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
                node - 2U,
                PAYLOAD_START + (node == 3U ? 0U : 16U));
        }
    }
    append_work(work, 5U, SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH, 10U, PAYLOAD_START + 28U);
    append_work(work, 4U, SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES, 11U, PAYLOAD_START + 32U);
    append_work(work, 2U, SERIALIZED_FILE_VALUES_FIELD_PADDING, 8U, PAYLOAD_START + 35U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 8U, PAYLOAD_START + 28U);
}

static void append_ordinary_payload_work(ExpectedWork* work) {
    append_ordinary_prefix_work(work);
    append_work(work, 5U, SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES, 12U, PAYLOAD_START + 36U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 12U, PAYLOAD_START + 36U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 0U, PAYLOAD_START);
}

static ExpectedWork ordinary_work_schedule(const ValuesFixture* fixture, size_t retained) {
    ExpectedWork work = {0};
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < ORDINARY_NODE_COUNT; ++node) {
        append_work(&work,
            1U,
            SERIALIZED_FILE_VALUES_FIELD_CONTEXT,
            node,
            fixture->nodes_start[0] + node * NODE_BYTES + 3U);
    }
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < ORDINARY_NODE_COUNT; ++node) {
        const uint64_t source = fixture->nodes_start[0] + node * NODE_BYTES;
        append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, node, source);
        for (size_t name = 0U; name < 2U; ++name) {
            size_t length = 0U;
            if (name == 0U) {
                length = strlen(ordinary_types[node]);
            } else if (node != 12U) {
                length = strlen(ordinary_fields[node]);
            }
            const SerializedFileValuesField field = name == 0U
                ? SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME
                : SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME;
            for (size_t unit = 0U; unit <= length; ++unit) {
                append_work(&work, 1U, field, node, source + 4U + name * 4U);
            }
        }
    }
    append_ordinary_payload_work(&work);
    work.planning_step = work.count;
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    work.allocation_step = work.count;
    append_work(&work, retained, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_ordinary_payload_work(&work);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    return work;
}

static bool ordinary_work_and_capacities(void) {
    ValuesFixture fixture;
    ordinary_fixture_init(&fixture, "counter", false, true, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    SerializedFileValuesLimits limits = ordinary_limits();
    allocation_calls = 0U;
    const SerializedFileValuesResult success =
        create_ordinary(&fixture, &parents, &limits, &output);
    const SerializedFileValuesView* view = serialized_file_values_view(&output);
    CHECK(success.status == SERIALIZED_FILE_VALUES_OK && view && allocation_calls == 1U &&
        success.required_retained_bytes == last_allocation_bytes &&
        success.peak_retained_bytes == last_allocation_bytes &&
        view->retained_bytes == last_allocation_bytes);
    const size_t allocation_bytes = last_allocation_bytes;
    serialized_file_values_dispose(&output);
    const ExpectedWork schedule = ordinary_work_schedule(&fixture, allocation_bytes);
    uint64_t total = 0U;
    for (size_t index = 0U; index < schedule.count; ++index) {
        total += schedule.steps[index].units;
    }
    CHECK(total == success.work_used);
    for (uint64_t cap = 0U; cap < total; ++cap) {
        limits = ordinary_limits();
        limits.max_work = cap;
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED &&
            result.limit == SERIALIZED_FILE_VALUES_LIMIT_WORK && !output.implementation &&
            live_allocations == 0U);
        uint64_t used = 0U;
        size_t pending = 0U;
        while (schedule.steps[pending].units <= cap - used) {
            used += schedule.steps[pending].units;
            ++pending;
        }
        const ExpectedWorkStep* expected = &schedule.steps[pending];
        CHECK(result.work_used == used && result.field == expected->field &&
            result.node_ordinal == expected->node && result.error_offset == expected->offset);
        CHECK(result.required_retained_bytes ==
                (pending > schedule.planning_step ? success.required_retained_bytes : 0U) &&
            result.peak_retained_bytes ==
                (pending > schedule.allocation_step ? success.required_retained_bytes : 0U));
    }
    limits = ordinary_limits();
    limits.max_work = total;
    CHECK(
        create_ordinary(&fixture, &parents, &limits, &output).status == SERIALIZED_FILE_VALUES_OK);
    serialized_file_values_dispose(&output);
    const uint64_t exact[] = {40U, 10U, 2U, 3U, 3U, 4U, success.required_retained_bytes, 8U, 29U};
    const SerializedFileValuesLimit expected_limits[] = {SERIALIZED_FILE_VALUES_LIMIT_PAYLOAD_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_VALUES,
        SERIALIZED_FILE_VALUES_LIMIT_DEPTH,
        SERIALIZED_FILE_VALUES_LIMIT_STRING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_TOTAL_STRING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_PADDING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_RETAINED_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_INTEGER_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_TOTAL_INTEGER_BYTES};
    for (size_t boundary = 0U; boundary < sizeof(exact) / sizeof(exact[0]); ++boundary) {
        for (size_t sample = 0U; sample < 3U; ++sample) {
            uint64_t cap = exact[boundary];
            if (sample == 0U) {
                cap = 0U;
            } else if (sample == 1U) {
                --cap;
            }
            limits = ordinary_limits();
            switch (boundary) {
            case 0U:
                limits.max_payload_bytes = cap;
                break;
            case 1U:
                limits.max_values = (size_t)cap;
                break;
            case 2U:
                limits.max_depth = (size_t)cap;
                break;
            case 3U:
                limits.max_string_bytes = cap;
                break;
            case 4U:
                limits.max_total_string_bytes = cap;
                break;
            case 5U:
                limits.max_padding_bytes = cap;
                break;
            case 6U:
                limits.max_retained_bytes = (size_t)cap;
                break;
            case 7U:
                limits.max_integer_bytes = cap;
                break;
            default:
                limits.max_total_integer_bytes = cap;
                break;
            }
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            CHECK(result.status ==
                (sample == 2U ? SERIALIZED_FILE_VALUES_OK : SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED));
            CHECK(result.limit ==
                (sample == 2U ? SERIALIZED_FILE_VALUES_LIMIT_NONE : expected_limits[boundary]));
            if (sample != 2U) {
                CHECK(!output.implementation && result.peak_retained_bytes == 0U);
                if (boundary >= 7U) {
                    size_t node = 2U;
                    size_t offset = 0U;
                    if (sample == 1U) {
                        node = boundary == 7U ? 3U : 12U;
                        offset = boundary == 7U ? 4U : 36U;
                    }
                    CHECK(result.field == SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES &&
                        result.node_ordinal == node &&
                        result.error_offset == PAYLOAD_START + offset);
                }
            }
            serialized_file_values_dispose(&output);
        }
    }
    allocation_calls = 0U;
    failing_allocation = 1U;
    limits = ordinary_limits();
    const SerializedFileValuesResult allocation =
        create_ordinary(&fixture, &parents, &limits, &output);
    failing_allocation = 0U;
    CHECK(allocation.status == SERIALIZED_FILE_VALUES_ALLOCATION_FAILED && allocation_calls == 1U &&
        !output.implementation && live_allocations == 0U &&
        last_allocation_bytes == allocation_bytes &&
        allocation.required_retained_bytes == last_allocation_bytes &&
        allocation.peak_retained_bytes == 0U);
    dispose_parents(&parents);
    return true;
}

static bool ordinary_extents_and_padding(void) {
    const SerializedFileValuesLimits limits = ordinary_limits();
    for (size_t cut = 0U; cut < 40U; ++cut) {
        ValuesFixture fixture;
        ordinary_fixture_init(&fixture, "counter", false, true, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        SerializedFileValuesField field = SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES;
        if ((cut >= 13U && cut < 16U) || cut == 35U) {
            field = SERIALIZED_FILE_VALUES_FIELD_PADDING;
        } else if (cut >= 28U && cut < 32U) {
            field = SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH;
        } else if (cut >= 32U && cut < 35U) {
            field = SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES;
        }
        CHECK(result.status == SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT && result.field == field &&
            result.error_offset == PAYLOAD_START + cut && !output.implementation &&
            result.peak_retained_bytes == 0U);
        if (cut == 0U) {
            SerializedFileValuesLimits zero = limits;
            zero.max_integer_bytes = 0U;
            const SerializedFileValuesResult first =
                create_ordinary(&fixture, &parents, &zero, &output);
            CHECK(first.status == SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED &&
                first.limit == SERIALIZED_FILE_VALUES_LIMIT_INTEGER_BYTES &&
                first.field == SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES &&
                first.node_ordinal == 2U && first.error_offset == PAYLOAD_START);
        }
        dispose_parents(&parents);
    }
    ValuesFixture fixture;
    ordinary_fixture_init(&fixture, "counter", false, true, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    for (size_t mapped = fixture.metadata_end; mapped < fixture.file_size; ++mapped) {
        const SerializedFileValuesResult result = serialized_file_values_create_ordinary(
            &parents.directory, &parents.schema, 0U, fixture.bytes, mapped, &limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_OBJECT && result.error_offset == mapped &&
            result.work_used == 1U && !output.implementation);
    }
    dispose_parents(&parents);
    static const uint8_t name[] = {0U, 0xffU, 0x80U, 'A', 'B', 'C', 'D'};
    const uint64_t bits[] = {0U, 1U, 0xbeU, 2U, UINT64_C(0x0123456789abcdef), UINT32_C(0xfedcba98)};
    for (size_t size = 0U; size <= sizeof(name); ++size) {
        ordinary_fixture_init(&fixture, "counter", false, true, false);
        ordinary_payload(&fixture, bits, name, size);
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        CHECK(create_ordinary(&fixture, &parents, &limits, &output).status ==
            SERIALIZED_FILE_VALUES_OK);
        CHECK(check_ordinary(&fixture, &output, bits, name, size, "counter"));
        serialized_file_values_dispose(&output);
        dispose_parents(&parents);
    }
    ordinary_fixture_init(&fixture, "counter", false, true, false);
    write_integer(fixture.bytes + PAYLOAD_START + 28U, 4U, UINT32_C(0x80000000), false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult negative =
        create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(negative.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_STRING_LENGTH &&
        negative.node_ordinal == 10U &&
        negative.field == SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH &&
        negative.error_offset == PAYLOAD_START + 28U && !output.implementation);
    dispose_parents(&parents);
    ordinary_fixture_init(&fixture, "counter", false, true, false);
    fixture.bytes[fixture.file_size] = 0x55U;
    ++fixture.file_size;
    write_integer(fixture.bytes + 24U, 8U, fixture.file_size, true);
    write_integer(fixture.bytes + fixture.object_start + 16U, 4U, 41U, false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult trailing =
        create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(trailing.status == SERIALIZED_FILE_VALUES_TRAILING_OBJECT_BYTES &&
        trailing.error_offset == PAYLOAD_START + 40U && !output.implementation);
    dispose_parents(&parents);
    return true;
}

static bool ordinary_controls_and_lineage(void) {
    const SerializedFileValuesLimits limits = ordinary_limits();
    /* Distinct local names let each original offset word identify exactly one
     * mutated fixed name. The arbitrary final field intentionally has no check. */
    for (size_t node = 0U; node < ORDINARY_NODE_COUNT; ++node) {
        for (size_t name = 0U; name < 2U; ++name) {
            if (node == 12U && name == 1U) {
                continue;
            }
            ValuesFixture fixture;
            ordinary_fixture_init(&fixture, "counter", false, true, false);
            const size_t word = fixture.nodes_start[0] + node * NODE_BYTES + 4U + name * 4U;
            const uint8_t* encoded = fixture.bytes + word;
            const uint32_t offset = (uint32_t)encoded[0] | (uint32_t)encoded[1] << 8U |
                (uint32_t)encoded[2] << 16U | (uint32_t)encoded[3] << 24U;
            fixture.bytes[fixture.strings_start[0] + offset] = 'x';
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            const SerializedFileValuesField expected = name == 0U
                ? SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME
                : SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME;
            CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA &&
                result.field == expected && result.node_ordinal == node &&
                result.error_offset == word && !output.implementation);
            dispose_parents(&parents);
        }
    }
    for (size_t node = 0U; node < ORDINARY_NODE_COUNT; ++node) {
        const size_t words[] = {0U, 3U, 12U, 16U, 20U, 24U};
        for (size_t mutation = 0U; mutation < sizeof(words) / sizeof(words[0]); ++mutation) {
            ValuesFixture fixture;
            ordinary_fixture_init(&fixture, "counter", false, true, false);
            fixture.bytes[fixture.nodes_start[0] + node * NODE_BYTES + words[mutation]] ^= 1U;
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA &&
                result.node_ordinal == node &&
                result.field == SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE &&
                !output.implementation && result.peak_retained_bytes == 0U);
            dispose_parents(&parents);
        }
    }
    {
        ValuesFixture fixture;
        ordinary_fixture_init(&fixture, "counter", false, true, false);
        /* This valid physical hierarchy moves m_FileID to the root and gives
         * m_PathID to it. The root's changed child count is the first mismatch. */
        fixture.bytes[fixture.nodes_start[0] + 2U * NODE_BYTES + 2U] = 1U;
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE && result.node_ordinal == 0U &&
            result.error_offset == fixture.nodes_start[0] && !output.implementation &&
            result.peak_retained_bytes == 0U);
        dispose_parents(&parents);
    }
    for (size_t alternative = 0U; alternative < 8U; ++alternative) {
        ValuesFixture fixture;
        ordinary_fixture_init(
            &fixture, "counter", alternative == 0U, alternative != 2U, alternative == 1U);
        SerializedFileValuesStatus expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE;
        if (alternative == 0U) {
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_ENDIAN;
        } else if (alternative == 1U) {
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_ENGINE;
        } else if (alternative == 2U) {
            expected = SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA;
        } else if (alternative == 3U) {
            fixture.bytes[fixture.type_start[0]] = 115U;
        } else if (alternative == 4U) {
            fixture.bytes[fixture.type_start[0] + 4U] = 1U;
        } else if (alternative == 5U) {
            write_integer(fixture.bytes + fixture.type_start[0] + 5U, 2U, UINT16_MAX, false);
        } else {
            const size_t registry = alternative == 6U ? 4U : 12U;
            fixture.bytes[fixture.nodes_start[0] + registry * NODE_BYTES + 3U] = 4U;
            expected = alternative == 6U ? SERIALIZED_FILE_VALUES_CONTEXT_REJECTED
                                         : SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA;
        }
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        CHECK(result.status == expected && !output.implementation &&
            result.peak_retained_bytes == 0U);
        if (alternative == 6U) {
            SerializedFileSchemaContext context;
            const SerializedFileSchemaContextResult nested =
                serialized_file_schema_context_query(&parents.schema,
                    SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT,
                    limits.max_work - 1U,
                    &context);
            CHECK(result.context_attempted && same_context_result(&result.context_result, &nested));
        }
        dispose_parents(&parents);
    }
    ValuesFixture fixture;
    ordinary_fixture_init(&fixture, "counter", false, true, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 1U, false, &parents));
    SerializedFileValues output = {0};
    CHECK(create_ordinary(&fixture, &parents, &limits, &output).status ==
        SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    dispose_parents(&parents);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const ValuesFixture copy = fixture;
    CHECK(serialized_file_values_create_ordinary(
              &parents.directory, &parents.schema, 0U, copy.bytes, copy.file_size, &limits, &output)
              .status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    CHECK(create_values(&fixture, &parents, &limits, &output).status ==
        SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE);
    CHECK(
        create_ordinary(&fixture, &parents, &limits, &output).status == SERIALIZED_FILE_VALUES_OK);
    void* owner = output.implementation;
    const SerializedFileValue saved = *serialized_file_values_value(&output, 7U);
    const SerializedFileValuesResult live = create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(live.status == SERIALIZED_FILE_VALUES_INVALID_STATE && live.work_used == 0U &&
        output.implementation == owner &&
        memcmp(&saved, serialized_file_values_value(&output, 7U), sizeof(saved)) == 0);
    serialized_file_values_dispose(&output);
    dispose_parents(&parents);
    fixture_init(&fixture, false, true, false, false, false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult text =
        create_ordinary(&fixture, &parents, &generous_limits, &output);
    CHECK(text.status == SERIALIZED_FILE_VALUES_OK &&
        text.work_used == text.required_retained_bytes + PROFILE_WORK + 2U * (20U + 9U) + 3U);
    CHECK(check_values(&fixture, &output));
    serialized_file_values_dispose(&output);
    dispose_parents(&parents);
    return true;
}

static bool ordinary_protected_mapping(void) {
#ifndef _WIN32
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > PAYLOAD_START + 40);
    const SerializedFileValuesLimits limits = ordinary_limits();
    for (size_t cut = 0U; cut <= 40U; ++cut) {
        ValuesFixture fixture;
        ordinary_fixture_init(&fixture, "counter", false, true, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        const size_t mapped_size = PAYLOAD_START + cut;
        const size_t mapping_size = 2U * (size_t)page_size;
        uint8_t* mapping =
            mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(mapping != MAP_FAILED);
        uint8_t* bytes = mapping + (size_t)page_size - mapped_size;
        memcpy(bytes, fixture.bytes, mapped_size);
        CHECK(mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) == 0);
        CHECK(mprotect(mapping, (size_t)page_size, PROT_READ) == 0);
        ValuesParents parents = {0};
        CHECK(prepare_parents_from_bytes(&fixture, bytes, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result = serialized_file_values_create_ordinary(
            &parents.directory, &parents.schema, 0U, bytes, mapped_size, &limits, &output);
        CHECK(result.status ==
            (cut == 40U ? SERIALIZED_FILE_VALUES_OK : SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT));
        CHECK(mprotect(mapping, (size_t)page_size, PROT_NONE) == 0);
        dispose_parents(&parents);
        serialized_file_values_dispose(&output);
        serialized_file_values_dispose(&output);
        CHECK(!output.implementation && live_allocations == 0U);
        CHECK(munmap(mapping, mapping_size) == 0);
    }
#endif
    return true;
}

enum {
    REFERENCE_NODE_COUNT = 15,
    REFERENCE_VALUE_COUNT = 12,
    REFERENCE_PROFILE_WORK = 273
};

typedef struct ReferenceFixtureNode {
    const char* type;
    const char* field;
    uint8_t level;
    uint32_t width;
    uint32_t metadata;
} ReferenceFixtureNode;

/* Independently authored wire facts from the complete Editor reference witness.
 * The final field is intentionally caller supplied; pointer interpretation is
 * limited to preserving the packed signed words, without resolving a target. */
static const ReferenceFixtureNode reference_nodes[REFERENCE_NODE_COUNT] = {
    {"MonoBehaviour", "Base", 0U, UINT32_MAX, 0x8000U},
    {"PPtr<GameObject>", "m_GameObject", 1U, 12U, 0x41U},
    {"int", "m_FileID", 2U, 4U, 0x41U},
    {"SInt64", "m_PathID", 2U, 8U, 0x41U},
    {"UInt8", "m_Enabled", 1U, 1U, 0x4101U},
    {"PPtr<MonoScript>", "m_Script", 1U, 12U, 0U},
    {"int", "m_FileID", 2U, 4U, 0x800001U},
    {"SInt64", "m_PathID", 2U, 8U, 0x800001U},
    {"string", "m_Name", 1U, UINT32_MAX, 0x88001U},
    {"Array", "Array", 2U, UINT32_MAX, 0x84001U},
    {"int", "size", 3U, 4U, 0x80001U},
    {"char", "data", 3U, 1U, 0x80001U},
    {"PPtr<$TextAsset>", NULL, 1U, 12U, 0U},
    {"int", "m_FileID", 2U, 4U, 0x800001U},
    {"SInt64", "m_PathID", 2U, 8U, 0x800001U}};

static size_t write_reference_type(
    ValuesFixture* fixture, size_t offset, size_t ordinal, const char* field) {
    fixture->type_start[ordinal] = offset;
    write_integer(fixture->bytes + offset, 4U, 114U, fixture->big_endian);
    write_integer(fixture->bytes + offset + 5U, 2U, 17U + ordinal, fixture->big_endian);
    offset += 7U;
    for (size_t byte = 0U; byte < 32U; ++byte) {
        fixture->bytes[offset + byte] = (uint8_t)(0x60U + byte + ordinal);
    }
    offset += 32U;
    if (!fixture->tree_enabled) {
        return offset;
    }
    write_integer(fixture->bytes + offset, 4U, REFERENCE_NODE_COUNT, fixture->big_endian);
    const size_t string_count = offset + 4U;
    fixture->nodes_start[ordinal] = offset + 8U;
    fixture->strings_start[ordinal] = offset + 8U + REFERENCE_NODE_COUNT * NODE_BYTES;
    size_t string_bytes = 0U;
    for (size_t index = 0U; index < REFERENCE_NODE_COUNT; ++index) {
        const ReferenceFixtureNode* expected = &reference_nodes[index];
        uint8_t* node = fixture->bytes + fixture->nodes_start[ordinal] + index * NODE_BYTES;
        memset(node, 0, NODE_BYTES);
        write_integer(node, 2U, 1U, fixture->big_endian);
        node[2U] = expected->level;
        node[3U] = index == 9U ? 1U : 0U;
        const char* names[] = {expected->type, index == 12U ? field : expected->field};
        for (size_t name = 0U; name < 2U; ++name) {
            write_integer(node + 4U + name * 4U, 4U, string_bytes, fixture->big_endian);
            const size_t length = strlen(names[name]) + 1U;
            memcpy(fixture->bytes + fixture->strings_start[ordinal] + string_bytes,
                names[name],
                length);
            string_bytes += length;
        }
        write_integer(node + 12U, 4U, expected->width, fixture->big_endian);
        write_integer(node + 16U, 4U, index, fixture->big_endian);
        write_integer(node + 20U, 4U, expected->metadata, fixture->big_endian);
    }
    write_integer(fixture->bytes + string_count, 4U, string_bytes, fixture->big_endian);
    offset = fixture->strings_start[ordinal] + string_bytes;
    write_integer(fixture->bytes + offset, 4U, 0U, fixture->big_endian);
    return offset + 4U;
}

static void reference_payload(
    ValuesFixture* fixture, const uint64_t bits[7], const uint8_t* name, size_t name_size) {
    ordinary_payload(fixture, bits, name, name_size);
    write_integer(fixture->bytes + fixture->file_size, 8U, bits[6], fixture->big_endian);
    fixture->file_size += 8U;
    write_integer(fixture->bytes + fixture->object_start + 16U,
        4U,
        fixture->file_size - fixture->payload_start,
        fixture->big_endian);
    write_integer(fixture->bytes + 24U, 8U, fixture->file_size, true);
}

static void reference_fixture_init(
    ValuesFixture* fixture, const char* field, bool big_endian, bool tree, bool exact29) {
    ordinary_fixture_init(fixture, field, big_endian, tree, exact29);
    /* Replace only the authored types and their following physical directory.
     * The existing helper supplies the same independent header conventions. */
    memset(fixture->bytes + 69U, 0, PAYLOAD_START - 69U);
    size_t offset = 69U;
    for (size_t ordinal = 0U; ordinal < TYPE_COUNT; ++ordinal) {
        offset = write_reference_type(fixture, offset, ordinal, field);
    }
    write_integer(fixture->bytes + offset, 4U, 1U, big_endian);
    offset += 4U;
    while (offset % 4U != 0U) {
        fixture->bytes[offset++] = 0xdbU;
    }
    fixture->object_start = offset;
    write_integer(fixture->bytes + offset, 8U, UINT64_C(0x8123456789abcdef), big_endian);
    offset += 24U + 12U + 1U;
    fixture->metadata_end = offset;
    write_integer(fixture->bytes + 16U, 8U, offset - 48U, true);
    const uint64_t bits[] = {0U,
        1U,
        0xbeU,
        2U,
        UINT64_C(0x0123456789abcdef),
        UINT32_C(0xfedcba98),
        UINT64_C(0x8123456789abcdef)};
    static const uint8_t name[] = {0xffU, 0U, 0x80U};
    reference_payload(fixture, bits, name, sizeof(name));
}

static SerializedFileValuesLimits reference_limits(void) {
    SerializedFileValuesLimits limits = ordinary_limits();
    limits.max_values = REFERENCE_VALUE_COUNT;
    limits.max_total_integer_bytes = 37U;
    return limits;
}

static bool check_reference(const ValuesFixture* fixture,
    const SerializedFileValues* output,
    const uint64_t bits[7],
    const uint8_t* name,
    size_t name_size,
    const char* field) {
    const SerializedFileValuesView* view = serialized_file_values_view(output);
    const size_t string_size = 4U + name_size + (4U - name_size % 4U) % 4U;
    const size_t final_start = 28U + string_size;
    const size_t payload_size = final_start + 12U;
    CHECK(view && view->value_count == REFERENCE_VALUE_COUNT && view->maximum_depth == 2U &&
        view->integer_bytes == 37U && view->string_bytes == name_size &&
        view->consumed_bytes == payload_size &&
        view->padding_bytes == 3U + (4U - name_size % 4U) % 4U &&
        view->object.path_id_bits == UINT64_C(0x8123456789abcdef) &&
        view->schema.class_id_bits == 114U && view->schema.has_script_hash &&
        view->schema.script_index_bits == 17U && view->schema.node_count == REFERENCE_NODE_COUNT &&
        view->context.kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT &&
        view->context.registry == SIZE_MAX && view->context.registry_end == SIZE_MAX &&
        !view->context.registry_omitted && view->context.traversal_end == REFERENCE_NODE_COUNT);
    CHECK(span_is(fixture, view->object.source, fixture->object_start, 24U) &&
        span_is(fixture,
            view->schema.type_entry_source,
            fixture->type_start[0],
            fixture->type_start[1] - fixture->type_start[0]) &&
        span_is(fixture, view->schema.script_hash_source, fixture->type_start[0] + 7U, 16U) &&
        span_is(fixture, view->schema.type_hash_source, fixture->type_start[0] + 23U, 16U));
    const size_t parents[] = {SIZE_MAX, 0U, 1U, 1U, 0U, 0U, 5U, 5U, 0U, 0U, 9U, 9U};
    const size_t children[] = {1U,
        2U,
        SIZE_MAX,
        SIZE_MAX,
        SIZE_MAX,
        6U,
        SIZE_MAX,
        SIZE_MAX,
        SIZE_MAX,
        10U,
        SIZE_MAX,
        SIZE_MAX};
    const size_t siblings[] = {
        SIZE_MAX, 4U, 3U, SIZE_MAX, 5U, 8U, 7U, SIZE_MAX, 9U, SIZE_MAX, 11U, SIZE_MAX};
    const size_t ends[] = {12U, 4U, 3U, 4U, 5U, 8U, 7U, 8U, 9U, 12U, 11U, 12U};
    const size_t offsets[] = {
        0U, 0U, 0U, 4U, 12U, 16U, 16U, 20U, 28U, final_start, final_start, final_start + 4U};
    const size_t sizes[] = {payload_size, 12U, 4U, 8U, 4U, 12U, 4U, 8U, string_size, 12U, 4U, 8U};
    const size_t bit_indices[] = {
        SIZE_MAX, SIZE_MAX, 0U, 1U, 2U, SIZE_MAX, 3U, 4U, SIZE_MAX, SIZE_MAX, 5U, 6U};
    for (size_t ordinal = 0U; ordinal < REFERENCE_VALUE_COUNT; ++ordinal) {
        const size_t node = ordinal >= 9U ? ordinal + 3U : ordinal;
        const SerializedFileValue* value = serialized_file_values_value(output, ordinal);
        const ReferenceFixtureNode* expected = &reference_nodes[node];
        const char* expected_field = node == 12U ? field : expected->field;
        CHECK(value && value->ordinal == ordinal && value->schema_node.ordinal == node &&
            value->parent == parents[ordinal] && value->first_child == children[ordinal] &&
            value->next_sibling == siblings[ordinal] && value->subtree_end == ends[ordinal] &&
            value->schema_node.version == 1U && value->schema_node.level == expected->level &&
            value->schema_node.type_flags == (node == 9U ? 1U : 0U) &&
            value->schema_node.byte_size_bits == expected->width &&
            value->schema_node.index_bits == node &&
            value->schema_node.meta_flags == expected->metadata);
        for (size_t byte = 0U; byte < sizeof(value->schema_node.opaque_tail); ++byte) {
            CHECK(value->schema_node.opaque_tail[byte] == 0U);
        }
        size_t child_count = 0U;
        if (ordinal == 0U) {
            child_count = 5U;
        } else if (ordinal == 1U || ordinal == 5U || ordinal == 9U) {
            child_count = 2U;
        }
        CHECK(value->child_count == child_count &&
            span_is(fixture, value->source, PAYLOAD_START + offsets[ordinal], sizes[ordinal]) &&
            span_is(fixture,
                value->schema_node.source,
                fixture->nodes_start[0] + node * NODE_BYTES,
                NODE_BYTES) &&
            value->schema_node.type_name.byte_count == strlen(expected->type) &&
            memcmp(value->schema_node.type_name.bytes,
                expected->type,
                strlen(expected->type) + 1U) == 0 &&
            value->schema_node.field_name.byte_count == strlen(expected_field) &&
            memcmp(value->schema_node.field_name.bytes,
                expected_field,
                strlen(expected_field) + 1U) == 0);
        CHECK(value->schema_node.type_name.space == SERIALIZED_FILE_SCHEMA_STRING_LOCAL &&
            value->schema_node.field_name.space == SERIALIZED_FILE_SCHEMA_STRING_LOCAL &&
            value->schema_node.type_name.encoded_offset == value->schema_node.type_offset_bits &&
            value->schema_node.field_name.encoded_offset == value->schema_node.name_offset_bits &&
            value->schema_node.type_name.source_offset ==
                fixture->strings_start[0] + value->schema_node.type_offset_bits &&
            value->schema_node.field_name.source_offset ==
                fixture->strings_start[0] + value->schema_node.name_offset_bits &&
            value->schema_node.type_name.bytes ==
                fixture->bytes + value->schema_node.type_name.source_offset &&
            value->schema_node.field_name.bytes ==
                fixture->bytes + value->schema_node.field_name.source_offset);
        if (ordinal == 8U) {
            CHECK(value->kind == SERIALIZED_FILE_VALUE_BYTE_STRING &&
                value->length_bits == name_size && value->array_schema_ordinal == 9U &&
                value->size_schema_ordinal == 10U && value->data_schema_ordinal == 11U &&
                value->integer_bits == 0U && span_absent(value->integer_source) &&
                span_is(fixture,
                    value->array_schema_source,
                    fixture->nodes_start[0] + 9U * NODE_BYTES,
                    NODE_BYTES) &&
                span_is(fixture,
                    value->size_schema_source,
                    fixture->nodes_start[0] + 10U * NODE_BYTES,
                    NODE_BYTES) &&
                span_is(fixture,
                    value->data_schema_source,
                    fixture->nodes_start[0] + 11U * NODE_BYTES,
                    NODE_BYTES) &&
                span_is(fixture, value->length_source, PAYLOAD_START + 28U, 4U) &&
                span_is(fixture, value->bytes_source, PAYLOAD_START + 32U, name_size) &&
                span_is(fixture,
                    value->padding_source,
                    PAYLOAD_START + 32U + name_size,
                    string_size - name_size - 4U));
            CHECK(name_size == 0U || memcmp(value->bytes_source.data, name, name_size) == 0);
        } else {
            CHECK(value->array_schema_ordinal == SIZE_MAX &&
                value->size_schema_ordinal == SIZE_MAX && value->data_schema_ordinal == SIZE_MAX &&
                span_absent(value->array_schema_source) && span_absent(value->size_schema_source) &&
                span_absent(value->data_schema_source) && value->length_bits == 0U &&
                span_absent(value->length_source) && span_absent(value->bytes_source));
            if (bit_indices[ordinal] == SIZE_MAX) {
                CHECK(value->kind == SERIALIZED_FILE_VALUE_CONTAINER && value->integer_bits == 0U &&
                    span_absent(value->integer_source) && span_absent(value->padding_source));
            } else {
                const size_t width = ordinal == 4U ? 1U : sizes[ordinal];
                CHECK(value->kind ==
                        (ordinal == 4U ? SERIALIZED_FILE_VALUE_UNSIGNED_INTEGER
                                       : SERIALIZED_FILE_VALUE_SIGNED_INTEGER) &&
                    value->integer_bits == bits[bit_indices[ordinal]] &&
                    span_is(
                        fixture, value->integer_source, PAYLOAD_START + offsets[ordinal], width));
                if (ordinal == 4U) {
                    CHECK(span_is(fixture, value->padding_source, PAYLOAD_START + 13U, 3U));
                } else {
                    CHECK(span_absent(value->padding_source));
                }
            }
        }
        for (size_t byte = 0U; byte < value->padding_source.size; ++byte) {
            CHECK(value->padding_source.data[byte] == 0xa5U);
        }
    }
    CHECK(!serialized_file_values_value(output, REFERENCE_VALUE_COUNT) &&
        !serialized_file_values_value(output, SIZE_MAX));
    return true;
}

static bool reference_bits_and_lifetime(void) {
    const char* fields[] = {"target", "different_reference", "", "\xce\xb1", "\xff"};
    static const uint64_t samples[][7] = {{0U, 0U, 0U, 0U, 0U, 0U, 0U},
        {1U, 1U, 1U, 1U, 1U, 1U, 1U},
        {UINT32_C(0x7fffffff),
            UINT64_C(0x7fffffffffffffff),
            0x7fU,
            UINT32_C(0x7fffffff),
            UINT64_C(0x7fffffffffffffff),
            UINT32_C(0x7fffffff),
            UINT64_C(0x7fffffffffffffff)},
        {UINT32_C(0x80000000),
            UINT64_C(0x8000000000000000),
            0x80U,
            UINT32_C(0x80000000),
            UINT64_C(0x8000000000000000),
            UINT32_C(0x80000000),
            UINT64_C(0x8000000000000000)},
        {UINT32_MAX, UINT64_MAX, 0xffU, UINT32_MAX, UINT64_MAX, UINT32_MAX, UINT64_MAX},
        {UINT32_C(0x89abcdef),
            UINT64_C(0x8123456789abcdef),
            0xbeU,
            7U,
            UINT64_C(0xfedcba9876543210),
            UINT32_C(0xfedcba98),
            UINT64_C(0x0123456789abcdef)}};
    static const uint8_t name[] = {0xffU, 0U, 0x80U};
    const SerializedFileValuesLimits limits = reference_limits();
    for (size_t field = 0U; field < sizeof(fields) / sizeof(fields[0]); ++field) {
        for (size_t sample = 0U; sample < sizeof(samples) / sizeof(samples[0]); ++sample) {
            ValuesFixture fixture;
            reference_fixture_init(&fixture, fields[field], false, true, false);
            reference_payload(&fixture, samples[sample], name, sizeof(name));
            CHECK(fixture.metadata_end < PAYLOAD_START);
            const ValuesFixture original = fixture;
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            allocation_calls = 0U;
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            CHECK(result.status == SERIALIZED_FILE_VALUES_OK && result.context_attempted &&
                result.context_result.work_used == 17U && allocation_calls == 1U &&
                live_allocations == 1U && result.required_retained_bytes == last_allocation_bytes &&
                result.peak_retained_bytes == last_allocation_bytes &&
                result.work_used ==
                    last_allocation_bytes + REFERENCE_PROFILE_WORK + 2U * (48U + 23U) + 3U);
            CHECK(check_reference(
                &fixture, &output, samples[sample], name, sizeof(name), fields[field]));
            const SerializedFileValue saved = *serialized_file_values_value(&output, 11U);
            dispose_parents(&parents);
            CHECK(check_reference(
                &fixture, &output, samples[sample], name, sizeof(name), fields[field]));
            serialized_file_values_dispose(&output);
            CHECK(saved.integer_bits == samples[sample][6] &&
                span_is(&fixture, saved.integer_source, PAYLOAD_START + 40U, 8U) &&
                memcmp(&fixture, &original, sizeof(fixture)) == 0 && live_allocations == 0U);
        }
    }
    return true;
}

static void append_reference_payload_work(ExpectedWork* work) {
    append_ordinary_prefix_work(work);
    append_work(work, 5U, SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES, 13U, PAYLOAD_START + 36U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 13U, PAYLOAD_START + 36U);
    append_work(work, 9U, SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES, 14U, PAYLOAD_START + 40U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 14U, PAYLOAD_START + 40U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 12U, PAYLOAD_START + 36U);
    append_work(work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 0U, PAYLOAD_START);
}

static ExpectedWork reference_work_schedule(const ValuesFixture* fixture, size_t retained) {
    ExpectedWork work = {0};
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < REFERENCE_NODE_COUNT; ++node) {
        append_work(&work,
            1U,
            SERIALIZED_FILE_VALUES_FIELD_CONTEXT,
            node,
            fixture->nodes_start[0] + node * NODE_BYTES + 3U);
    }
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_CONTEXT, SIZE_MAX, UINT64_MAX);
    for (size_t node = 0U; node < REFERENCE_NODE_COUNT; ++node) {
        const uint64_t source = fixture->nodes_start[0] + node * NODE_BYTES;
        append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, node, source);
        const char* names[] = {reference_nodes[node].type, reference_nodes[node].field};
        for (size_t name = 0U; name < 2U; ++name) {
            const size_t length = names[name] ? strlen(names[name]) : 0U;
            const SerializedFileValuesField field = name == 0U
                ? SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME
                : SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME;
            for (size_t unit = 0U; unit <= length; ++unit) {
                append_work(&work, 1U, field, node, source + 4U + name * 4U);
            }
        }
    }
    append_reference_payload_work(&work);
    work.planning_step = work.count;
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    work.allocation_step = work.count;
    append_work(&work, retained, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    append_reference_payload_work(&work);
    append_work(&work, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
    return work;
}

static void reference_cap(SerializedFileValuesLimits* limits, size_t boundary, uint64_t cap) {
    switch (boundary) {
    case 0U:
        limits->max_payload_bytes = cap;
        break;
    case 1U:
        limits->max_values = (size_t)cap;
        break;
    case 2U:
        limits->max_depth = (size_t)cap;
        break;
    case 3U:
        limits->max_string_bytes = cap;
        break;
    case 4U:
        limits->max_total_string_bytes = cap;
        break;
    case 5U:
        limits->max_padding_bytes = cap;
        break;
    case 6U:
        limits->max_retained_bytes = (size_t)cap;
        break;
    case 7U:
        limits->max_integer_bytes = cap;
        break;
    default:
        limits->max_total_integer_bytes = cap;
        break;
    }
}

static bool reference_work_and_capacities(void) {
    ValuesFixture fixture;
    reference_fixture_init(&fixture, "target", false, true, false);
    const ValuesFixture original = fixture;
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));

    struct {
        uint64_t before;
        SerializedFileValues output;
        uint64_t after;
    } guarded = {UINT64_C(0x123456789abcdef0), {0}, UINT64_C(0xfedcba9876543210)};

    SerializedFileValuesLimits limits = reference_limits();
    const SerializedFileValuesResult success =
        create_ordinary(&fixture, &parents, &limits, &guarded.output);
    CHECK(success.status == SERIALIZED_FILE_VALUES_OK);
    const size_t allocation_bytes = success.required_retained_bytes;
    CHECK(serialized_file_values_view(&guarded.output)->retained_bytes == allocation_bytes);
    serialized_file_values_dispose(&guarded.output);
    const ExpectedWork schedule = reference_work_schedule(&fixture, allocation_bytes);
    CHECK(schedule.count <= sizeof(schedule.steps) / sizeof(schedule.steps[0]));
    uint64_t total = 0U;
    for (size_t step = 0U; step < schedule.count; ++step) {
        total += schedule.steps[step].units;
    }
    CHECK(total == success.work_used &&
        total == allocation_bytes + REFERENCE_PROFILE_WORK + 2U * (48U + 23U) + 3U);
    for (uint64_t cap = 0U; cap <= total; ++cap) {
        limits = reference_limits();
        limits.max_work = cap;
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &guarded.output);
        if (cap == total) {
            CHECK(result.status == SERIALIZED_FILE_VALUES_OK);
            serialized_file_values_dispose(&guarded.output);
            continue;
        }
        uint64_t used = 0U;
        size_t pending = 0U;
        while (schedule.steps[pending].units <= cap - used) {
            used += schedule.steps[pending].units;
            ++pending;
        }
        const ExpectedWorkStep* expected = &schedule.steps[pending];
        CHECK(result.status == SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED &&
            result.limit == SERIALIZED_FILE_VALUES_LIMIT_WORK && result.work_used == used &&
            result.field == expected->field && result.node_ordinal == expected->node &&
            result.error_offset == expected->offset && !guarded.output.implementation &&
            live_allocations == 0U &&
            result.required_retained_bytes ==
                (pending > schedule.planning_step ? allocation_bytes : 0U) &&
            result.peak_retained_bytes ==
                (pending > schedule.allocation_step ? allocation_bytes : 0U));
    }
    const uint64_t exact[] = {48U, 12U, 2U, 3U, 3U, 4U, allocation_bytes, 8U, 37U};
    const SerializedFileValuesLimit expected_limits[] = {SERIALIZED_FILE_VALUES_LIMIT_PAYLOAD_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_VALUES,
        SERIALIZED_FILE_VALUES_LIMIT_DEPTH,
        SERIALIZED_FILE_VALUES_LIMIT_STRING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_TOTAL_STRING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_PADDING_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_RETAINED_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_INTEGER_BYTES,
        SERIALIZED_FILE_VALUES_LIMIT_TOTAL_INTEGER_BYTES};
    for (size_t boundary = 0U; boundary < sizeof(exact) / sizeof(exact[0]); ++boundary) {
        for (size_t sample = 0U; sample < 3U; ++sample) {
            uint64_t cap = exact[boundary];
            if (sample == 0U) {
                cap = 0U;
            } else if (sample == 1U) {
                --cap;
            }
            limits = reference_limits();
            reference_cap(&limits, boundary, cap);
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &guarded.output);
            CHECK(result.status ==
                    (sample == 2U ? SERIALIZED_FILE_VALUES_OK
                                  : SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED) &&
                result.limit ==
                    (sample == 2U ? SERIALIZED_FILE_VALUES_LIMIT_NONE : expected_limits[boundary]));
            if (sample != 2U) {
                CHECK(!guarded.output.implementation && result.peak_retained_bytes == 0U);
                if (boundary >= 7U) {
                    size_t node = 2U;
                    size_t offset = 0U;
                    if (sample == 1U) {
                        node = boundary == 7U ? 3U : 14U;
                        offset = boundary == 7U ? 4U : 40U;
                    }
                    CHECK(result.field == SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES &&
                        result.node_ordinal == node &&
                        result.error_offset == PAYLOAD_START + offset);
                }
            }
            serialized_file_values_dispose(&guarded.output);
        }
    }
    allocation_calls = 0U;
    failing_allocation = 1U;
    limits = reference_limits();
    const SerializedFileValuesResult failure =
        create_ordinary(&fixture, &parents, &limits, &guarded.output);
    failing_allocation = 0U;
    CHECK(failure.status == SERIALIZED_FILE_VALUES_ALLOCATION_FAILED && allocation_calls == 1U &&
        failure.required_retained_bytes == allocation_bytes && failure.peak_retained_bytes == 0U &&
        !guarded.output.implementation && live_allocations == 0U &&
        guarded.before == UINT64_C(0x123456789abcdef0) &&
        guarded.after == UINT64_C(0xfedcba9876543210) &&
        memcmp(&fixture, &original, sizeof(fixture)) == 0);
    dispose_parents(&parents);
    return true;
}

static bool reference_extents_and_padding(void) {
    const SerializedFileValuesLimits limits = reference_limits();
    for (size_t cut = 0U; cut < 48U; ++cut) {
        ValuesFixture fixture;
        reference_fixture_init(&fixture, "target", false, true, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        SerializedFileValuesField field = SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES;
        if ((cut >= 13U && cut < 16U) || cut == 35U) {
            field = SERIALIZED_FILE_VALUES_FIELD_PADDING;
        } else if (cut >= 28U && cut < 32U) {
            field = SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH;
        } else if (cut >= 32U && cut < 35U) {
            field = SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES;
        }
        CHECK(result.status == SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT && result.field == field &&
            result.error_offset == PAYLOAD_START + cut && !output.implementation &&
            result.peak_retained_bytes == 0U);
        if (cut >= 36U) {
            CHECK(result.node_ordinal == (cut < 40U ? 13U : 14U));
        }
        if (cut == 36U || cut == 40U) {
            SerializedFileValuesLimits exhausted = limits;
            exhausted.max_total_integer_bytes = cut == 36U ? 25U : 29U;
            const SerializedFileValuesResult cap =
                create_ordinary(&fixture, &parents, &exhausted, &output);
            CHECK(cap.status == SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED &&
                cap.limit == SERIALIZED_FILE_VALUES_LIMIT_TOTAL_INTEGER_BYTES &&
                cap.field == SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES &&
                cap.node_ordinal == (cut == 36U ? 13U : 14U) &&
                cap.error_offset == PAYLOAD_START + cut);
        }
        dispose_parents(&parents);
    }
    ValuesFixture fixture;
    reference_fixture_init(&fixture, "target", false, true, false);
    ValuesParents parents = {0};
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    SerializedFileValues output = {0};
    for (size_t mapped = fixture.metadata_end; mapped < fixture.file_size; ++mapped) {
        const SerializedFileValuesResult result = serialized_file_values_create_ordinary(
            &parents.directory, &parents.schema, 0U, fixture.bytes, mapped, &limits, &output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING &&
            result.field == SERIALIZED_FILE_VALUES_FIELD_OBJECT && result.error_offset == mapped &&
            result.work_used == 1U && !output.implementation);
    }
    dispose_parents(&parents);
    const uint64_t bits[] = {0U,
        1U,
        0xbeU,
        2U,
        UINT64_C(0x0123456789abcdef),
        UINT32_C(0xfedcba98),
        UINT64_C(0x8123456789abcdef)};
    static const uint8_t name[] = {0U, 0xffU, 0x80U, 'A', 'B', 'C', 'D'};
    for (size_t size = 0U; size <= sizeof(name); ++size) {
        reference_fixture_init(&fixture, "target", false, true, false);
        reference_payload(&fixture, bits, name, size);
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        CHECK(create_ordinary(&fixture, &parents, &limits, &output).status ==
            SERIALIZED_FILE_VALUES_OK);
        CHECK(check_reference(&fixture, &output, bits, name, size, "target"));
        serialized_file_values_dispose(&output);
        dispose_parents(&parents);
    }
    reference_fixture_init(&fixture, "target", false, true, false);
    write_integer(fixture.bytes + PAYLOAD_START + 28U, 4U, UINT32_C(0x80000000), false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult negative =
        create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(negative.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_STRING_LENGTH &&
        negative.node_ordinal == 10U &&
        negative.field == SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH &&
        negative.error_offset == PAYLOAD_START + 28U && !output.implementation);
    dispose_parents(&parents);
    reference_fixture_init(&fixture, "target", false, true, false);
    fixture.bytes[fixture.file_size++] = 0x55U;
    write_integer(fixture.bytes + 24U, 8U, fixture.file_size, true);
    write_integer(fixture.bytes + fixture.object_start + 16U, 4U, 49U, false);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    const SerializedFileValuesResult trailing =
        create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(trailing.status == SERIALIZED_FILE_VALUES_TRAILING_OBJECT_BYTES &&
        trailing.error_offset == PAYLOAD_START + 48U && !output.implementation);
    dispose_parents(&parents);
    return true;
}

static bool reference_schema_controls(void) {
    const SerializedFileValuesLimits limits = reference_limits();
    for (size_t node = 0U; node < REFERENCE_NODE_COUNT; ++node) {
        for (size_t name = 0U; name < 2U; ++name) {
            if (node == 12U && name == 1U) {
                continue;
            }
            ValuesFixture fixture;
            reference_fixture_init(&fixture, "target", false, true, false);
            const size_t word = fixture.nodes_start[0] + node * NODE_BYTES + 4U + name * 4U;
            const uint8_t* encoded = fixture.bytes + word;
            const uint32_t offset = (uint32_t)encoded[0] | (uint32_t)encoded[1] << 8U |
                (uint32_t)encoded[2] << 16U | (uint32_t)encoded[3] << 24U;
            fixture.bytes[fixture.strings_start[0] + offset] = 'x';
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            const SerializedFileValuesField expected = name == 0U
                ? SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME
                : SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME;
            CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA &&
                result.field == expected && result.node_ordinal == node &&
                result.error_offset == word && !output.implementation);
            dispose_parents(&parents);
        }
        const size_t words[] = {0U, 3U, 12U, 16U, 20U, 24U};
        for (size_t mutation = 0U; mutation < sizeof(words) / sizeof(words[0]); ++mutation) {
            ValuesFixture fixture;
            reference_fixture_init(&fixture, "target", false, true, false);
            fixture.bytes[fixture.nodes_start[0] + node * NODE_BYTES + words[mutation]] ^= 1U;
            ValuesParents parents = {0};
            CHECK(prepare_parents(&fixture, 0U, false, &parents));
            SerializedFileValues output = {0};
            const SerializedFileValuesResult result =
                create_ordinary(&fixture, &parents, &limits, &output);
            CHECK(result.status == SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA &&
                result.field == SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE &&
                result.node_ordinal == node && !output.implementation &&
                result.peak_retained_bytes == 0U);
            dispose_parents(&parents);
        }
    }
    /* The spelling marker and complete final-child hierarchy are controls,
     * not permission to accept an arbitrary similarly named pointer. */
    for (size_t alternative = 0U; alternative < 10U; ++alternative) {
        ValuesFixture fixture;
        reference_fixture_init(
            &fixture, "target", alternative == 0U, alternative != 2U, alternative == 1U);
        SerializedFileValuesStatus expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE;
        if (alternative == 0U) {
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_ENDIAN;
        } else if (alternative == 1U) {
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_ENGINE;
        } else if (alternative == 2U) {
            expected = SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA;
        } else if (alternative == 3U) {
            fixture.bytes[fixture.type_start[0]] = 115U;
        } else if (alternative == 4U) {
            fixture.bytes[fixture.type_start[0] + 4U] = 1U;
        } else if (alternative == 5U) {
            write_integer(fixture.bytes + fixture.type_start[0] + 5U, 2U, UINT16_MAX, false);
        } else if (alternative == 6U || alternative == 7U) {
            const size_t registry = alternative == 6U ? 4U : 12U;
            fixture.bytes[fixture.nodes_start[0] + registry * NODE_BYTES + 3U] = 4U;
            expected = alternative == 6U ? SERIALIZED_FILE_VALUES_CONTEXT_REJECTED
                                         : SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA;
        } else if (alternative == 8U) {
            fixture.bytes[fixture.nodes_start[0] + 13U * NODE_BYTES + 2U] = 1U;
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA;
        } else {
            const size_t word = fixture.nodes_start[0] + 12U * NODE_BYTES + 4U;
            const uint8_t* encoded = fixture.bytes + word;
            const uint32_t offset = (uint32_t)encoded[0] | (uint32_t)encoded[1] << 8U |
                (uint32_t)encoded[2] << 16U | (uint32_t)encoded[3] << 24U;
            fixture.bytes[fixture.strings_start[0] + offset + 5U] = '_';
            expected = SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA;
        }
        ValuesParents parents = {0};
        CHECK(prepare_parents(&fixture, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result =
            create_ordinary(&fixture, &parents, &limits, &output);
        CHECK(result.status == expected && !output.implementation &&
            result.peak_retained_bytes == 0U);
        if (alternative == 6U || alternative == 7U) {
            SerializedFileSchemaContext context;
            const SerializedFileSchemaContextResult nested =
                serialized_file_schema_context_query(&parents.schema,
                    SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT,
                    limits.max_work - 1U,
                    &context);
            CHECK(result.context_attempted && same_context_result(&result.context_result, &nested));
        }
        dispose_parents(&parents);
    }
    return true;
}

static bool reference_lineage_and_states(void) {
    ValuesFixture fixture;
    reference_fixture_init(&fixture, "target", false, true, false);
    const ValuesFixture original = fixture;
    ValuesParents parents = {0};
    const SerializedFileValuesLimits limits = reference_limits();
    CHECK(prepare_parents(&fixture, 1U, false, &parents));
    SerializedFileValues output = {0};
    CHECK(create_ordinary(&fixture, &parents, &limits, &output).status ==
        SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    dispose_parents(&parents);
    CHECK(prepare_parents(&fixture, 0U, false, &parents));
    CHECK(serialized_file_values_create_ordinary(&parents.directory,
              &parents.schema,
              0U,
              original.bytes,
              original.file_size,
              &limits,
              &output)
              .status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    CHECK(serialized_file_values_create_ordinary(&parents.directory,
              &parents.schema,
              0U,
              fixture.bytes,
              fixture.file_size + 1U,
              &limits,
              &output)
              .status == SERIALIZED_FILE_VALUES_SOURCE_MISMATCH);
    CHECK(create_values(&fixture, &parents, &limits, &output).status ==
        SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE);
    for (size_t missing = 0U; missing < 5U; ++missing) {
        const SerializedFileDirectory* directory = missing == 0U ? NULL : &parents.directory;
        const SerializedFileSchema* schema = missing == 1U ? NULL : &parents.schema;
        const uint8_t* bytes = missing == 2U ? NULL : fixture.bytes;
        const SerializedFileValuesLimits* selected_limits = missing == 3U ? NULL : &limits;
        SerializedFileValues* selected_output = missing == 4U ? NULL : &output;
        const SerializedFileValuesResult result = serialized_file_values_create_ordinary(
            directory, schema, 0U, bytes, fixture.file_size, selected_limits, selected_output);
        CHECK(result.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT && result.work_used == 0U);
    }
    SerializedFileValues* overlapping = (SerializedFileValues*)(void*)fixture.bytes;
    const SerializedFileValuesResult alias =
        create_ordinary(&fixture, &parents, &limits, overlapping);
    CHECK(alias.status == SERIALIZED_FILE_VALUES_INVALID_ARGUMENT && alias.work_used == 0U &&
        memcmp(&fixture, &original, sizeof(fixture)) == 0);
    CHECK(
        create_ordinary(&fixture, &parents, &limits, &output).status == SERIALIZED_FILE_VALUES_OK);
    void* owner = output.implementation;
    const SerializedFileValuesView saved_view = *serialized_file_values_view(&output);
    const SerializedFileValue saved_value = *serialized_file_values_value(&output, 11U);
    const SerializedFileValuesResult live = create_ordinary(&fixture, &parents, &limits, &output);
    CHECK(live.status == SERIALIZED_FILE_VALUES_INVALID_STATE && live.work_used == 0U &&
        output.implementation == owner &&
        memcmp(&saved_view, serialized_file_values_view(&output), sizeof(saved_view)) == 0 &&
        memcmp(&saved_value, serialized_file_values_value(&output, 11U), sizeof(saved_value)) == 0);
    serialized_file_values_dispose(&output);
    dispose_parents(&parents);
    return true;
}

static bool reference_protected_mapping(void) {
#ifndef _WIN32
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > PAYLOAD_START + 48);
    const SerializedFileValuesLimits limits = reference_limits();
    for (size_t cut = 0U; cut <= 48U; ++cut) {
        ValuesFixture fixture;
        reference_fixture_init(&fixture, "target", false, true, false);
        write_integer(fixture.bytes + fixture.object_start + 16U, 4U, cut, false);
        const size_t mapped_size = PAYLOAD_START + cut;
        const size_t mapping_size = 2U * (size_t)page_size;
        uint8_t* mapping =
            mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(mapping != MAP_FAILED);
        uint8_t* bytes = mapping + (size_t)page_size - mapped_size;
        memcpy(bytes, fixture.bytes, mapped_size);
        CHECK(mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) == 0);
        CHECK(mprotect(mapping, (size_t)page_size, PROT_READ) == 0);
        ValuesParents parents = {0};
        CHECK(prepare_parents_from_bytes(&fixture, bytes, 0U, false, &parents));
        SerializedFileValues output = {0};
        const SerializedFileValuesResult result = serialized_file_values_create_ordinary(
            &parents.directory, &parents.schema, 0U, bytes, mapped_size, &limits, &output);
        CHECK(result.status ==
            (cut == 48U ? SERIALIZED_FILE_VALUES_OK : SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT));
        CHECK(mprotect(mapping, (size_t)page_size, PROT_NONE) == 0);
        dispose_parents(&parents);
        serialized_file_values_dispose(&output);
        serialized_file_values_dispose(&output);
        CHECK(!output.implementation && live_allocations == 0U);
        CHECK(munmap(mapping, mapping_size) == 0);
    }
#endif
    return true;
}

int main(void) {
    if (!success_and_lifetime() || !work_and_allocation_boundaries() || !capacity_boundaries() ||
        !string_extents_and_empty() || !object_cursor_domain() || !string_alignment_residues() ||
        !object_end_guard() || !profile_refusals() || !aliases_leave_inputs_unchanged() ||
        !disposal_does_not_read_backing() || !sources_and_states() ||
        !ordinary_scalar_bits_and_lifetime() || !ordinary_work_and_capacities() ||
        !ordinary_extents_and_padding() || !ordinary_controls_and_lineage() ||
        !ordinary_protected_mapping() || !reference_bits_and_lifetime() ||
        !reference_work_and_capacities() || !reference_extents_and_padding() ||
        !reference_schema_controls() || !reference_lineage_and_states() ||
        !reference_protected_mapping()) {
        return 1;
    }
    if (live_allocations != 0U) {
        fprintf(stderr, "Values allocation leak: %zu\n", live_allocations);
        return 1;
    }
    puts(
        "SerializedFile ordinary values: ownership, provenance, profile, work and extent checks passed");
    return 0;
}
