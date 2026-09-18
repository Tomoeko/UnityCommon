#include "serialized_metadata_tail_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const SerializedFileDirectoryLimits directory_limits = {
    12U, 0U, 0U, 0U, 0U, 0U, 25U, 65536U, 0U, UINT64_C(1048576)};

void tail_fixture_store(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        bytes[big_endian ? width - index - 1U : index] = (uint8_t)value;
        value >>= 8U;
    }
}

void tail_fixture_header(TailFixture* fixture, uint64_t metadata_end, uint64_t logical_size) {
    assert(metadata_end >= 48U && logical_size >= metadata_end);
    fixture->logical_size = logical_size;
    tail_fixture_store(fixture->bytes + 16U, 8U, metadata_end - 48U, true);
    tail_fixture_store(fixture->bytes + 24U, 8U, logical_size, true);
    tail_fixture_store(fixture->bytes + 32U, 8U, metadata_end, true);
}

static SerializedFilePrefixSpan span(TailFixture* fixture, size_t start, size_t size) {
    const SerializedFilePrefixSpan result = {fixture->bytes + start, start, size};
    return result;
}

static SerializedFilePrefixSpan absent(void) {
    const SerializedFilePrefixSpan result = {NULL, UINT64_MAX, 0U};
    return result;
}

static void event(TailFixture* fixture,
    size_t offset,
    size_t size,
    uint64_t work,
    SerializedFileMetadataTailField field,
    size_t row,
    bool terminated_byte) {
    assert(fixture->event_count < TAIL_FIXTURE_EVENTS);
    fixture->events[fixture->event_count++] =
        (TailFixtureEvent){offset, size, work, field, row, terminated_byte};
}

static SerializedFilePrefixSpan take(
    TailFixture* fixture, size_t size, SerializedFileMetadataTailField field, size_t row) {
    assert(size <= TAIL_FIXTURE_CAPACITY - fixture->end);
    const size_t start = fixture->end;
    fixture->end += size;
    event(fixture, start, size, size, field, row, false);
    return span(fixture, start, size);
}

static SerializedFilePrefixSpan scalar(TailFixture* fixture,
    size_t width,
    uint64_t value,
    SerializedFileMetadataTailField field,
    size_t row) {
    const SerializedFilePrefixSpan source = take(fixture, width, field, row);
    tail_fixture_store(fixture->bytes + source.offset, width, value, fixture->big_endian);
    return source;
}

static SerializedFilePrefixSpan pattern(TailFixture* fixture,
    size_t width,
    uint8_t first,
    SerializedFileMetadataTailField field,
    size_t row) {
    const SerializedFilePrefixSpan source = take(fixture, width, field, row);
    for (size_t index = 0U; index < width; ++index) {
        fixture->bytes[source.offset + index] = (uint8_t)(first + index);
    }
    return source;
}

static SerializedFilePrefixSpan terminated(TailFixture* fixture,
    const uint8_t* bytes,
    size_t size,
    SerializedFileMetadataTailField field,
    size_t row) {
    assert(size && bytes[size - 1U] == 0U && size <= TAIL_FIXTURE_CAPACITY - fixture->end);
    const size_t start = fixture->end;
    for (size_t index = 0U; index < size; ++index) {
        assert(index + 1U == size || bytes[index] != 0U);
        fixture->bytes[fixture->end] = bytes[index];
        event(fixture, fixture->end, 1U, 1U, field, row, true);
        ++fixture->end;
    }
    fixture->terminated_strings += size;
    return span(fixture, start, size);
}

static void complete_row(
    TailFixture* fixture, SerializedFileMetadataTailField field, size_t ordinal, size_t start) {
    event(fixture, start, 0U, 1U, field, ordinal, false);
}

static void script(TailFixture* fixture, size_t ordinal) {
    SerializedFileMetadataTailScriptRow* row = &fixture->scripts[ordinal];
    const size_t start = fixture->end;
    row->ordinal = ordinal;
    row->file_index_bits = UINT32_C(0x87654321);
    row->local_identifier_bits = UINT64_C(0xfedcba9876543210);
    row->file_index_source = scalar(fixture,
        4U,
        row->file_index_bits,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY,
        ordinal);
    /* The first file word ends at 81, then padding reaches 84. An incorrect
     * align-before-row reader sees different file and identifier bytes. */
    const size_t padding = ordinal ? 0U : 3U;
    row->alignment_source = pattern(
        fixture, padding, 0xd1U, SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ALIGNMENT, ordinal);
    row->local_identifier_source = scalar(fixture,
        8U,
        row->local_identifier_bits,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY,
        ordinal);
    row->source = span(fixture, start, fixture->end - start);
    complete_row(fixture, SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY, ordinal, start);
}

static void external(TailFixture* fixture, size_t ordinal) {
    static const uint8_t alias[] = {0xff, 0x80, 0};
    static const uint8_t empty[] = {0};
    static const uint8_t first_path[] = {'/', 0xfe, '\\', 0};
    static const uint8_t second_path[] = {'x', '/', '.', '.', '/', 'y', 0};
    static const uint32_t words[] = {
        UINT32_C(0x01234567), UINT32_C(0x89abcdef), UINT32_C(0xfedcba98), UINT32_C(0x76543210)};
    SerializedFileMetadataTailExternalRow* row = &fixture->externals[ordinal];
    const size_t start = fixture->end;
    row->ordinal = ordinal;
    row->leading_string_source = terminated(fixture,
        ordinal ? empty : alias,
        ordinal ? sizeof(empty) : sizeof(alias),
        SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_LEADING_STRING,
        ordinal);
    row->guid_source =
        take(fixture, 16U, SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_GUID, ordinal);
    for (size_t index = 0U; index < 4U; ++index) {
        row->guid_words[index] = words[index];
        tail_fixture_store(fixture->bytes + row->guid_source.offset + index * 4U,
            4U,
            words[index],
            fixture->big_endian);
    }
    row->type_bits = ordinal ? 1U : UINT32_C(0xfffffffe);
    row->type_source = scalar(
        fixture, 4U, row->type_bits, SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_TYPE, ordinal);
    row->path_source = terminated(fixture,
        ordinal ? second_path : first_path,
        ordinal ? sizeof(second_path) : sizeof(first_path),
        SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_PATH,
        ordinal);
    row->source = span(fixture, start, fixture->end - start);
    complete_row(fixture, SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_ENTRY, ordinal, start);
}

static void reference(TailFixture* fixture, size_t ordinal) {
    static const uint8_t empty[] = {0};
    static const uint8_t name[] = {0xff, 0x80, 0};
    static const uint8_t assembly[] = {'A', 0};
    static const uint32_t classes[] = {UINT32_C(0x87654321), 114U, UINT32_MAX};
    static const uint16_t indexes[] = {UINT16_C(0x8000), UINT16_C(0x8001), UINT16_C(0x7fff)};
    SerializedFileMetadataTailReferenceTypeRow* row = &fixture->references[ordinal];
    const size_t start = fixture->end;
    row->ordinal = ordinal;
    row->class_id_bits = classes[ordinal];
    row->stripped_raw = (uint8_t)(0xa6U + ordinal);
    row->script_index_bits = indexes[ordinal];
    row->has_script_hash = ordinal != 0U;
    row->has_tree = fixture->tree;
    row->class_id_source = scalar(fixture,
        4U,
        row->class_id_bits,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY,
        ordinal);
    row->stripped_source = scalar(fixture,
        1U,
        row->stripped_raw,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY,
        ordinal);
    row->script_index_source = scalar(fixture,
        2U,
        row->script_index_bits,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY,
        ordinal);
    row->script_hash_source = absent();
    if (row->has_script_hash) {
        row->script_hash_source = pattern(
            fixture, 16U, 0x90U, SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY, ordinal);
    }
    row->type_hash_source = pattern(
        fixture, 16U, 0x70U, SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY, ordinal);
    row->tree = (SerializedFileDirectoryTree){absent(), absent(), absent(), absent(), 0U, 0U};
    row->class_name_source = absent();
    row->namespace_source = absent();
    row->assembly_name_source = absent();
    if (fixture->tree) {
        row->tree.node_count = ordinal == 1U ? 2U : 1U;
        row->tree.string_byte_count = ordinal == 0U ? 0U : 5U;
        row->tree.node_count_source = scalar(
            fixture, 4U, row->tree.node_count, SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE, ordinal);
        row->tree.string_count_source = scalar(fixture,
            4U,
            row->tree.string_byte_count,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
            ordinal);
        row->tree.nodes_source = pattern(fixture,
            32U * row->tree.node_count,
            0xd0U,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
            ordinal);
        row->tree.strings_source = pattern(fixture,
            row->tree.string_byte_count,
            0xfbU,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
            ordinal);
        row->class_name_source = terminated(fixture,
            ordinal == 1U ? name : empty,
            ordinal == 1U ? sizeof(name) : sizeof(empty),
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_CLASS_NAME,
            ordinal);
        row->namespace_source = terminated(fixture,
            ordinal ? empty : name,
            ordinal ? sizeof(empty) : sizeof(name),
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_NAMESPACE,
            ordinal);
        row->assembly_name_source = terminated(fixture,
            ordinal ? empty : assembly,
            ordinal ? sizeof(empty) : sizeof(assembly),
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_ASSEMBLY_NAME,
            ordinal);
        fixture->nodes += row->tree.node_count;
        fixture->tree_strings += row->tree.string_byte_count;
    }
    row->source = span(fixture, start, fixture->end - start);
    complete_row(fixture, SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY, ordinal, start);
}

void tail_fixture_build(TailFixture* fixture,
    bool big_endian,
    bool tree,
    SerializedFileDirectoryEngineVersion version,
    bool empty) {
    memset(fixture, 0, sizeof(*fixture));
    memset(fixture->bytes, 0xe7, sizeof(fixture->bytes));
    memset(fixture->bytes, 0, 48U);
    fixture->end = TAIL_FIXTURE_START;
    fixture->big_endian = big_endian;
    fixture->tree = tree;
    fixture->version = version;
    tail_fixture_store(fixture->bytes + 8U, 4U, 22U, true);
    fixture->bytes[40] = big_endian ? 1U : 0U;
    memcpy(fixture->bytes + 48U,
        version == SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 ? "2021.3.35f1" : "2021.3.29f1",
        12U);
    tail_fixture_store(fixture->bytes + 60U, 4U, UINT32_C(0xfedcba98), big_endian);
    fixture->bytes[64] = tree ? 1U : 0U;
    tail_fixture_store(fixture->bytes + 65U, 4U, 0U, big_endian);
    tail_fixture_store(fixture->bytes + 69U, 4U, 0U, big_endian);
    fixture->script_count = empty ? 0U : TAIL_FIXTURE_SCRIPTS;
    fixture->external_count = empty ? 0U : TAIL_FIXTURE_EXTERNALS;
    fixture->reference_count = empty ? 0U : TAIL_FIXTURE_REFERENCES;

    fixture->counts[0] = scalar(fixture,
        4U,
        fixture->script_count,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_COUNT,
        SIZE_MAX);
    size_t start = fixture->end;
    for (size_t ordinal = 0U; ordinal < fixture->script_count; ++ordinal) {
        script(fixture, ordinal);
    }
    fixture->tables[0] = span(fixture, start, fixture->end - start);
    fixture->counts[1] = scalar(fixture,
        4U,
        fixture->external_count,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_COUNT,
        SIZE_MAX);
    start = fixture->end;
    for (size_t ordinal = 0U; ordinal < fixture->external_count; ++ordinal) {
        external(fixture, ordinal);
    }
    fixture->tables[1] = span(fixture, start, fixture->end - start);
    fixture->counts[2] = scalar(fixture,
        4U,
        fixture->reference_count,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_COUNT,
        SIZE_MAX);
    start = fixture->end;
    for (size_t ordinal = 0U; ordinal < fixture->reference_count; ++ordinal) {
        reference(fixture, ordinal);
    }
    fixture->tables[2] = span(fixture, start, fixture->end - start);
    static const uint8_t information[] = {0x80, 0xff, 'u', 0};
    static const uint8_t empty_information[] = {0};
    fixture->information = terminated(fixture,
        empty ? empty_information : information,
        empty ? sizeof(empty_information) : sizeof(information),
        SERIALIZED_FILE_METADATA_TAIL_FIELD_USER_INFORMATION,
        SIZE_MAX);
    tail_fixture_header(fixture, fixture->end, 4096U);
}

bool tail_fixture_parent_at(
    const TailFixture* fixture, const uint8_t* bytes, SerializedFileDirectory* out_parent) {
    serialized_file_directory_init(out_parent);
    const SerializedFileDirectoryResult result = serialized_file_directory_create(bytes,
        TAIL_FIXTURE_START,
        fixture->logical_size,
        fixture->version,
        &directory_limits,
        out_parent);
    if (result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        fprintf(stderr,
            "Tail fixture parent rejected: %d at %llu\n",
            (int)result.status,
            (unsigned long long)result.error_offset);
        return false;
    }
    return true;
}

bool tail_fixture_parent(const TailFixture* fixture, SerializedFileDirectory* out_parent) {
    return tail_fixture_parent_at(fixture, fixture->bytes, out_parent);
}
