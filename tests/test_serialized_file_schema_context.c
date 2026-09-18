#include "io/serialized_file_schema_context.h"

#include "serialized_schema_context_fixture.h"

#include "common/common.h"

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
    TAIL_FILE_BYTES = 4336,
    TAIL_REFERENCE_NODES = 2069,
    TAIL_REFERENCE_COUNT = 6,
    SCHEMA_SNAPSHOT_BYTES = 8192,
    NODE_BYTES = 32
};

static const char tail_sha256[] =
    "0a2dbcb88979c18afb5331524fe5184f44af13c847279f9cefb514d6f585fea0";

static SerializedFileSchemaContextKind context_kind(bool reference) {
    return reference ? SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD
                     : SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT;
}

static bool no_node_diagnostic(const SerializedFileSchemaContextResult* result) {
    CHECK(result->field == SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE &&
        result->node_ordinal == SIZE_MAX && result->error_offset == UINT64_MAX);
    return true;
}

static bool exhaustive_work(const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind kind,
    size_t count,
    uint64_t node_start) {
    const SerializedFileSchemaView* view = serialized_file_schema_view(schema);
    CHECK(view && view->node_count == count);
    unsigned char original_storage[SCHEMA_SNAPSHOT_BYTES];
    CHECK(view->retained_bytes <= sizeof(original_storage));
    memcpy(original_storage, schema->implementation, view->retained_bytes);
    const size_t allocation_count = g_allocations_count;
    const size_t allocation_bytes = g_allocated_bytes;
    for (uint64_t maximum = 0U; maximum < count + 2U; ++maximum) {
        SerializedFileSchemaContext output;
        memset(&output, 0xa7, sizeof(output));
        unsigned char original[sizeof(output)];
        memcpy(original, &output, sizeof(original));
        const SerializedFileSchemaContextResult result =
            serialized_file_schema_context_query(schema, kind, maximum, &output);
        CHECK(result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_WORK_LIMIT &&
            result.work_used == maximum && result.type_ordinal == view->type_ordinal &&
            memcmp(original, &output, sizeof(original)) == 0);
        if (maximum == 0U || maximum == count + 1U) {
            CHECK(no_node_diagnostic(&result));
        } else {
            CHECK(result.field == SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_TYPE_FLAGS &&
                result.node_ordinal == maximum - 1U &&
                result.error_offset == node_start + (maximum - 1U) * NODE_BYTES + 3U);
        }
        CHECK(g_allocations_count == allocation_count && g_allocated_bytes == allocation_bytes &&
            memcmp(original_storage, schema->implementation, view->retained_bytes) == 0);
    }
    SerializedFileSchemaContext output;
    const SerializedFileSchemaContextResult success =
        serialized_file_schema_context_query(schema, kind, count + 2U, &output);
    CHECK(success.status == SERIALIZED_FILE_SCHEMA_CONTEXT_OK && success.work_used == count + 2U &&
        no_node_diagnostic(&success) &&
        memcmp(original_storage, schema->implementation, view->retained_bytes) == 0 &&
        g_allocations_count == allocation_count && g_allocated_bytes == allocation_bytes);
    return true;
}

static bool argument_and_alias_boundaries(SchemaContextFixture* fixture) {
    CHECK(schema_context_fixture_prepare(fixture));
    SerializedFileSchema schema = {0};
    CHECK(schema_context_fixture_schema(fixture, true, 0U, &schema));
    const SerializedFileSchemaView* view = serialized_file_schema_view(&schema);
    SerializedFileSchema empty = {0};
    SerializedFileSchemaContext output;
    memset(&output, 0x6d, sizeof(output));
    unsigned char original_output[sizeof(output)];
    memcpy(original_output, &output, sizeof(original_output));
    CHECK(serialized_file_schema_context_query(NULL, context_kind(true), 10U, &output).status ==
        SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT);
    CHECK(serialized_file_schema_context_query(&schema, context_kind(true), 10U, NULL).status ==
        SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT);
    CHECK(serialized_file_schema_context_query(
              &schema, (SerializedFileSchemaContextKind)0, 10U, &output)
              .status == SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT);
    CHECK(serialized_file_schema_context_query(
              &schema, (SerializedFileSchemaContextKind)99, 10U, &output)
              .status == SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT);
    const SerializedFileSchemaContextResult empty_result =
        serialized_file_schema_context_query(&empty, context_kind(true), 10U, &output);
    CHECK(empty_result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_STATE &&
        !empty_result.work_used && empty_result.type_ordinal == SIZE_MAX &&
        no_node_diagnostic(&empty_result));
    const SerializedFileSchemaContextResult wrong =
        serialized_file_schema_context_query(&schema, context_kind(false), 0U, &output);
    CHECK(wrong.status == SERIALIZED_FILE_SCHEMA_CONTEXT_WRONG_ROW_KIND && !wrong.work_used &&
        wrong.type_ordinal == 0U && no_node_diagnostic(&wrong));
    CHECK(memcmp(original_output, &output, sizeof(output)) == 0);

    unsigned char original_file[TAIL_FILE_BYTES];
    memcpy(original_file, fixture->file.data, sizeof(original_file));
    unsigned char original_storage[SCHEMA_SNAPSHOT_BYTES];
    CHECK(view->retained_bytes <= sizeof(original_storage));
    memcpy(original_storage, schema.implementation, view->retained_bytes);
    const SerializedFileSchemaNode* marker = serialized_file_schema_node(&schema, 1U);
    CHECK(marker && marker->type_name.space == SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35);
    const uint8_t* common = marker->type_name.bytes - marker->type_name.source_offset;
    const size_t alignment = _Alignof(SerializedFileSchemaContext);
    const size_t padding = (alignment - (uintptr_t)common % alignment) % alignment;
    const uint8_t* common_alias = common + padding;
    void* aliases[] = {(void*)&schema,
        schema.implementation,
        (void*)view,
        (void*)marker,
        fixture->file.data,
        fixture->file.data + 128U,
        (void*)common_alias};
    for (size_t index = 0U; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
        const SerializedFileSchemaContextResult result = serialized_file_schema_context_query(
            &schema, context_kind(true), UINT64_MAX, (SerializedFileSchemaContext*)aliases[index]);
        CHECK(result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT &&
            !result.work_used && result.type_ordinal == SIZE_MAX && no_node_diagnostic(&result) &&
            memcmp(original_file, fixture->file.data, sizeof(original_file)) == 0 &&
            memcmp(original_storage, schema.implementation, view->retained_bytes) == 0);
    }
    CHECK(exhaustive_work(&schema, context_kind(true), 6U, TAIL_REFERENCE_NODES));
    schema_context_fixture_dispose_parents(fixture);
    CHECK(serialized_file_schema_context_query(&schema, context_kind(true), 8U, &output).status ==
        SERIALIZED_FILE_SCHEMA_CONTEXT_OK);
    CHECK(output.traversal_end == 6U && output.registry == SIZE_MAX &&
        output.registry_end == SIZE_MAX && !output.registry_omitted);
    serialized_file_schema_dispose(&schema);
    CHECK(output.tree.nodes_source.data == fixture->file.data + TAIL_REFERENCE_NODES &&
        output.tree.nodes_source.size == 192U && output.tree.nodes_source.data[2] == 0U);
    return true;
}

typedef struct RegistryMutation {
    const char* name;
    size_t first_node;
    uint8_t first_flags;
    size_t second_node;
    uint8_t second_flags;
    SerializedFileSchemaContextStatus expected;
    size_t error_node;
} RegistryMutation;

static bool registry_mutations(SchemaContextFixture* fixture) {
    /* These are product-only synthetic bytes. A mask0x04 node is a boundary
     * marker here even when its authored type/name is not a registry shape. */
    static const RegistryMutation cases[] = {
        {"final", 2U, 4U, SIZE_MAX, 0U, SERIALIZED_FILE_SCHEMA_CONTEXT_OK, SIZE_MAX},
        {"root",
            0U,
            4U,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
            0U},
        {"nested",
            3U,
            4U,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
            3U},
        {"nonfinal",
            1U,
            4U,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
            1U},
        {"multiple-siblings",
            1U,
            4U,
            2U,
            4U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
            1U},
        {"nested-in-registry",
            2U,
            4U,
            3U,
            4U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
            3U},
        {"combined",
            2U,
            5U,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_FLAGS,
            2U},
        {"combined-root",
            0U,
            0x84U,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_FLAGS,
            0U},
        {"unknown-unclassified",
            2U,
            0xfbU,
            SIZE_MAX,
            0U,
            SERIALIZED_FILE_SCHEMA_CONTEXT_OK,
            SIZE_MAX}};
    uint8_t original_file[TAIL_FILE_BYTES];
    memcpy(original_file, fixture->file.data, sizeof(original_file));
    for (size_t reference = 0U; reference < 2U; ++reference) {
        const size_t nodes[] = {0U, 1U, reference ? 2U : 5U, reference ? 3U : 6U};
        const size_t node_start = reference ? TAIL_REFERENCE_NODES : 100U;
        const size_t node_count = reference ? 6U : 9U;
        for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            const RegistryMutation* mutation = &cases[index];
            memcpy(fixture->file.data, original_file, sizeof(original_file));
            fixture->file.data[node_start + nodes[mutation->first_node] * NODE_BYTES + 3U] =
                mutation->first_flags;
            if (mutation->second_node != SIZE_MAX) {
                fixture->file.data[node_start + nodes[mutation->second_node] * NODE_BYTES + 3U] =
                    mutation->second_flags;
            }
            CHECK(schema_context_fixture_prepare(fixture));
            SerializedFileSchema schema = {0};
            CHECK(schema_context_fixture_schema(fixture, reference != 0U, 0U, &schema));
            SerializedFileSchemaContext output;
            memset(&output, 0xc3, sizeof(output));
            unsigned char original[sizeof(output)];
            memcpy(original, &output, sizeof(original));
            const SerializedFileSchemaContextResult result = serialized_file_schema_context_query(
                &schema, context_kind(reference != 0U), node_count + 2U, &output);
            if (result.status != mutation->expected) {
                fprintf(
                    stderr, "Registry mutation %s returned %d\n", mutation->name, result.status);
                return false;
            }
            CHECK(result.type_ordinal == 0U);
            if (mutation->expected == SERIALIZED_FILE_SCHEMA_CONTEXT_OK) {
                CHECK(result.work_used == node_count + 2U && no_node_diagnostic(&result));
                const bool has_registry = mutation->first_flags == 4U;
                const bool omitted = has_registry && reference != 0U;
                CHECK(output.registry == (has_registry ? nodes[2] : SIZE_MAX) &&
                    output.registry_end == (has_registry ? node_count : SIZE_MAX) &&
                    output.traversal_end == (omitted ? nodes[2] : node_count) &&
                    output.registry_omitted == omitted && output.first_child == 1U);
            } else {
                const bool flags =
                    mutation->expected == SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_FLAGS;
                const size_t error_node = nodes[mutation->error_node];
                CHECK(result.node_ordinal == error_node && result.work_used == error_node + 2U &&
                    result.field ==
                        (flags ? SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_TYPE_FLAGS
                               : SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_LEVEL) &&
                    result.error_offset ==
                        node_start + error_node * NODE_BYTES + (flags ? 3U : 2U) &&
                    memcmp(original, &output, sizeof(original)) == 0);
            }
            serialized_file_schema_dispose(&schema);
            schema_context_fixture_dispose_parents(fixture);
        }
    }
    memcpy(fixture->file.data, original_file, sizeof(original_file));
    return true;
}

static void store_integer(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        bytes[big_endian ? width - index - 1U : index] = (uint8_t)value;
        value >>= 8U;
    }
}

static bool singleton_contexts(void) {
    for (size_t reference = 0U; reference < 2U; ++reference) {
        for (size_t endian = 0U; endian < 2U; ++endian) {
            SchemaContextFixture fixture = {0};
            fixture.file.size = reference ? 177U : 153U;
            fixture.file.data = calloc(1U, fixture.file.size);
            CHECK(fixture.file.data);
            uint8_t* bytes = fixture.file.data;
            const bool big_endian = endian != 0U;
            const size_t row = reference ? 85U : 69U;
            const size_t count = reference ? 124U : 92U;
            const size_t node = count + 8U;
            memcpy(bytes + 48U, "2021.3.35f1", 12U);
            bytes[64U] = 1U;
            store_integer(bytes + 65U, 4U, reference ? 0U : 1U, big_endian);
            if (reference) {
                store_integer(bytes + 81U, 4U, 1U, big_endian);
                memcpy(bytes + 164U, "Payload\0N\0A\0", 12U);
            }
            store_integer(bytes + row, 4U, reference ? UINT32_MAX : 1U, big_endian);
            store_integer(bytes + row + 5U, 2U, reference ? 0U : UINT16_MAX, big_endian);
            store_integer(bytes + count, 4U, 1U, big_endian);
            store_integer(bytes + node, 2U, UINT16_MAX, big_endian);
            bytes[node + 3U] = 0xf3U; /* Unknown non-registry flags stay unclassified. */
            store_integer(bytes + node + 4U, 4U, UINT32_C(0x80000000), big_endian);
            store_integer(bytes + node + 8U, 4U, UINT32_C(0x80000000), big_endian);
            store_integer(bytes + node + 16U, 4U, UINT32_MAX, big_endian);
            store_integer(bytes + node + 20U, 4U, UINT32_MAX, big_endian);
            memset(bytes + node + 24U, 0xa5, 8U);
            store_integer(bytes + 8U, 4U, 22U, true);
            store_integer(bytes + 16U, 8U, fixture.file.size - 48U, true);
            store_integer(bytes + 24U, 8U, fixture.file.size, true);
            store_integer(bytes + 32U, 8U, fixture.file.size, true);
            bytes[40U] = (uint8_t)endian;
            CHECK(schema_context_fixture_prepare(&fixture));
            SerializedFileSchema schema = {0};
            CHECK(schema_context_fixture_schema(&fixture, reference != 0U, 0U, &schema));
            CHECK(exhaustive_work(&schema, context_kind(reference != 0U), 1U, node));
            schema_context_fixture_dispose_parents(&fixture);
            SerializedFileSchemaContext output;
            CHECK(serialized_file_schema_context_query(
                      &schema, context_kind(reference != 0U), 3U, &output)
                      .status == SERIALIZED_FILE_SCHEMA_CONTEXT_OK);
            CHECK(output.first_child == SIZE_MAX && output.traversal_end == 1U &&
                output.registry == SIZE_MAX && output.registry_end == SIZE_MAX &&
                !output.registry_omitted && output.endian_selector == endian &&
                output.file_size == fixture.file.size);
            const SerializedFileSchemaNode* root = serialized_file_schema_node(&schema, 0U);
            CHECK(root && root->version == UINT16_MAX && root->type_flags == 0xf3U &&
                root->byte_size_bits == 0U && root->meta_flags == UINT32_MAX &&
                root->opaque_tail[0] == 0xa5U);
            common_file_bytes_dispose(&fixture.file);
            serialized_file_schema_dispose(&schema);
            CHECK(output.traversal_end == 1U && output.file_size == (reference ? 177U : 153U));
        }
    }
    return true;
}

static bool empty_selected_prefix(SchemaContextFixture* fixture) {
    uint8_t original_file[TAIL_FILE_BYTES];
    memcpy(original_file, fixture->file.data, sizeof(original_file));
    static const uint8_t levels[] = {0U, 1U, 2U, 3U, 4U, 4U};
    for (size_t ordinal = 0U; ordinal < TAIL_REFERENCE_COUNT; ++ordinal) {
        fixture->file.data[TAIL_REFERENCE_NODES + ordinal * NODE_BYTES + 2U] = levels[ordinal];
    }
    fixture->file.data[TAIL_REFERENCE_NODES + NODE_BYTES + 3U] = 4U;
    CHECK(schema_context_fixture_prepare(fixture));
    SerializedFileSchema schema = {0};
    CHECK(schema_context_fixture_schema(fixture, true, 0U, &schema));
    SerializedFileSchemaContext output;
    CHECK(serialized_file_schema_context_query(&schema, context_kind(true), 8U, &output).status ==
        SERIALIZED_FILE_SCHEMA_CONTEXT_OK);
    CHECK(output.first_child == 1U && output.traversal_end == 1U && output.registry == 1U &&
        output.registry_end == 6U && output.registry_omitted);
    serialized_file_schema_dispose(&schema);
    schema_context_fixture_dispose_parents(fixture);
    memcpy(fixture->file.data, original_file, sizeof(original_file));
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s TAIL_TREE\n", argv[0]);
        return EXIT_FAILURE;
    }
    SchemaContextFixture fixture = {0};
    const bool passed =
        schema_context_fixture_load(argv[1], TAIL_FILE_BYTES, tail_sha256, &fixture) &&
        argument_and_alias_boundaries(&fixture) && registry_mutations(&fixture) &&
        empty_selected_prefix(&fixture) && singleton_contexts() &&
        schema_context_fixture_hash_matches(&fixture, tail_sha256);
    schema_context_fixture_dispose(&fixture);
    if (!passed || g_allocations_count || g_allocated_bytes) {
        return EXIT_FAILURE;
    }
    puts(
        "Schema contexts: bounded traversal, unsupported placements, aliases and lifetimes passed");
    return EXIT_SUCCESS;
}
