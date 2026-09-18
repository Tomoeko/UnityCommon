#include "io/serialized_file_directory.h"

#include "common/common.h"

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
    DIRECTORY_FIXTURE_SIZE = 512,
    DIRECTORY_NO_TREE_END = 208,
    DIRECTORY_TREE_END = 316,
    DIRECTORY_PREFIX_END = 65,
    DIRECTORY_CANARY_SIZE = 16
};

static const SerializedFileDirectoryLimits generous_limits = {
    12U, 16U, 16U, 64U, 512U, 64U, 512U, 65536U, 0U, UINT64_C(1048576)};

typedef struct DirectoryFixture {
    uint8_t bytes[DIRECTORY_FIXTURE_SIZE];
    size_t directory_end;
    uint64_t logical_size;
    bool big_endian;
} DirectoryFixture;

typedef struct GuardedDirectory {
    uint8_t before[DIRECTORY_CANARY_SIZE];
    SerializedFileDirectory value;
    uint8_t after[DIRECTORY_CANARY_SIZE];
} GuardedDirectory;

/* Fixture encoders write only fixed-width wire scalars. Row layout and optional
 * fields are explicit below; no production writer or parser supplies offsets.
 */
static void store_integer(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        const size_t destination = big_endian ? width - index - 1U : index;
        bytes[destination] = (uint8_t)value;
        value >>= 8U;
    }
}

static void header_fixture(DirectoryFixture* fixture,
    bool big_endian,
    uint8_t tree_flag,
    size_t directory_end,
    uint64_t metadata_end,
    uint64_t data_origin,
    uint64_t logical_size) {
    memset(fixture, 0, sizeof(*fixture));
    memset(fixture->bytes, 0xe7, sizeof(fixture->bytes));
    memset(fixture->bytes, 0, 48U);
    fixture->big_endian = big_endian;
    fixture->directory_end = directory_end;
    fixture->logical_size = logical_size;
    store_integer(fixture->bytes + 8U, 4U, 22U, true);
    store_integer(fixture->bytes + 16U, 8U, metadata_end - 48U, true);
    store_integer(fixture->bytes + 24U, 8U, logical_size, true);
    store_integer(fixture->bytes + 32U, 8U, data_origin, true);
    fixture->bytes[40] = big_endian ? 1U : 0U;
    memcpy(fixture->bytes + 48U, "2021.3.35f1", 12U);
    store_integer(fixture->bytes + 60U, 4U, UINT32_C(0x89abcdef), big_endian);
    fixture->bytes[64] = tree_flag;
}

static void type_prefix(DirectoryFixture* fixture,
    size_t start,
    uint32_t class_id,
    uint8_t stripped,
    uint16_t script_index,
    bool include_hash) {
    store_integer(fixture->bytes + start, 4U, class_id, fixture->big_endian);
    fixture->bytes[start + 4U] = stripped;
    store_integer(fixture->bytes + start + 5U, 2U, script_index, fixture->big_endian);
    size_t type_hash = start + 7U;
    if (include_hash) {
        for (size_t index = 0U; index < 16U; ++index) {
            fixture->bytes[type_hash + index] = (uint8_t)(0xa0U + index);
        }
        type_hash += 16U;
    }
    for (size_t index = 0U; index < 16U; ++index) {
        fixture->bytes[type_hash + index] = (uint8_t)(0x70U + index);
    }
}

static void object_row(DirectoryFixture* fixture,
    size_t start,
    uint64_t path_id,
    uint64_t relative_offset,
    uint32_t size,
    uint32_t type_ordinal) {
    store_integer(fixture->bytes + start, 8U, path_id, fixture->big_endian);
    store_integer(fixture->bytes + start + 8U, 8U, relative_offset, fixture->big_endian);
    store_integer(fixture->bytes + start + 16U, 4U, size, fixture->big_endian);
    store_integer(fixture->bytes + start + 20U, 4U, type_ordinal, fixture->big_endian);
}

static void no_tree_fixture(DirectoryFixture* fixture, bool big_endian) {
    header_fixture(fixture, big_endian, 0U, DIRECTORY_NO_TREE_END, 232U, 256U, 320U);
    store_integer(fixture->bytes + 65U, 4U, 2U, big_endian);
    type_prefix(fixture, 69U, UINT32_C(0x87654321), 0xa6U, UINT16_MAX, false);
    type_prefix(fixture, 92U, 114U, 0xffU, UINT16_C(0x8000), true);
    store_integer(fixture->bytes + 131U, 4U, 3U, big_endian);
    fixture->bytes[135U] = 0xd3U;
    object_row(fixture, 136U, UINT64_MAX, 8U, 12U, 1U);
    object_row(fixture, 160U, UINT64_MAX, 12U, 8U, 0U);
    object_row(fixture, 184U, UINT64_C(0x0102030405060708), 64U, 0U, 1U);
}

static void tree_fixture(DirectoryFixture* fixture, bool big_endian) {
    header_fixture(fixture, big_endian, 1U, DIRECTORY_TREE_END, 340U, 384U, 512U);
    store_integer(fixture->bytes + 65U, 4U, 2U, big_endian);
    type_prefix(fixture, 69U, 49U, 2U, UINT16_MAX, false);
    store_integer(fixture->bytes + 92U, 4U, 1U, big_endian);
    store_integer(fixture->bytes + 96U, 4U, 3U, big_endian);
    for (size_t index = 100U; index < 132U; ++index) {
        fixture->bytes[index] = (uint8_t)(index ^ 0xa5U);
    }
    fixture->bytes[132U] = 0xffU;
    fixture->bytes[133U] = 0U;
    fixture->bytes[134U] = 0x80U;
    store_integer(fixture->bytes + 135U, 4U, 2U, big_endian);
    store_integer(fixture->bytes + 139U, 4U, UINT32_MAX, big_endian);
    store_integer(fixture->bytes + 143U, 4U, UINT32_C(0x87654321), big_endian);
    type_prefix(fixture, 147U, 48U, 0xfeU, 0U, true);
    store_integer(fixture->bytes + 186U, 4U, 2U, big_endian);
    store_integer(fixture->bytes + 190U, 4U, 0U, big_endian);
    for (size_t index = 194U; index < 258U; ++index) {
        fixture->bytes[index] = (uint8_t)(index ^ 0x5aU);
    }
    store_integer(fixture->bytes + 258U, 4U, 0U, big_endian);
    store_integer(fixture->bytes + 262U, 4U, 2U, big_endian);
    fixture->bytes[266U] = 0xaaU;
    fixture->bytes[267U] = 0x55U;
    object_row(fixture, 268U, UINT64_C(0xabcdef0123456789), 16U, 8U, 1U);
    object_row(fixture, 292U, UINT64_C(0xabcdef0123456789), 20U, 8U, 0U);
}

static bool span_is(
    SerializedFilePrefixSpan span, const uint8_t* bytes, size_t offset, size_t size) {
    CHECK(span.data == bytes + offset && span.offset == offset && span.size == size);
    return true;
}

static bool absent(SerializedFilePrefixSpan span) {
    CHECK(span.data == NULL && span.offset == UINT64_MAX && span.size == 0U);
    return true;
}

static bool canaries_intact(const GuardedDirectory* guarded) {
    for (size_t index = 0U; index < DIRECTORY_CANARY_SIZE; ++index) {
        CHECK(guarded->before[index] == 0xa5U && guarded->after[index] == 0xa5U);
    }
    return true;
}

static void init_guard(GuardedDirectory* guarded) {
    memset(guarded, 0xa5, sizeof(*guarded));
    serialized_file_directory_init(&guarded->value);
}

static bool created(const DirectoryFixture* fixture,
    const SerializedFileDirectoryLimits* limits,
    GuardedDirectory* guarded,
    SerializedFileDirectoryResult* out_result) {
    init_guard(guarded);
    uint8_t original[DIRECTORY_FIXTURE_SIZE];
    memcpy(original, fixture->bytes, sizeof(original));
    const size_t allocations = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    const SerializedFileDirectoryResult result = serialized_file_directory_create(fixture->bytes,
        fixture->directory_end,
        fixture->logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        limits,
        &guarded->value);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_OK &&
        result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_NONE &&
        result.field == SERIALIZED_FILE_DIRECTORY_FIELD_NONE && result.row_ordinal == SIZE_MAX &&
        result.error_offset == UINT64_MAX);
    CHECK(canaries_intact(guarded) && !memcmp(original, fixture->bytes, sizeof(original)));
    CHECK(result.prefix_attempted && result.prefix_result.status == SERIALIZED_FILE_PREFIX_OK &&
        result.prefix_result.work_used == 66U && result.prefix_result.error_offset == UINT64_MAX);
    const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded->value);
    CHECK(view != NULL && view->retained_bytes == result.required_retained_bytes &&
        result.peak_retained_bytes == view->retained_bytes && result.peak_scratch_bytes == 0U);
    CHECK(g_allocations_count == allocations + 1U &&
        g_allocated_bytes == allocated_bytes + view->retained_bytes);
    const uint64_t directory_bytes = fixture->directory_end - DIRECTORY_PREFIX_END;
    const uint64_t expected_work = 66U + 11U +
        2U * (directory_bytes + view->type_count + view->object_count) + view->retained_bytes + 2U;
    CHECK(result.work_used == expected_work && result.work_used <= limits->max_work);
    if (out_result) {
        *out_result = result;
    }
    return true;
}

static bool failed(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    SerializedFileDirectoryEngineVersion version,
    const SerializedFileDirectoryLimits* limits,
    SerializedFileDirectoryStatus status,
    SerializedFileDirectoryField field,
    size_t row,
    uint64_t offset,
    SerializedFileDirectoryResult* out_result) {
    GuardedDirectory guarded;
    init_guard(&guarded);
    uint8_t previous[sizeof(guarded)];
    memcpy(previous, &guarded, sizeof(previous));
    const size_t allocations = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    const SerializedFileDirectoryResult result = serialized_file_directory_create(
        bytes, mapped_size, logical_size, version, limits, &guarded.value);
    if (result.status != status || result.field != field || result.row_ordinal != row ||
        result.error_offset != offset) {
        fprintf(stderr,
            "directory failure status=%u field=%u row=%zu offset=%llu; "
            "expected %u/%u/%zu/%llu\n",
            (unsigned int)result.status,
            (unsigned int)result.field,
            result.row_ordinal,
            (unsigned long long)result.error_offset,
            (unsigned int)status,
            (unsigned int)field,
            row,
            (unsigned long long)offset);
    }
    CHECK(result.status == status && result.field == field && result.row_ordinal == row &&
        result.error_offset == offset);
    CHECK(!memcmp(previous, &guarded, sizeof(previous)) && g_allocations_count == allocations &&
        g_allocated_bytes == allocated_bytes);
    CHECK(!serialized_file_directory_view(&guarded.value) &&
        !serialized_file_directory_type(&guarded.value, 0U) &&
        !serialized_file_directory_object(&guarded.value, 0U));
    CHECK(!limits || result.work_used <= limits->max_work);
    CHECK(result.peak_scratch_bytes == 0U);
    if (out_result) {
        *out_result = result;
    }
    serialized_file_directory_dispose(&guarded.value);
    return true;
}

static bool check_type_prefix(const SerializedFileDirectoryTypeRow* row,
    const uint8_t* bytes,
    size_t ordinal,
    size_t start,
    size_t size,
    uint32_t class_id,
    uint8_t stripped,
    uint16_t script_index,
    bool has_hash) {
    CHECK(row && row->ordinal == ordinal && row->class_id_bits == class_id &&
        row->stripped_raw == stripped && row->script_index_bits == script_index &&
        row->has_script_hash == has_hash);
    CHECK(span_is(row->source, bytes, start, size));
    CHECK(span_is(row->class_id_source, bytes, start, 4U));
    CHECK(span_is(row->stripped_source, bytes, start + 4U, 1U));
    CHECK(span_is(row->script_index_source, bytes, start + 5U, 2U));
    if (has_hash) {
        CHECK(span_is(row->script_hash_source, bytes, start + 7U, 16U));
    } else {
        CHECK(absent(row->script_hash_source));
    }
    CHECK(span_is(row->type_hash_source, bytes, start + (has_hash ? 23U : 7U), 16U));
    return true;
}

static bool no_tree_sources_are_absent(const SerializedFileDirectoryTypeRow* row) {
    CHECK(!row->has_tree && !row->tree.node_count && !row->tree.string_byte_count &&
        !row->dependency_count);
    CHECK(absent(row->tree.node_count_source) && absent(row->tree.string_count_source) &&
        absent(row->tree.nodes_source) && absent(row->tree.strings_source) &&
        absent(row->dependency_count_source) && absent(row->dependency_words_source));
    return true;
}

static bool original_rows_and_optional_hashes(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        DirectoryFixture fixture;
        no_tree_fixture(&fixture, order != 0U);
        GuardedDirectory guarded;
        CHECK(created(&fixture, &generous_limits, &guarded, NULL));
        const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
        CHECK(view->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 &&
            view->type_count == 2U && view->object_count == 3U && !view->node_record_count &&
            !view->string_byte_count && !view->dependency_word_count);
        CHECK(view->prefix.header.endian_selector == order &&
            view->prefix.target_platform == UINT32_C(0x89abcdef));
        CHECK(span_is(view->parsed_metadata_source, fixture.bytes, 48U, 160U));
        CHECK(span_is(view->type_count_source, fixture.bytes, 65U, 4U));
        CHECK(span_is(view->type_rows_source, fixture.bytes, 69U, 62U));
        CHECK(span_is(view->object_count_source, fixture.bytes, 131U, 4U));
        CHECK(span_is(view->object_padding_source, fixture.bytes, 135U, 1U) &&
            view->object_padding_source.data[0] == 0xd3U);
        CHECK(span_is(view->object_rows_source, fixture.bytes, 136U, 72U));
        CHECK(view->remaining_metadata.offset == 208U && view->remaining_metadata.size == 24U);
        const SerializedFileDirectoryTypeRow* first =
            serialized_file_directory_type(&guarded.value, 0U);
        const SerializedFileDirectoryTypeRow* second =
            serialized_file_directory_type(&guarded.value, 1U);
        CHECK(check_type_prefix(
            first, fixture.bytes, 0U, 69U, 23U, UINT32_C(0x87654321), 0xa6U, UINT16_MAX, false));
        CHECK(check_type_prefix(
            second, fixture.bytes, 1U, 92U, 39U, 114U, 0xffU, UINT16_C(0x8000), true));
        CHECK(no_tree_sources_are_absent(first) && no_tree_sources_are_absent(second));
        const uint64_t paths[] = {UINT64_MAX, UINT64_MAX, UINT64_C(0x0102030405060708)};
        const uint64_t relative[] = {8U, 12U, 64U};
        const uint32_t sizes[] = {12U, 8U, 0U};
        const uint32_t types[] = {1U, 0U, 1U};
        for (size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
            const SerializedFileDirectoryObjectRow* object =
                serialized_file_directory_object(&guarded.value, ordinal);
            CHECK(object && object->ordinal == ordinal && object->path_id_bits == paths[ordinal] &&
                object->relative_data_offset == relative[ordinal] &&
                object->byte_size == sizes[ordinal] && object->type_ordinal == types[ordinal] &&
                object->payload.offset == 256U + relative[ordinal] &&
                object->payload.size == sizes[ordinal]);
            CHECK(span_is(object->source, fixture.bytes, 136U + ordinal * 24U, 24U));
        }
        CHECK(!serialized_file_directory_type(&guarded.value, 2U) &&
            !serialized_file_directory_type(&guarded.value, SIZE_MAX) &&
            !serialized_file_directory_object(&guarded.value, 3U) &&
            !serialized_file_directory_object(&guarded.value, SIZE_MAX));
        serialized_file_directory_dispose(&guarded.value);
        CHECK(canaries_intact(&guarded));
    }
    return true;
}

static bool opaque_trees_and_dependencies(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        DirectoryFixture fixture;
        tree_fixture(&fixture, order != 0U);
        GuardedDirectory guarded;
        CHECK(created(&fixture, &generous_limits, &guarded, NULL));
        const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
        CHECK(view->type_count == 2U && view->object_count == 2U && view->node_record_count == 3U &&
            view->string_byte_count == 3U && view->dependency_word_count == 2U);
        CHECK(span_is(view->parsed_metadata_source, fixture.bytes, 48U, 268U));
        CHECK(span_is(view->type_rows_source, fixture.bytes, 69U, 193U));
        CHECK(span_is(view->object_count_source, fixture.bytes, 262U, 4U));
        CHECK(span_is(view->object_padding_source, fixture.bytes, 266U, 2U));
        CHECK(span_is(view->object_rows_source, fixture.bytes, 268U, 48U));
        CHECK(view->remaining_metadata.offset == 316U && view->remaining_metadata.size == 24U);
        const SerializedFileDirectoryTypeRow* first =
            serialized_file_directory_type(&guarded.value, 0U);
        const SerializedFileDirectoryTypeRow* second =
            serialized_file_directory_type(&guarded.value, 1U);
        CHECK(check_type_prefix(first, fixture.bytes, 0U, 69U, 78U, 49U, 2U, UINT16_MAX, false));
        CHECK(check_type_prefix(second, fixture.bytes, 1U, 147U, 115U, 48U, 0xfeU, 0U, true));
        CHECK(first->has_tree && second->has_tree && first->tree.node_count == 1U &&
            second->tree.node_count == 2U && first->tree.string_byte_count == 3U &&
            second->tree.string_byte_count == 0U && first->dependency_count == 2U &&
            second->dependency_count == 0U);
        CHECK(span_is(first->tree.node_count_source, fixture.bytes, 92U, 4U));
        CHECK(span_is(first->tree.string_count_source, fixture.bytes, 96U, 4U));
        CHECK(span_is(first->tree.nodes_source, fixture.bytes, 100U, 32U));
        CHECK(span_is(first->tree.strings_source, fixture.bytes, 132U, 3U));
        CHECK(span_is(first->dependency_count_source, fixture.bytes, 135U, 4U));
        CHECK(span_is(first->dependency_words_source, fixture.bytes, 139U, 8U));
        CHECK(span_is(second->tree.node_count_source, fixture.bytes, 186U, 4U));
        CHECK(span_is(second->tree.string_count_source, fixture.bytes, 190U, 4U));
        CHECK(span_is(second->tree.nodes_source, fixture.bytes, 194U, 64U));
        CHECK(span_is(second->tree.strings_source, fixture.bytes, 258U, 0U));
        CHECK(span_is(second->dependency_count_source, fixture.bytes, 258U, 4U));
        CHECK(span_is(second->dependency_words_source, fixture.bytes, 262U, 0U));
        /* No byte swapping of opaque node/hash/dependency storage is permitted,
     * including the unresolved final eight bytes of each node. */
        CHECK(first->tree.nodes_source.data[24U] == (uint8_t)(124U ^ 0xa5U) &&
            second->tree.nodes_source.data[63U] == (uint8_t)(257U ^ 0x5aU));
        serialized_file_directory_dispose(&guarded.value);
    }
    return true;
}

static bool hash_predicate_and_unsupported_shapes(void) {
    static const struct HashCase {
        uint32_t class_id;
        uint16_t script_index;
        bool hash;
        bool supported;
    } cases[] = {{49U, UINT16_MAX, false, true},
        {49U, UINT16_C(0xfffe), false, true},
        {49U, UINT16_C(0x8000), false, true},
        {49U, UINT16_C(0x7fff), true, true},
        {49U, 0U, true, true},
        {48U, 1U, true, true},
        {114U, UINT16_MAX, true, true},
        {114U, UINT16_C(0x8000), true, true},
        {114U, 0U, true, true},
        {UINT32_MAX, 0U, true, true},
        {UINT32_MAX, UINT16_C(0x7fff), true, true},
        {UINT32_MAX, UINT16_MAX, false, false},
        {UINT32_MAX, UINT16_C(0x8000), false, false}};

    for (unsigned int order = 0U; order < 2U; ++order) {
        for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            DirectoryFixture fixture;
            const size_t type_size = cases[index].hash ? 39U : 23U;
            const size_t object_count_offset = 69U + type_size;
            header_fixture(&fixture, order != 0U, 0U, object_count_offset + 4U, 160U, 192U, 192U);
            store_integer(fixture.bytes + 65U, 4U, 1U, fixture.big_endian);
            type_prefix(&fixture,
                69U,
                cases[index].class_id,
                0xffU,
                cases[index].script_index,
                cases[index].hash);
            store_integer(fixture.bytes + object_count_offset, 4U, 0U, fixture.big_endian);
            if (!cases[index].supported) {
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &generous_limits,
                    SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TYPE_SHAPE,
                    SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY,
                    0U,
                    69U,
                    NULL));
                continue;
            }
            GuardedDirectory guarded;
            CHECK(created(&fixture, &generous_limits, &guarded, NULL));
            const SerializedFileDirectoryTypeRow* row =
                serialized_file_directory_type(&guarded.value, 0U);
            CHECK(check_type_prefix(row,
                fixture.bytes,
                0U,
                69U,
                type_size,
                cases[index].class_id,
                0xffU,
                cases[index].script_index,
                cases[index].hash));
            const SerializedFileDirectoryView* view =
                serialized_file_directory_view(&guarded.value);
            CHECK(span_is(view->object_count_source, fixture.bytes, object_count_offset, 4U));
            CHECK(
                span_is(view->object_padding_source, fixture.bytes, object_count_offset + 4U, 0U));
            CHECK(span_is(view->object_rows_source, fixture.bytes, object_count_offset + 4U, 0U));
            serialized_file_directory_dispose(&guarded.value);
        }
        DirectoryFixture fixture;
        tree_fixture(&fixture, order != 0U);
        store_integer(fixture.bytes + 92U, 4U, 0U, fixture.big_endian);
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &generous_limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_SHAPE,
            SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
            0U,
            92U,
            NULL));
        for (unsigned int flag = 2U; flag <= 255U; flag += 253U) {
            fixture.bytes[64U] = (uint8_t)flag;
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &generous_limits,
                SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_FLAG,
                SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
                SIZE_MAX,
                64U,
                NULL));
        }
    }
    return true;
}

typedef struct DirectoryWireField {
    size_t offset;
    size_t size;
    SerializedFileDirectoryField field;
    size_t row;
} DirectoryWireField;

/* The actual field widths are explicit fixture facts. In particular, objects
 * are atomic24-byte reads, while each type prefix keeps its separate fields. */
static const DirectoryWireField no_tree_fields[] = {
    {65U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT, SIZE_MAX},
    {69U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {73U, 1U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {74U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {76U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {92U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {96U, 1U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {97U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {99U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {115U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {131U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT, SIZE_MAX},
    {135U, 1U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_PADDING, SIZE_MAX},
    {136U, 24U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY, 0U},
    {160U, 24U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY, 1U},
    {184U, 24U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY, 2U}};

static const DirectoryWireField tree_fields[] = {
    {65U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT, SIZE_MAX},
    {69U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {73U, 1U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {74U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {76U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 0U},
    {92U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 0U},
    {96U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 0U},
    {100U, 32U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 0U},
    {132U, 3U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 0U},
    {135U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES, 0U},
    {139U, 8U, SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES, 0U},
    {147U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {151U, 1U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {152U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {154U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {170U, 16U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY, 1U},
    {186U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 1U},
    {190U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 1U},
    {194U, 64U, SERIALIZED_FILE_DIRECTORY_FIELD_TREE, 1U},
    {258U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES, 1U},
    {262U, 4U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT, SIZE_MAX},
    {266U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_PADDING, SIZE_MAX},
    {268U, 24U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY, 0U},
    {292U, 24U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY, 1U}};

static const DirectoryWireField* first_unavailable_field(
    const DirectoryWireField* fields, size_t field_count, size_t available_end) {
    for (size_t index = 0U; index < field_count; ++index) {
        if (fields[index].offset + fields[index].size > available_end) {
            return &fields[index];
        }
    }
    return NULL;
}

static bool mapping_metadata_and_byte_caps(void) {
    for (unsigned int trees = 0U; trees < 2U; ++trees) {
        const DirectoryWireField* fields = trees ? tree_fields : no_tree_fields;
        const size_t field_count = trees ? sizeof(tree_fields) / sizeof(tree_fields[0])
                                         : sizeof(no_tree_fields) / sizeof(no_tree_fields[0]);
        for (unsigned int order = 0U; order < 2U; ++order) {
            DirectoryFixture fixture;
            if (trees) {
                tree_fixture(&fixture, order != 0U);
            } else {
                no_tree_fixture(&fixture, order != 0U);
            }
            for (size_t mapped = 0U; mapped < fixture.directory_end; ++mapped) {
                const DirectoryWireField* next =
                    first_unavailable_field(fields, field_count, mapped);
                CHECK(next != NULL);
                const bool prefix = mapped < DIRECTORY_PREFIX_END;
                SerializedFileDirectoryResult result;
                CHECK(failed(fixture.bytes,
                    mapped,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &generous_limits,
                    prefix ? SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED
                           : SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
                    prefix ? SERIALIZED_FILE_DIRECTORY_FIELD_NONE : next->field,
                    prefix ? SIZE_MAX : next->row,
                    mapped,
                    &result));
                CHECK(!result.required_retained_bytes && !result.peak_retained_bytes);
                if (prefix) {
                    CHECK(result.prefix_attempted &&
                        result.prefix_result.status == SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING &&
                        result.prefix_result.error_offset == mapped);
                }
            }
            for (size_t metadata_end = 48U; metadata_end < fixture.directory_end; ++metadata_end) {
                store_integer(fixture.bytes + 16U, 8U, metadata_end - 48U, true);
                const DirectoryWireField* next =
                    first_unavailable_field(fields, field_count, metadata_end);
                CHECK(next != NULL);
                const bool prefix = metadata_end < DIRECTORY_PREFIX_END;
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &generous_limits,
                    prefix ? SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED
                           : SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
                    prefix ? SERIALIZED_FILE_DIRECTORY_FIELD_NONE : next->field,
                    prefix ? SIZE_MAX : next->row,
                    metadata_end,
                    NULL));
            }
            if (trees) {
                tree_fixture(&fixture, order != 0U);
            } else {
                no_tree_fixture(&fixture, order != 0U);
            }
            SerializedFileDirectoryLimits limits = generous_limits;
            for (size_t cap = 0U; cap < fixture.directory_end - 48U; ++cap) {
                limits.max_metadata_bytes = cap;
                const DirectoryWireField* next =
                    first_unavailable_field(fields, field_count, 48U + cap);
                CHECK(next != NULL);
                const bool prefix = cap < 17U;
                SerializedFileDirectoryResult result;
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
                    prefix ? SERIALIZED_FILE_DIRECTORY_FIELD_NONE : next->field,
                    prefix ? SIZE_MAX : next->row,
                    prefix ? DIRECTORY_PREFIX_END : next->offset,
                    &result));
                CHECK(result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_METADATA_BYTES &&
                    !result.peak_retained_bytes);
            }
            limits.max_metadata_bytes = fixture.directory_end - 48U;
            GuardedDirectory guarded;
            CHECK(created(&fixture, &limits, &guarded, NULL));
            serialized_file_directory_dispose(&guarded.value);
        }
    }
    return true;
}

static bool counts_ordinals_and_payload_ranges(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        static const struct NegativeCount {
            size_t offset;
            SerializedFileDirectoryField field;
            size_t row;
            bool trees;
        } negative[] = {{65U, SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT, SIZE_MAX, false},
            {131U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT, SIZE_MAX, false},
            {135U, SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES, 0U, true}};

        for (size_t index = 0U; index < sizeof(negative) / sizeof(negative[0]); ++index) {
            DirectoryFixture fixture;
            if (negative[index].trees) {
                tree_fixture(&fixture, order != 0U);
            } else {
                no_tree_fixture(&fixture, order != 0U);
            }
            store_integer(
                fixture.bytes + negative[index].offset, 4U, UINT32_MAX, fixture.big_endian);
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &generous_limits,
                SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
                negative[index].field,
                negative[index].row,
                negative[index].offset,
                NULL));
        }

        static const struct InvalidObject {
            size_t field_offset;
            size_t width;
            uint64_t value;
            SerializedFileDirectoryField field;
        } invalid[] = {{8U, 8U, UINT64_MAX, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE},
            {8U, 8U, 65U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE},
            {16U, 4U, 57U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE},
            {16U, 4U, UINT32_MAX, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE},
            {20U, 4U, 2U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_TYPE},
            {20U, 4U, 114U, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_TYPE},
            {20U, 4U, UINT32_MAX, SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_TYPE}};

        for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
            DirectoryFixture fixture;
            no_tree_fixture(&fixture, order != 0U);
            store_integer(fixture.bytes + 136U + invalid[index].field_offset,
                invalid[index].width,
                invalid[index].value,
                fixture.big_endian);
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &generous_limits,
                SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
                invalid[index].field,
                0U,
                136U + invalid[index].field_offset,
                NULL));
        }
    }
    return true;
}

static bool engine_selection_and_prefix_limits(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        DirectoryFixture fixture;
        no_tree_fixture(&fixture, order != 0U);
        for (size_t index = 0U; index < 11U; ++index) {
            const uint8_t original = fixture.bytes[48U + index];
            fixture.bytes[48U + index] ^= 1U;
            SerializedFileDirectoryResult result;
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &generous_limits,
                SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
                SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
                SIZE_MAX,
                48U + index,
                &result));
            CHECK(result.work_used == 66U + index + 1U && result.prefix_result.work_used == 66U);
            fixture.bytes[48U + index] = original;
        }
        SerializedFileDirectoryResult result;
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            (SerializedFileDirectoryEngineVersion)0,
            &generous_limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            UINT64_MAX,
            &result));
        CHECK(result.work_used == 66U && result.prefix_attempted);
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1,
            &generous_limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            55U,
            NULL));
        memcpy(fixture.bytes + 48U, "2021.3.29f1", 12U);
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &generous_limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            55U,
            NULL));
        GuardedDirectory guarded;
        init_guard(&guarded);
        result = serialized_file_directory_create(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1,
            &generous_limits,
            &guarded.value);
        CHECK(result.status == SERIALIZED_FILE_DIRECTORY_OK && canaries_intact(&guarded));
        const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
        CHECK(view && view->engine_version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1 &&
            !memcmp(view->prefix.version_source.data, "2021.3.29f1", 11U) &&
            view->type_count == 2U && view->object_count == 3U);
        CHECK(result.work_used == 66U + 11U + 2U * (143U + 2U + 3U) + view->retained_bytes + 2U);
        CHECK(check_type_prefix(serialized_file_directory_type(&guarded.value, 1U),
            fixture.bytes,
            1U,
            92U,
            39U,
            114U,
            0xffU,
            UINT16_C(0x8000),
            true));
        serialized_file_directory_dispose(&guarded.value);

        no_tree_fixture(&fixture, order != 0U);
        for (size_t cap = 0U; cap < 12U; ++cap) {
            SerializedFileDirectoryLimits limits = generous_limits;
            limits.max_version_bytes = cap;
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &limits,
                SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED,
                SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
                SIZE_MAX,
                48U + cap,
                &result));
            CHECK(result.prefix_result.status == SERIALIZED_FILE_PREFIX_VERSION_LIMIT &&
                result.work_used == 48U + cap);
        }
        fixture.bytes[54U] = 0U;
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &generous_limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            54U,
            &result));
        CHECK(result.prefix_result.work_used == 61U && result.work_used == 67U);
        no_tree_fixture(&fixture, order != 0U);
        fixture.bytes[59U] = 'x';
        fixture.bytes[60U] = 0U;
        SerializedFileDirectoryLimits limits = generous_limits;
        limits.max_version_bytes = 13U;
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &limits,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            59U,
            &result));
        CHECK(result.prefix_result.work_used == 67U && result.work_used == 79U);
    }
    return true;
}

static bool aggregate_cardinality_caps(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        DirectoryFixture fixture;
        tree_fixture(&fixture, order != 0U);
        for (unsigned int which = 0U; which < 5U; ++which) {
            const size_t required[] = {2U, 2U, 3U, 3U, 2U};
            const SerializedFileDirectoryLimit kinds[] = {SERIALIZED_FILE_DIRECTORY_LIMIT_TYPES,
                SERIALIZED_FILE_DIRECTORY_LIMIT_OBJECTS,
                SERIALIZED_FILE_DIRECTORY_LIMIT_NODES,
                SERIALIZED_FILE_DIRECTORY_LIMIT_STRING_BYTES,
                SERIALIZED_FILE_DIRECTORY_LIMIT_DEPENDENCIES};
            for (size_t cap = 0U; cap < required[which]; ++cap) {
                SerializedFileDirectoryLimits limits = generous_limits;
                SerializedFileDirectoryField field;
                size_t row = SIZE_MAX;
                size_t offset;
                switch (which) {
                case 0U:
                    limits.max_types = cap;
                    field = SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT;
                    offset = 65U;
                    break;
                case 1U:
                    limits.max_objects = cap;
                    field = SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT;
                    offset = 262U;
                    break;
                case 2U:
                    limits.max_node_records = cap;
                    field = SERIALIZED_FILE_DIRECTORY_FIELD_TREE;
                    row = cap ? 1U : 0U;
                    offset = cap ? 186U : 92U;
                    break;
                case 3U:
                    limits.max_string_bytes = cap;
                    field = SERIALIZED_FILE_DIRECTORY_FIELD_TREE;
                    row = 0U;
                    offset = 96U;
                    break;
                default:
                    limits.max_dependency_words = cap;
                    field = SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES;
                    row = 0U;
                    offset = 135U;
                    break;
                }
                SerializedFileDirectoryResult result;
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
                    field,
                    row,
                    offset,
                    &result));
                CHECK(result.limit == kinds[which] && !result.required_retained_bytes &&
                    !result.peak_retained_bytes);
            }
        }
        SerializedFileDirectoryLimits limits = generous_limits;
        limits.max_types = 2U;
        limits.max_objects = 2U;
        limits.max_node_records = 3U;
        limits.max_string_bytes = 3U;
        limits.max_dependency_words = 2U;
        limits.max_metadata_bytes = 268U;
        GuardedDirectory guarded;
        CHECK(created(&fixture, &limits, &guarded, NULL));
        serialized_file_directory_dispose(&guarded.value);
    }
    return true;
}

static bool unsigned_tree_counts_and_logical_payload(void) {
    for (unsigned int order = 0U; order < 2U; ++order) {
        const uint32_t counts[] = {UINT32_C(0x80000000), UINT32_MAX};
        for (size_t index = 0U; index < sizeof(counts) / sizeof(counts[0]); ++index) {
            for (unsigned int strings = 0U; strings < 2U; ++strings) {
                DirectoryFixture fixture;
                tree_fixture(&fixture, order != 0U);
                const size_t offset = strings ? 96U : 92U;
                store_integer(fixture.bytes + offset, 4U, counts[index], fixture.big_endian);
                SerializedFileDirectoryLimits limits = generous_limits;
                SerializedFileDirectoryResult result;
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
                    SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
                    0U,
                    offset,
                    &result));
                CHECK(result.limit ==
                    (strings ? SERIALIZED_FILE_DIRECTORY_LIMIT_STRING_BYTES
                             : SERIALIZED_FILE_DIRECTORY_LIMIT_NODES));
                limits.max_node_records = UINT32_MAX;
                limits.max_string_bytes = UINT32_MAX;
                limits.max_metadata_bytes = UINT64_MAX;
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
                    SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
                    0U,
                    340U,
                    NULL));
                /* Counts retain their high bits without allocating their spans.
                 * A large logical extent changes exhaustion into missing mapping. */
                const uint64_t metadata_end = UINT64_C(0x2000000100);
                fixture.logical_size = metadata_end + 128U;
                store_integer(fixture.bytes + 16U, 8U, metadata_end - 48U, true);
                store_integer(fixture.bytes + 24U, 8U, fixture.logical_size, true);
                store_integer(fixture.bytes + 32U, 8U, metadata_end, true);
                CHECK(failed(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
                    SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
                    0U,
                    fixture.directory_end,
                    NULL));
            }
        }
        DirectoryFixture fixture;
        no_tree_fixture(&fixture, order != 0U);
        const uint64_t data_origin = UINT64_C(0x8000000000000000);
        fixture.logical_size = UINT64_MAX;
        store_integer(fixture.bytes + 16U, 8U, data_origin - 48U, true);
        store_integer(fixture.bytes + 24U, 8U, UINT64_MAX, true);
        store_integer(fixture.bytes + 32U, 8U, data_origin, true);
        object_row(&fixture, 136U, UINT64_MAX, UINT64_C(0x7ffffffffffffffe), 1U, 1U);
        object_row(&fixture, 160U, UINT64_MAX, UINT64_C(0x7ffffffffffffffe), 1U, 0U);
        object_row(
            &fixture, 184U, UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff), 0U, 1U);
        GuardedDirectory guarded;
        SerializedFileDirectoryLimits limits = generous_limits;
        limits.max_metadata_bytes = 160U;
        CHECK(created(&fixture, &limits, &guarded, NULL));
        const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
        CHECK(view->remaining_metadata.offset == 208U &&
            view->remaining_metadata.size == data_origin - 208U);
        const SerializedFileDirectoryObjectRow* first =
            serialized_file_directory_object(&guarded.value, 0U);
        const SerializedFileDirectoryObjectRow* last =
            serialized_file_directory_object(&guarded.value, 2U);
        CHECK(first->payload.offset == UINT64_MAX - 1U && first->payload.size == 1U &&
            last->payload.offset == UINT64_MAX && last->payload.size == 0U &&
            last->path_id_bits == UINT64_C(0x8000000000000000));
        serialized_file_directory_dispose(&guarded.value);
        store_integer(fixture.bytes + 200U, 4U, 1U, fixture.big_endian);
        CHECK(failed(fixture.bytes,
            fixture.directory_end,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &limits,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE,
            2U,
            200U,
            NULL));
    }
    return true;
}

static bool work_and_retained_caps(void) {
    for (unsigned int trees = 0U; trees < 2U; ++trees) {
        DirectoryFixture fixture;
        if (trees) {
            tree_fixture(&fixture, true);
        } else {
            no_tree_fixture(&fixture, false);
        }
        GuardedDirectory guarded;
        SerializedFileDirectoryResult complete;
        CHECK(created(&fixture, &generous_limits, &guarded, &complete));
        serialized_file_directory_dispose(&guarded.value);
        const size_t retained = complete.required_retained_bytes;
        CHECK(retained > 0U && retained <= generous_limits.max_retained_bytes);
        SerializedFileDirectoryLimits limits = generous_limits;
        for (size_t cap = 0U; cap < retained; ++cap) {
            limits.max_retained_bytes = cap;
            SerializedFileDirectoryResult result;
            CHECK(failed(fixture.bytes,
                fixture.directory_end,
                fixture.logical_size,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                &limits,
                SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
                SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
                SIZE_MAX,
                UINT64_MAX,
                &result));
            CHECK(result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_RETAINED_BYTES &&
                result.required_retained_bytes == retained && result.peak_retained_bytes == 0U);
        }
        limits.max_retained_bytes = retained;
        CHECK(created(&fixture, &limits, &guarded, NULL));
        serialized_file_directory_dispose(&guarded.value);
        for (uint64_t budget = 0U; budget < complete.work_used; ++budget) {
            init_guard(&guarded);
            uint8_t original[sizeof(guarded)];
            memcpy(original, &guarded, sizeof(original));
            limits.max_work = budget;
            const size_t allocations = g_allocations_count;
            const size_t allocated_bytes = g_allocated_bytes;
            const SerializedFileDirectoryResult result =
                serialized_file_directory_create(fixture.bytes,
                    fixture.directory_end,
                    fixture.logical_size,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    &limits,
                    &guarded.value);
            CHECK(result.status ==
                (budget < 66U ? SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED
                              : SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED));
            CHECK(result.limit ==
                (budget < 66U ? SERIALIZED_FILE_DIRECTORY_LIMIT_NONE
                              : SERIALIZED_FILE_DIRECTORY_LIMIT_WORK));
            CHECK(!memcmp(original, &guarded, sizeof(original)) && result.work_used <= budget &&
                result.peak_scratch_bytes == 0U && g_allocations_count == allocations &&
                g_allocated_bytes == allocated_bytes);
            CHECK((result.required_retained_bytes == 0U ||
                      result.required_retained_bytes == retained) &&
                (result.peak_retained_bytes == 0U || result.peak_retained_bytes == retained));
            if (budget == complete.work_used - 1U) {
                CHECK(result.field == SERIALIZED_FILE_DIRECTORY_FIELD_NONE &&
                    result.row_ordinal == SIZE_MAX && result.error_offset == UINT64_MAX &&
                    result.required_retained_bytes == retained &&
                    result.peak_retained_bytes == retained && result.work_used == budget);
            }
            if (budget >= 66U && budget < 77U) {
                CHECK(result.field == SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION &&
                    result.error_offset == 48U + budget - 66U && result.work_used == budget);
            }
        }
        limits.max_work = complete.work_used;
        CHECK(created(&fixture, &limits, &guarded, NULL));
        serialized_file_directory_dispose(&guarded.value);
    }
    return true;
}

static bool empty_tables_and_coordinate_alignment(void) {
    DirectoryFixture fixture;
    header_fixture(&fixture, false, 0U, 73U, 97U, 128U, 128U);
    store_integer(fixture.bytes + 65U, 4U, 0U, false);
    store_integer(fixture.bytes + 69U, 4U, 0U, false);
    SerializedFileDirectoryLimits limits = generous_limits;
    limits.max_types = 0U;
    limits.max_objects = 0U;
    limits.max_node_records = 0U;
    limits.max_string_bytes = 0U;
    limits.max_dependency_words = 0U;
    limits.max_metadata_bytes = 25U;
    GuardedDirectory guarded;
    CHECK(created(&fixture, &limits, &guarded, NULL));
    const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
    CHECK(!view->type_count && !view->object_count && view->remaining_metadata.offset == 73U);
    CHECK(span_is(view->type_rows_source, fixture.bytes, 69U, 0U));
    CHECK(span_is(view->object_count_source, fixture.bytes, 69U, 4U));
    CHECK(span_is(view->object_padding_source, fixture.bytes, 73U, 0U));
    CHECK(span_is(view->object_rows_source, fixture.bytes, 73U, 0U));
    serialized_file_directory_dispose(&guarded.value);

    no_tree_fixture(&fixture, false);
    fixture.directory_end = 135U;
    store_integer(fixture.bytes + 131U, 4U, 0U, false);
    CHECK(created(&fixture, &generous_limits, &guarded, NULL));
    view = serialized_file_directory_view(&guarded.value);
    CHECK(view->type_count == 2U && view->object_count == 0U &&
        view->remaining_metadata.offset == 135U);
    CHECK(span_is(view->object_padding_source, fixture.bytes, 135U, 0U));
    CHECK(span_is(view->object_rows_source, fixture.bytes, 135U, 0U));
    serialized_file_directory_dispose(&guarded.value);

    /* Object alignment uses metadata coordinates even when the backing pointer
     * is deliberately unaligned. No native scalar load may assume alignment. */
    no_tree_fixture(&fixture, true);
    uint8_t shifted[DIRECTORY_FIXTURE_SIZE + 1U];
    memcpy(shifted + 1U, fixture.bytes, sizeof(fixture.bytes));
    init_guard(&guarded);
    CHECK(serialized_file_directory_create(shifted + 1U,
              fixture.directory_end,
              fixture.logical_size,
              SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
              &generous_limits,
              &guarded.value)
              .status == SERIALIZED_FILE_DIRECTORY_OK);
    view = serialized_file_directory_view(&guarded.value);
    CHECK(span_is(view->object_padding_source, shifted + 1U, 135U, 1U));
    CHECK(span_is(view->object_rows_source, shifted + 1U, 136U, 72U));
    CHECK(serialized_file_directory_object(&guarded.value, 2U)->path_id_bits ==
        UINT64_C(0x0102030405060708));
    serialized_file_directory_dispose(&guarded.value);
    return true;
}

static bool first_failure_order(void) {
    DirectoryFixture fixture;
    no_tree_fixture(&fixture, false);
    store_integer(fixture.bytes + 16U, 8U, 19U, true);
    CHECK(failed(fixture.bytes,
        65U,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
        SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT,
        SIZE_MAX,
        67U,
        NULL));
    no_tree_fixture(&fixture, false);
    SerializedFileDirectoryLimits limits = generous_limits;
    limits.max_work = 77U;
    SerializedFileDirectoryResult result;
    CHECK(failed(fixture.bytes,
        65U,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
        SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT,
        SIZE_MAX,
        65U,
        &result));
    CHECK(result.work_used == 77U);
    limits.max_metadata_bytes = 17U;
    CHECK(failed(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
        SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT,
        SIZE_MAX,
        65U,
        &result));
    CHECK(
        result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_METADATA_BYTES && result.work_used == 77U);
    limits.max_metadata_bytes = generous_limits.max_metadata_bytes;
    CHECK(failed(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
        SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT,
        SIZE_MAX,
        65U,
        &result));
    CHECK(result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_WORK && result.work_used == 77U);

    tree_fixture(&fixture, false);
    limits = generous_limits;
    limits.max_node_records = 0U;
    CHECK(failed(fixture.bytes,
        96U,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
        SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
        0U,
        92U,
        &result));
    CHECK(result.limit == SERIALIZED_FILE_DIRECTORY_LIMIT_NODES);
    limits.max_node_records = 1U;
    limits.max_string_bytes = 0U;
    CHECK(failed(fixture.bytes,
        96U,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
        SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
        0U,
        96U,
        NULL));
    fixture.bytes[64U] = 255U;
    limits.max_metadata_bytes = 0U;
    CHECK(failed(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_FLAG,
        SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
        SIZE_MAX,
        64U,
        NULL));
    store_integer(fixture.bytes + 8U, 4U, 23U, true);
    CHECK(failed(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        (SerializedFileDirectoryEngineVersion)0,
        &limits,
        SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        8U,
        &result));
    CHECK(result.prefix_attempted &&
        result.prefix_result.status == SERIALIZED_FILE_PREFIX_UNSUPPORTED_VERSION);
    return true;
}

static bool arguments_aliases_and_lifetimes(void) {
    serialized_file_directory_init(NULL);
    serialized_file_directory_dispose(NULL);
    CHECK(!serialized_file_directory_view(NULL) && !serialized_file_directory_type(NULL, 0U) &&
        !serialized_file_directory_object(NULL, 0U));
    DirectoryFixture fixture;
    no_tree_fixture(&fixture, false);
    SerializedFileDirectoryResult result;
    CHECK(failed(NULL,
        1U,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        &result));
    CHECK(!result.prefix_attempted && result.work_used == 0U &&
        result.prefix_result.status == SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT);
    CHECK(failed(fixture.bytes,
        321U,
        320U,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        NULL));
    CHECK(failed(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        NULL,
        SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        NULL));
    CHECK(failed(NULL,
        0U,
        0U,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        0U,
        &result));
    CHECK(result.prefix_result.status == SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT);
    CHECK(serialized_file_directory_create(fixture.bytes,
              fixture.directory_end,
              fixture.logical_size,
              SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
              &generous_limits,
              NULL)
              .status == SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT);

    GuardedDirectory guarded;
    CHECK(created(&fixture, &generous_limits, &guarded, NULL));
    const void* implementation = guarded.value.implementation;
    const SerializedFileDirectoryView* view = serialized_file_directory_view(&guarded.value);
    const size_t allocation_count = g_allocations_count;
    result = serialized_file_directory_create(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        &guarded.value);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_INVALID_STATE && !result.prefix_attempted &&
        result.work_used == 0U && guarded.value.implementation == implementation &&
        serialized_file_directory_view(&guarded.value) == view &&
        g_allocations_count == allocation_count && canaries_intact(&guarded));
    serialized_file_directory_dispose(&guarded.value);
    serialized_file_directory_dispose(&guarded.value);
    CHECK(!guarded.value.implementation && !serialized_file_directory_view(&guarded.value));

    union AlignedInput {
        max_align_t alignment;
        uint8_t bytes[DIRECTORY_FIXTURE_SIZE];
    } input;

    memcpy(input.bytes, fixture.bytes, sizeof(input.bytes));
    SerializedFileDirectory* overlapping_output =
        (SerializedFileDirectory*)(void*)(input.bytes + 128U);
    serialized_file_directory_init(overlapping_output);
    uint8_t original[sizeof(input.bytes)];
    memcpy(original, input.bytes, sizeof(original));
    result = serialized_file_directory_create(input.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        overlapping_output);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT && !result.prefix_attempted &&
        !memcmp(original, input.bytes, sizeof(original)));

    SerializedFileDirectoryLimits* overlapping_limits =
        (SerializedFileDirectoryLimits*)(void*)(input.bytes + 128U);
    *overlapping_limits = generous_limits;
    memcpy(original, input.bytes, sizeof(original));
    CHECK(failed(input.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        overlapping_limits,
        SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT,
        SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        NULL));
    CHECK(!memcmp(original, input.bytes, sizeof(original)));

    union OverlappingArguments {
        SerializedFileDirectoryLimits limits;
        SerializedFileDirectory output;
    } shared;

    shared.limits = generous_limits;
    uint8_t previous[sizeof(shared)];
    memcpy(previous, &shared, sizeof(previous));
    result = serialized_file_directory_create(fixture.bytes,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &shared.limits,
        &shared.output);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT && !result.prefix_attempted &&
        !memcmp(previous, &shared, sizeof(previous)));

    uint8_t* borrowed = malloc(fixture.directory_end);
    CHECK(borrowed != NULL);
    memcpy(borrowed, fixture.bytes, fixture.directory_end);
    init_guard(&guarded);
    result = serialized_file_directory_create(borrowed,
        fixture.directory_end,
        fixture.logical_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &generous_limits,
        &guarded.value);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_OK);
    CHECK(serialized_file_directory_object(&guarded.value, 0U)->source.data == borrowed + 136U);
    free(borrowed);
    /* No row/span accessor is valid here; disposal expressly needs no backing. */
    serialized_file_directory_dispose(&guarded.value);
    CHECK(!guarded.value.implementation && canaries_intact(&guarded));
    return true;
}

static bool guarded_mapping_tails(void) {
#ifndef _WIN32
    const long page_query = sysconf(_SC_PAGESIZE);
    CHECK(page_query > 0 && (unsigned long)page_query >= DIRECTORY_FIXTURE_SIZE);
    const size_t page_size = (size_t)page_query;
    uint8_t* mapping =
        mmap(NULL, page_size * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(mprotect(mapping + page_size, page_size, PROT_NONE) == 0);
    DirectoryFixture fixture;
    tree_fixture(&fixture, true);
    for (size_t size = 0U; size < fixture.directory_end; ++size) {
        uint8_t* prefix = mapping + page_size - size;
        if (size) {
            memcpy(prefix, fixture.bytes, size);
        }
        const DirectoryWireField* next = first_unavailable_field(
            tree_fields, sizeof(tree_fields) / sizeof(tree_fields[0]), size);
        CHECK(next != NULL);
        const bool in_prefix = size < DIRECTORY_PREFIX_END;
        CHECK(failed(prefix,
            size,
            fixture.logical_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &generous_limits,
            in_prefix ? SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED
                      : SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
            in_prefix ? SERIALIZED_FILE_DIRECTORY_FIELD_NONE : next->field,
            in_prefix ? SIZE_MAX : next->row,
            size,
            NULL));
    }
    uint8_t* prefix = mapping + page_size - fixture.directory_end;
    memcpy(prefix, fixture.bytes, fixture.directory_end);
    SerializedFileDirectory directory;
    serialized_file_directory_init(&directory);
    CHECK(serialized_file_directory_create(prefix,
              fixture.directory_end,
              fixture.logical_size,
              SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
              &generous_limits,
              &directory)
              .status == SERIALIZED_FILE_DIRECTORY_OK);
    CHECK(serialized_file_directory_object(&directory, 1U)->source.data == prefix + 292U);
    CHECK(mprotect(mapping, page_size, PROT_NONE) == 0);
    serialized_file_directory_dispose(&directory);
    CHECK(munmap(mapping, page_size * 2U) == 0);
#endif
    return true;
}

int main(void) {
    if (!original_rows_and_optional_hashes() || !opaque_trees_and_dependencies() ||
        !hash_predicate_and_unsupported_shapes() || !mapping_metadata_and_byte_caps() ||
        !counts_ordinals_and_payload_ranges() || !engine_selection_and_prefix_limits() ||
        !aggregate_cardinality_caps() || !unsigned_tree_counts_and_logical_payload() ||
        !work_and_retained_caps() || !empty_tables_and_coordinate_alignment() ||
        !first_failure_order() || !arguments_aliases_and_lifetimes() || !guarded_mapping_tails()) {
        return 1;
    }
    if (g_allocations_count || g_allocated_bytes) {
        fputs("directory tests retained Common allocations\n", stderr);
        return 1;
    }
    puts("SerializedFile directory: raw rows, versions, limits, mapping and lifetime passed");
    return 0;
}
