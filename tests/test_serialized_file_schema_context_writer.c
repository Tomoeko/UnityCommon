#include "io/serialized_file_schema_context.h"

#include "serialized_schema_context_fixture.h"

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

typedef struct ExpectedTree {
    bool reference;
    size_t ordinal;
    size_t row_offset;
    size_t row_size;
    size_t node_count_offset;
    size_t string_count_offset;
    size_t nodes_offset;
    size_t strings_offset;
    size_t node_count;
    size_t string_bytes;
    size_t registry;
    size_t traversal_end;
} ExpectedTree;

typedef struct ExpectedFixture {
    size_t bytes;
    const char* sha256;
    size_t tree_count;
    ExpectedTree trees[4];
} ExpectedFixture;

/* Expected coordinates/boundaries were independently decoded from the three
 * whole-file pins. Product Directory/Tail/Schema calls provide setup only.
 * Every tree is covered, including TailPayload's natural child-list end and
 * the two newer reference trees' final registry boundaries. */
static const ExpectedFixture fixtures[] = {
    {4456U,
        "58955a4e1cb8c769315fab4a96483094263adaa0bd37771dd7e8468d552cf7f6",
        4U,
        {{false, 0U, 69U, 1378U, 92U, 96U, 100U, 1220U, 35U, 223U, SIZE_MAX, 35U},
            {false, 1U, 1447U, 323U, 1470U, 1474U, 1478U, 1766U, 9U, 0U, SIZE_MAX, 9U},
            {false, 2U, 1770U, 492U, 1809U, 1813U, 1817U, 2233U, 13U, 25U, SIZE_MAX, 13U},
            {false, 3U, 2262U, 492U, 2301U, 2305U, 2309U, 2725U, 13U, 25U, SIZE_MAX, 13U}}},
    {4336U,
        "0a2dbcb88979c18afb5331524fe5184f44af13c847279f9cefb514d6f585fea0",
        3U,
        {{false, 0U, 69U, 323U, 92U, 96U, 100U, 388U, 9U, 0U, SIZE_MAX, 9U},
            {false, 1U, 392U, 1510U, 431U, 435U, 439U, 1687U, 39U, 207U, 18U, 39U},
            {true, 0U, 2022U, 316U, 2061U, 2065U, 2069U, 2261U, 6U, 25U, SIZE_MAX, 6U}}},
    {6528U,
        "0165c78b5ad8bbc458db995641964ae915adc61fa29e2de83709f29c9094d191",
        3U,
        {{false, 0U, 69U, 1734U, 108U, 112U, 116U, 1588U, 46U, 203U, 25U, 46U},
            {true, 0U, 2102U, 1209U, 2141U, 2145U, 2149U, 3077U, 29U, 177U, 8U, 8U},
            {true, 1U, 3311U, 1113U, 3350U, 3354U, 3358U, 4190U, 26U, 178U, 5U, 5U}}}};

static bool source_matches(
    SerializedFilePrefixSpan actual, const uint8_t* bytes, size_t offset, size_t size) {
    CHECK(actual.data == bytes + offset && actual.offset == offset && actual.size == size);
    return true;
}

static bool context_matches(const SerializedFileSchemaContext* actual,
    const ExpectedFixture* fixture,
    const ExpectedTree* tree,
    const uint8_t* bytes) {
    CHECK(actual->kind ==
        (tree->reference ? SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD
                         : SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT));
    CHECK(actual->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 &&
        actual->row_kind ==
            (tree->reference ? SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE
                             : SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE) &&
        actual->type_ordinal == tree->ordinal && actual->file_size == fixture->bytes &&
        actual->endian_selector == 0U);
    CHECK(source_matches(actual->header_source, bytes, 0U, 48U));
    CHECK(source_matches(actual->type_entry_source, bytes, tree->row_offset, tree->row_size));
    CHECK(source_matches(actual->tree.node_count_source, bytes, tree->node_count_offset, 4U));
    CHECK(source_matches(actual->tree.string_count_source, bytes, tree->string_count_offset, 4U));
    CHECK(source_matches(
        actual->tree.nodes_source, bytes, tree->nodes_offset, tree->node_count * 32U));
    CHECK(source_matches(
        actual->tree.strings_source, bytes, tree->strings_offset, tree->string_bytes));
    CHECK(actual->tree.node_count == tree->node_count &&
        actual->tree.string_byte_count == tree->string_bytes && actual->root == 0U &&
        actual->first_child == 1U && actual->traversal_end == tree->traversal_end &&
        actual->registry == tree->registry &&
        actual->registry_end == (tree->registry == SIZE_MAX ? SIZE_MAX : tree->node_count) &&
        actual->registry_omitted == (tree->reference && tree->registry != SIZE_MAX));
    return true;
}

static bool query_tree(const SerializedFileSchema* schema,
    const ExpectedFixture* fixture,
    const ExpectedTree* tree,
    const uint8_t* bytes,
    SerializedFileSchemaContext* out_context) {
    const SerializedFileSchemaContextKind kind = tree->reference
        ? SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD
        : SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT;
    const SerializedFileSchemaContextResult result =
        serialized_file_schema_context_query(schema, kind, tree->node_count + 2U, out_context);
    CHECK(result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_OK &&
        result.work_used == tree->node_count + 2U && result.type_ordinal == tree->ordinal &&
        result.field == SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE &&
        result.node_ordinal == SIZE_MAX && result.error_offset == UINT64_MAX);
    CHECK(context_matches(out_context, fixture, tree, bytes));

    unsigned char before[sizeof(*out_context)];
    memcpy(before, out_context, sizeof(before));
    const SerializedFileSchemaContextKind wrong = tree->reference
        ? SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT
        : SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD;
    const SerializedFileSchemaContextResult rejected =
        serialized_file_schema_context_query(schema, wrong, 0U, out_context);
    CHECK(rejected.status == SERIALIZED_FILE_SCHEMA_CONTEXT_WRONG_ROW_KIND &&
        rejected.work_used == 0U && rejected.type_ordinal == tree->ordinal &&
        rejected.field == SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE &&
        rejected.node_ordinal == SIZE_MAX && rejected.error_offset == UINT64_MAX &&
        memcmp(before, out_context, sizeof(before)) == 0);
    return true;
}

static bool check_fixture(const char* path, const ExpectedFixture* expected) {
    SchemaContextFixture fixture = {0};
    SerializedFileSchema schemas[4] = {{0}};
    SerializedFileSchemaContext contexts[4];
    bool passed = false;
    if (!schema_context_fixture_load(path, expected->bytes, expected->sha256, &fixture) ||
        !schema_context_fixture_prepare(&fixture)) {
        goto cleanup_fixture;
    }
    for (size_t index = 0U; index < expected->tree_count; ++index) {
        const ExpectedTree* tree = &expected->trees[index];
        if (!schema_context_fixture_schema(
                &fixture, tree->reference, tree->ordinal, &schemas[index]) ||
            !query_tree(&schemas[index], expected, tree, fixture.file.data, &contexts[index])) {
            goto cleanup_fixture;
        }
    }
    schema_context_fixture_dispose_parents(&fixture);
    for (size_t index = 0U; index < expected->tree_count; ++index) {
        if (!query_tree(&schemas[index],
                expected,
                &expected->trees[index],
                fixture.file.data,
                &contexts[index])) {
            goto cleanup_fixture;
        }
        serialized_file_schema_dispose(&schemas[index]);
        if (!context_matches(
                &contexts[index], expected, &expected->trees[index], fixture.file.data)) {
            goto cleanup_fixture;
        }
    }
    passed = schema_context_fixture_hash_matches(&fixture, expected->sha256);

cleanup_fixture:
    for (size_t index = 0U; index < 4U; ++index) {
        serialized_file_schema_dispose(&schemas[index]);
    }
    schema_context_fixture_dispose(&fixture);
    CHECK(passed);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s DIRECTORY_TREE TAIL_TREE REGISTRY_TREE\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (size_t index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        if (!check_fixture(argv[index + 1U], &fixtures[index])) {
            return EXIT_FAILURE;
        }
    }
    puts(
        "Official schema contexts: all 10 trees and 225 nodes retain expected traversal boundaries");
    return EXIT_SUCCESS;
}
