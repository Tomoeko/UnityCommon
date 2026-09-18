#include "io/serialized_file_schema.h"

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
    FIXTURE_BYTES = 16384,
    MAX_FIXTURE_NODES = 256,
    NODE_BYTES = 32,
    OFFICIAL_COMMON_BYTES = 1170
};

typedef struct SchemaFixture {
    uint8_t bytes[FIXTURE_BYTES];
    size_t node_start;
    size_t strings_start;
    size_t end;
    size_t count;
    size_t string_size;
    bool reference;
    bool big_endian;
    bool no_tree;
} SchemaFixture;

typedef struct SchemaParents {
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
} SchemaParents;

static const SerializedFileSchemaLimits generous_limits = {
    MAX_FIXTURE_NODES, FIXTURE_BYTES, 255U, 1048576U, 1048576U, UINT64_C(1048576)};
static size_t allocation_ordinal;
static size_t failing_allocation;
static size_t schema_live_allocations;

void* schema_test_allocate(size_t size) {
    ++allocation_ordinal;
    if (failing_allocation && allocation_ordinal == failing_allocation) {
        return NULL;
    }
    void* allocation = malloc(size);
    if (allocation) {
        ++schema_live_allocations;
    }
    return allocation;
}

void schema_test_release(void* allocation) {
    if (allocation) {
        --schema_live_allocations;
        free(allocation);
    }
}

static void store_integer(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        bytes[big_endian ? width - index - 1U : index] = (uint8_t)value;
        value >>= 8U;
    }
}

static size_t fixture_type(SchemaFixture* fixture, size_t offset) {
    uint8_t* bytes = fixture->bytes;
    store_integer(bytes + offset, 4U, fixture->reference ? UINT32_MAX : 1U, fixture->big_endian);
    bytes[offset + 4U] = 0xa7U;
    store_integer(
        bytes + offset + 5U, 2U, fixture->reference ? 0U : UINT16_MAX, fixture->big_endian);
    offset += 7U;
    const size_t hashes = fixture->reference ? 32U : 16U;
    for (size_t index = 0U; index < hashes; ++index) {
        bytes[offset + index] = (uint8_t)(0x80U + index);
    }
    offset += hashes;
    if (fixture->no_tree) {
        return offset;
    }
    store_integer(bytes + offset, 4U, fixture->count, fixture->big_endian);
    store_integer(bytes + offset + 4U, 4U, fixture->string_size, fixture->big_endian);
    fixture->node_start = offset + 8U;
    for (size_t ordinal = 0U; ordinal < fixture->count; ++ordinal) {
        uint8_t* node = bytes + fixture->node_start + ordinal * NODE_BYTES;
        store_integer(node, 2U, 0x8100U + ordinal, fixture->big_endian);
        node[2] = ordinal ? 1U : 0U;
        node[3] = 0xffU;
        store_integer(node + 4U, 4U, 0U, fixture->big_endian);
        store_integer(node + 8U, 4U, 5U, fixture->big_endian);
        store_integer(node + 12U, 4U, UINT32_C(0x80000000) + ordinal, fixture->big_endian);
        store_integer(node + 16U, 4U, UINT32_MAX - ordinal, fixture->big_endian);
        store_integer(node + 20U, 4U, UINT32_C(0xfedcba98), fixture->big_endian);
        for (size_t byte = 0U; byte < 8U; ++byte) {
            node[24U + byte] = (uint8_t)(ordinal + byte + 1U);
        }
    }
    fixture->strings_start = fixture->node_start + fixture->count * NODE_BYTES;
    /* The unused final four bytes deliberately have no NUL. */
    memcpy(bytes + fixture->strings_start, "Node\0field\0tail", fixture->string_size);
    offset = fixture->strings_start + fixture->string_size;
    if (fixture->reference) {
        static const uint8_t names[] = "Payload\0Namespace\0Assembly\0";
        memcpy(bytes + offset, names, sizeof(names) - 1U);
        offset += sizeof(names) - 1U;
    } else {
        store_integer(bytes + offset, 4U, 0U, fixture->big_endian);
        offset += 4U;
    }
    return offset;
}

static void fixture_init(
    SchemaFixture* fixture, bool reference, bool big_endian, size_t count, bool no_tree) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->reference = reference;
    fixture->big_endian = big_endian;
    fixture->count = count;
    fixture->string_size = 15U;
    fixture->no_tree = no_tree;
    uint8_t* bytes = fixture->bytes;
    memcpy(bytes + 48U, "2021.3.35f1", 12U);
    bytes[64U] = no_tree ? 0U : 1U;
    store_integer(bytes + 65U, 4U, reference ? 0U : 1U, big_endian);
    size_t offset = reference ? 69U : fixture_type(fixture, 69U);
    store_integer(bytes + offset, 4U, 0U, big_endian);
    offset += 4U;
    store_integer(bytes + offset, 4U, 0U, big_endian);
    store_integer(bytes + offset + 4U, 4U, 0U, big_endian);
    store_integer(bytes + offset + 8U, 4U, reference ? 1U : 0U, big_endian);
    offset += 12U;
    if (reference) {
        offset = fixture_type(fixture, offset);
    }
    bytes[offset++] = 0U;
    fixture->end = offset;
    store_integer(bytes + 8U, 4U, 22U, true);
    store_integer(bytes + 16U, 8U, offset - 48U, true);
    store_integer(bytes + 24U, 8U, offset, true);
    store_integer(bytes + 32U, 8U, offset, true);
    bytes[40U] = big_endian ? 1U : 0U;
}

static bool prepare_parents(const SchemaFixture* fixture, SchemaParents* parents) {
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 2U, 0U, MAX_FIXTURE_NODES, FIXTURE_BYTES, 0U, FIXTURE_BYTES, 1048576U, 0U, 1048576U};
    const bool exact29 = fixture->bytes[55U] == '2';
    const SerializedFileDirectoryEngineVersion engine = exact29
        ? SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1
        : SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1;
    const SerializedFileDirectoryResult directory = serialized_file_directory_create(
        fixture->bytes, fixture->end, fixture->end, engine, &directory_limits, &parents->directory);
    CHECK(directory.status == SERIALIZED_FILE_DIRECTORY_OK);
    if (fixture->reference) {
        const SerializedFileMetadataTailLimits tail_limits = {0U,
            0U,
            2U,
            MAX_FIXTURE_NODES,
            FIXTURE_BYTES,
            1024U,
            FIXTURE_BYTES,
            1048576U,
            0U,
            1048576U};
        CHECK(serialized_file_metadata_tail_create(&parents->directory,
                  fixture->bytes,
                  fixture->end,
                  fixture->end,
                  &tail_limits,
                  &parents->tail)
                  .status == SERIALIZED_FILE_METADATA_TAIL_OK);
    }
    return true;
}

static void dispose_parents(SchemaParents* parents) {
    serialized_file_metadata_tail_dispose(&parents->tail);
    serialized_file_directory_dispose(&parents->directory);
}

static SerializedFileSchemaResult create_schema(const SchemaFixture* fixture,
    const SchemaParents* parents,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* output) {
    return fixture->reference
        ? serialized_file_schema_create_reference(&parents->tail, 0U, limits, output)
        : serialized_file_schema_create_ordinary(&parents->directory, 0U, limits, output);
}

static bool check_raw_nodes(const SchemaFixture* fixture, const SerializedFileSchema* schema) {
    const SerializedFileSchemaView* view = serialized_file_schema_view(schema);
    CHECK(view && view->node_count == fixture->count && view->type_ordinal == 0U &&
        view->maximum_depth == (fixture->count > 1U ? 1U : 0U));
    for (size_t ordinal = 0U; ordinal < fixture->count; ++ordinal) {
        const SerializedFileSchemaNode* node = serialized_file_schema_node(schema, ordinal);
        CHECK(node && node->ordinal == ordinal &&
            node->source.data == fixture->bytes + fixture->node_start + ordinal * NODE_BYTES &&
            node->source.offset == fixture->node_start + ordinal * NODE_BYTES &&
            node->source.size == NODE_BYTES && node->version == 0x8100U + ordinal &&
            node->type_flags == 0xffU && node->byte_size_bits == UINT32_C(0x80000000) + ordinal &&
            node->index_bits == UINT32_MAX - ordinal && node->meta_flags == UINT32_C(0xfedcba98));
        CHECK(!memcmp(node->opaque_tail, node->source.data + 24U, 8U) &&
            node->type_name.space == SERIALIZED_FILE_SCHEMA_STRING_LOCAL &&
            node->type_name.encoded_offset == 0U && node->type_name.byte_count == 4U &&
            node->type_name.bytes == fixture->bytes + fixture->strings_start &&
            node->type_name.source_offset == fixture->strings_start &&
            node->field_name.byte_count == 5U &&
            node->field_name.bytes == fixture->bytes + fixture->strings_start + 5U);
        if (!ordinal) {
            CHECK(node->parent == SIZE_MAX && node->next_sibling == SIZE_MAX &&
                node->first_child == (fixture->count > 1U ? 1U : SIZE_MAX) &&
                node->child_count == fixture->count - 1U && node->subtree_end == fixture->count);
        } else {
            CHECK(node->parent == 0U && node->first_child == SIZE_MAX && !node->child_count &&
                node->next_sibling == (ordinal + 1U < fixture->count ? ordinal + 1U : SIZE_MAX) &&
                node->subtree_end == ordinal + 1U);
        }
    }
    CHECK(!serialized_file_schema_node(schema, fixture->count) &&
        !serialized_file_schema_node(schema, SIZE_MAX));
    return true;
}

static bool raw_orders_and_lifetime(void) {
    for (size_t mode = 0U; mode < 4U; ++mode) {
        SchemaFixture fixture;
        fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 4U, false);
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        allocation_ordinal = 0U;
        const SerializedFileSchemaResult result =
            create_schema(&fixture, &parents, &generous_limits, &schema);
        CHECK(result.status == SERIALIZED_FILE_SCHEMA_OK && allocation_ordinal == 2U &&
            schema_live_allocations == 1U &&
            result.peak_retained_bytes == result.required_retained_bytes &&
            result.peak_scratch_bytes == result.required_scratch_bytes &&
            result.work_used ==
                result.required_retained_bytes + result.required_scratch_bytes +
                    52U * fixture.count + fixture.string_size + OFFICIAL_COMMON_BYTES + 2051U &&
            check_raw_nodes(&fixture, &schema));
        dispose_parents(&parents);
        CHECK(check_raw_nodes(&fixture, &schema));
        serialized_file_schema_dispose(&schema);
        serialized_file_schema_dispose(&schema);
        CHECK(!schema_live_allocations && !g_allocations_count && !g_allocated_bytes);
    }
    return true;
}

static bool expect_status(SchemaFixture* fixture,
    SerializedFileSchemaStatus status,
    SerializedFileSchemaLimit limit,
    size_t expected_node) {
    SchemaParents parents = {0};
    CHECK(prepare_parents(fixture, &parents));
    SerializedFileSchema schema = {0};
    const SerializedFileSchemaResult result =
        create_schema(fixture, &parents, &generous_limits, &schema);
    CHECK(result.status == status && result.limit == limit &&
        result.node_ordinal == expected_node && !schema.implementation && !schema_live_allocations);
    dispose_parents(&parents);
    return true;
}

static bool string_boundaries(void) {
    const uint32_t offsets[] = {
        1U, 15U, 11U, UINT32_C(0x80000490), UINT32_C(0x80000492), UINT32_MAX};
    for (size_t mode = 0U; mode < 4U; ++mode) {
        for (size_t index = 0U; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
            SchemaFixture fixture;
            fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 1U, false);
            store_integer(
                fixture.bytes + fixture.node_start + 4U, 4U, offsets[index], fixture.big_endian);
            CHECK(expect_status(&fixture,
                index == 2U ? SERIALIZED_FILE_SCHEMA_UNTERMINATED_STRING
                            : SERIALIZED_FILE_SCHEMA_INVALID_STRING_OFFSET,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
                0U));
        }
        SchemaFixture fixture;
        fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 1U, false);
        store_integer(
            fixture.bytes + fixture.node_start + 4U, 4U, UINT32_C(0x80000491), fixture.big_endian);
        store_integer(
            fixture.bytes + fixture.node_start + 8U, 4U, UINT32_C(0x80000000), fixture.big_endian);
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        CHECK(create_schema(&fixture, &parents, &generous_limits, &schema).status ==
            SERIALIZED_FILE_SCHEMA_OK);
        const SerializedFileSchemaNode* node = serialized_file_schema_node(&schema, 0U);
        CHECK(node && node->type_name.space == SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35 &&
            node->type_name.source_offset == 1169U && !node->type_name.byte_count &&
            node->type_name.bytes[0] == 0U && node->field_name.byte_count == 4U &&
            !memcmp(node->field_name.bytes, "AABB", 5U));
        serialized_file_schema_dispose(&schema);
        dispose_parents(&parents);
    }
    return true;
}

static bool hierarchy_bounds(void) {
    for (size_t mode = 0U; mode < 4U; ++mode) {
        for (size_t scenario = 0U; scenario < 3U; ++scenario) {
            SchemaFixture fixture;
            fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 3U, false);
            const size_t node = scenario ? 1U : 0U;
            fixture.bytes[fixture.node_start + NODE_BYTES * node + 2U] =
                scenario == 1U ? 0U : (scenario == 2U ? 2U : 1U);
            CHECK(expect_status(&fixture,
                SERIALIZED_FILE_SCHEMA_MALFORMED_HIERARCHY,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
                node));
        }
        SchemaFixture fixture;
        fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 256U, false);
        for (size_t ordinal = 0U; ordinal < 256U; ++ordinal) {
            fixture.bytes[fixture.node_start + NODE_BYTES * ordinal + 2U] = (uint8_t)ordinal;
        }
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        CHECK(create_schema(&fixture, &parents, &generous_limits, &schema).status ==
            SERIALIZED_FILE_SCHEMA_OK);
        CHECK(serialized_file_schema_view(&schema)->maximum_depth == 255U);
        for (size_t ordinal = 0U; ordinal < 256U; ++ordinal) {
            const SerializedFileSchemaNode* node = serialized_file_schema_node(&schema, ordinal);
            CHECK(node && node->parent == (ordinal ? ordinal - 1U : SIZE_MAX) &&
                node->first_child == (ordinal < 255U ? ordinal + 1U : SIZE_MAX) &&
                node->next_sibling == SIZE_MAX && node->subtree_end == 256U);
        }
        serialized_file_schema_dispose(&schema);
        SerializedFileSchemaLimits limits = generous_limits;
        limits.max_depth = 254U;
        const SerializedFileSchemaResult result =
            create_schema(&fixture, &parents, &limits, &schema);
        CHECK(result.status == SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED &&
            result.limit == SERIALIZED_FILE_SCHEMA_LIMIT_DEPTH && result.node_ordinal == 255U &&
            !schema.implementation && !schema_live_allocations);
        dispose_parents(&parents);
    }
    return true;
}

static bool mixed_hierarchy_and_empty_names(void) {
    static const uint8_t levels[] = {0U, 1U, 2U, 1U, 2U, 2U, 1U};
    static const size_t parents_expected[] = {SIZE_MAX, 0U, 1U, 0U, 3U, 3U, 0U};
    static const size_t first_children[] = {1U, 2U, SIZE_MAX, 4U, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    static const size_t siblings[] = {SIZE_MAX, 3U, SIZE_MAX, 6U, 5U, SIZE_MAX, SIZE_MAX};
    static const size_t ends[] = {7U, 3U, 3U, 6U, 5U, 6U, 7U};
    for (size_t mode = 0U; mode < 4U; ++mode) {
        SchemaFixture fixture;
        fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 7U, false);
        for (size_t ordinal = 0U; ordinal < 7U; ++ordinal) {
            fixture.bytes[fixture.node_start + NODE_BYTES * ordinal + 2U] = levels[ordinal];
        }
        fixture.bytes[fixture.strings_start] = 0U;
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        CHECK(create_schema(&fixture, &parents, &generous_limits, &schema).status ==
            SERIALIZED_FILE_SCHEMA_OK);
        for (size_t ordinal = 0U; ordinal < 7U; ++ordinal) {
            const SerializedFileSchemaNode* node = serialized_file_schema_node(&schema, ordinal);
            CHECK(node && node->parent == parents_expected[ordinal] &&
                node->first_child == first_children[ordinal] &&
                node->next_sibling == siblings[ordinal] && node->subtree_end == ends[ordinal] &&
                !node->type_name.byte_count && node->type_name.bytes[0] == 0U &&
                node->type_name.space == SERIALIZED_FILE_SCHEMA_STRING_LOCAL);
        }
        serialized_file_schema_dispose(&schema);
        dispose_parents(&parents);

        fixture_init(&fixture, (mode & 1U) != 0U, (mode & 2U) != 0U, 1U, false);
        store_integer(
            fixture.bytes + fixture.node_start + 4U, 4U, UINT32_C(0x80000000), fixture.big_endian);
        store_integer(
            fixture.bytes + fixture.node_start + 8U, 4U, UINT32_C(0x80000000), fixture.big_endian);
        memmove(fixture.bytes + fixture.strings_start,
            fixture.bytes + fixture.strings_start + fixture.string_size,
            fixture.end - fixture.strings_start - fixture.string_size);
        fixture.end -= fixture.string_size;
        fixture.string_size = 0U;
        store_integer(fixture.bytes + fixture.node_start - 4U, 4U, 0U, fixture.big_endian);
        store_integer(fixture.bytes + 16U, 8U, fixture.end - 48U, true);
        store_integer(fixture.bytes + 24U, 8U, fixture.end, true);
        store_integer(fixture.bytes + 32U, 8U, fixture.end, true);
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchemaLimits limits = generous_limits;
        limits.max_depth = 0U;
        limits.max_local_string_bytes = 0U;
        CHECK(create_schema(&fixture, &parents, &limits, &schema).status ==
            SERIALIZED_FILE_SCHEMA_OK);
        const SerializedFileSchemaView* view = serialized_file_schema_view(&schema);
        CHECK(view && !view->maximum_depth && !view->tree.strings_source.size &&
            view->tree.strings_source.data == fixture.bytes + fixture.strings_start);
        serialized_file_schema_dispose(&schema);
        dispose_parents(&parents);
    }
    return true;
}

static bool limits_and_allocations(void) {
    for (size_t reference = 0U; reference < 2U; ++reference) {
        SchemaFixture fixture;
        fixture_init(&fixture, reference != 0U, false, 4U, false);
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        const SerializedFileSchemaResult baseline =
            create_schema(&fixture, &parents, &generous_limits, &schema);
        CHECK(baseline.status == SERIALIZED_FILE_SCHEMA_OK);
        serialized_file_schema_dispose(&schema);
        const size_t parent_allocations = g_allocations_count;
        const size_t parent_bytes = g_allocated_bytes;
        SerializedFileSchemaLimits exact = {fixture.count,
            fixture.string_size,
            1U,
            baseline.required_retained_bytes,
            baseline.required_scratch_bytes,
            baseline.work_used};
        CHECK(
            create_schema(&fixture, &parents, &exact, &schema).status == SERIALIZED_FILE_SCHEMA_OK);
        serialized_file_schema_dispose(&schema);
        for (size_t index = 0U; index < 5U; ++index) {
            SerializedFileSchemaLimits limits = exact;
            SerializedFileSchemaLimit expected;
            switch (index) {
            case 0U:
                --limits.max_nodes;
                expected = SERIALIZED_FILE_SCHEMA_LIMIT_NODES;
                break;
            case 1U:
                --limits.max_local_string_bytes;
                expected = SERIALIZED_FILE_SCHEMA_LIMIT_LOCAL_STRINGS;
                break;
            case 2U:
                --limits.max_depth;
                expected = SERIALIZED_FILE_SCHEMA_LIMIT_DEPTH;
                break;
            case 3U:
                --limits.max_retained_bytes;
                expected = SERIALIZED_FILE_SCHEMA_LIMIT_RETAINED;
                break;
            default:
                --limits.max_scratch_bytes;
                expected = SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH;
                break;
            }
            const SerializedFileSchemaResult result =
                create_schema(&fixture, &parents, &limits, &schema);
            CHECK(result.status == SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED &&
                result.limit == expected && !schema.implementation && !schema_live_allocations);
        }
        for (uint64_t work = 0U; work < baseline.work_used; ++work) {
            SerializedFileSchemaLimits limits = exact;
            limits.max_work = work;
            const SerializedFileSchemaResult result =
                create_schema(&fixture, &parents, &limits, &schema);
            CHECK(result.status == SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED &&
                result.limit == SERIALIZED_FILE_SCHEMA_LIMIT_WORK && result.work_used <= work &&
                !schema.implementation && !schema_live_allocations &&
                g_allocations_count == parent_allocations && g_allocated_bytes == parent_bytes);
        }
        for (size_t failure = 1U; failure <= 2U; ++failure) {
            allocation_ordinal = 0U;
            failing_allocation = failure;
            const SerializedFileSchemaResult result =
                create_schema(&fixture, &parents, &exact, &schema);
            failing_allocation = 0U;
            CHECK(result.status == SERIALIZED_FILE_SCHEMA_ALLOCATION_FAILED &&
                allocation_ordinal == failure &&
                result.peak_retained_bytes ==
                    (failure == 2U ? baseline.required_retained_bytes : 0U) &&
                !result.peak_scratch_bytes && !schema.implementation && !schema_live_allocations &&
                g_allocations_count == parent_allocations && g_allocated_bytes == parent_bytes);
        }
        dispose_parents(&parents);
    }
    return true;
}

static bool arguments_and_absence(void) {
    SerializedFileSchema schema = {0};
    SerializedFileDirectory empty = {0};
    CHECK(serialized_file_schema_create_ordinary(NULL, 0U, &generous_limits, &schema).status ==
        SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT);
    CHECK(serialized_file_schema_create_ordinary(&empty, 0U, &generous_limits, &schema).status ==
        SERIALIZED_FILE_SCHEMA_INVALID_STATE);
    CHECK(!serialized_file_schema_view(NULL) && !serialized_file_schema_view(&schema) &&
        !serialized_file_schema_node(NULL, 0U));
    serialized_file_schema_init(NULL);
    serialized_file_schema_dispose(NULL);
    for (size_t reference = 0U; reference < 2U; ++reference) {
        SchemaFixture fixture;
        fixture_init(&fixture, reference != 0U, false, 1U, true);
        CHECK(expect_status(&fixture,
            SERIALIZED_FILE_SCHEMA_NO_EMBEDDED_TREE,
            SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
            SIZE_MAX));
        fixture_init(&fixture, reference != 0U, false, 1U, false);
        memcpy(fixture.bytes + 48U, "2021.3.29f1", 12U);
        CHECK(expect_status(&fixture,
            SERIALIZED_FILE_SCHEMA_UNSUPPORTED_ENGINE,
            SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
            SIZE_MAX));
    }
    return true;
}

static bool aliases_preserve_parents(void) {
    for (size_t reference = 0U; reference < 2U; ++reference) {
        SchemaFixture fixture;
        fixture_init(&fixture, reference != 0U, false, 4U, false);
        SchemaFixture original = fixture;
        SchemaParents parents = {0};
        CHECK(prepare_parents(&fixture, &parents));
        SerializedFileSchema schema = {0};
        const void* row = reference
            ? (const void*)serialized_file_metadata_tail_reference_type(&parents.tail, 0U)
            : (const void*)serialized_file_directory_type(&parents.directory, 0U);
        SerializedFileSchema* outputs[] = {(SerializedFileSchema*)(void*)fixture.bytes,
            reference ? (SerializedFileSchema*)(void*)&parents.tail
                      : (SerializedFileSchema*)(void*)&parents.directory,
            (SerializedFileSchema*)(void*)row,
            (SerializedFileSchema*)(void*)&generous_limits};
        for (size_t index = 0U; index < sizeof(outputs) / sizeof(outputs[0]); ++index) {
            const SerializedFileSchemaResult result =
                create_schema(&fixture, &parents, &generous_limits, outputs[index]);
            CHECK(result.status == SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT && !result.work_used &&
                !memcmp(&fixture, &original, sizeof(fixture)));
        }
        CHECK(create_schema(&fixture,
                  &parents,
                  (const SerializedFileSchemaLimits*)(const void*)fixture.bytes,
                  &schema)
                  .status == SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT);
        CHECK(create_schema(&fixture, &parents, &generous_limits, &schema).status ==
            SERIALIZED_FILE_SCHEMA_OK);
        const void* storage = schema.implementation;
        CHECK(create_schema(&fixture, &parents, &generous_limits, &schema).status ==
                SERIALIZED_FILE_SCHEMA_INVALID_STATE &&
            schema.implementation == storage);
        serialized_file_schema_dispose(&schema);
        dispose_parents(&parents);
    }
    return true;
}

static bool dispose_after_backing_release(void) {
    SchemaFixture* fixture = malloc(sizeof(*fixture));
    CHECK(fixture);
    fixture_init(fixture, true, false, 4U, false);
    SchemaParents parents = {0};
    CHECK(prepare_parents(fixture, &parents));
    SerializedFileSchema schema = {0};
    CHECK(create_schema(fixture, &parents, &generous_limits, &schema).status ==
        SERIALIZED_FILE_SCHEMA_OK);
    dispose_parents(&parents);
    free(fixture);
    serialized_file_schema_dispose(&schema);
    CHECK(!schema_live_allocations);
    return true;
}

#ifndef _WIN32
static bool guarded_backing_and_publication(void) {
    const long page_size_result = sysconf(_SC_PAGESIZE);
    CHECK(page_size_result > 0);
    const size_t page_size = (size_t)page_size_result;
    SchemaFixture fixture;
    fixture_init(&fixture, false, false, 4U, false);
    CHECK(fixture.end < page_size);
    uint8_t* source_pages =
        mmap(NULL, page_size * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(source_pages != MAP_FAILED);
    uint8_t* output_pages =
        mmap(NULL, page_size * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(output_pages != MAP_FAILED);
    CHECK(mprotect(source_pages + page_size, page_size, PROT_NONE) == 0 &&
        mprotect(output_pages + page_size, page_size, PROT_NONE) == 0);
    uint8_t* source = source_pages + page_size - fixture.end;
    memcpy(source, fixture.bytes, fixture.end);
    SerializedFileSchema* output =
        (SerializedFileSchema*)(void*)(output_pages + page_size - sizeof(*output));
    serialized_file_schema_init(output);
    SerializedFileDirectory directory = {0};
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 2U, 0U, MAX_FIXTURE_NODES, FIXTURE_BYTES, 0U, FIXTURE_BYTES, 1048576U, 0U, 1048576U};
    CHECK(serialized_file_directory_create(source,
              fixture.end,
              fixture.end,
              SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
              &directory_limits,
              &directory)
              .status == SERIALIZED_FILE_DIRECTORY_OK);
    CHECK(serialized_file_schema_create_ordinary(&directory, 0U, &generous_limits, output).status ==
        SERIALIZED_FILE_SCHEMA_OK);
    serialized_file_directory_dispose(&directory);
    CHECK(munmap(source_pages, page_size * 2U) == 0);
    serialized_file_schema_dispose(output);
    CHECK(!output->implementation && munmap(output_pages, page_size * 2U) == 0 &&
        !schema_live_allocations);
    return true;
}
#endif

int main(void) {
    if (!raw_orders_and_lifetime() || !string_boundaries() || !hierarchy_bounds() ||
        !mixed_hierarchy_and_empty_names() || !limits_and_allocations() ||
        !arguments_and_absence() || !aliases_preserve_parents() ||
        !dispose_after_backing_release() || schema_live_allocations || g_allocations_count ||
        g_allocated_bytes) {
        return EXIT_FAILURE;
    }
#ifndef _WIN32
    if (!guarded_backing_and_publication()) {
        return EXIT_FAILURE;
    }
#endif
    puts(
        "Serialized schemas: raw nodes, tagged strings, hierarchy, exact limits and ownership passed");
    return EXIT_SUCCESS;
}
