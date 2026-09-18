#include "io/serialized_file_directory.h"

#include "common/sha256.h"
#include "io/serialized_file.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum {
    WRITER_FILE_SIZE = 4456,
    WRITER_TYPE_COUNT = 4,
    WRITER_OBJECT_COUNT = 5
};

typedef struct WriterTypeObservation {
    size_t with_tree_offset;
    size_t with_tree_size;
    size_t without_tree_offset;
    size_t without_tree_size;
    uint32_t class_id;
    uint16_t script_index;
    uint32_t node_count;
    uint32_t string_bytes;
    const char* type_hash;
    const char* script_hash;
    const char* nodes_sha256;
    const char* strings_sha256;
} WriterTypeObservation;

/* These fixed observations come from the independent raw writer report.
 * The two script rows have the same class ID but distinct type ordinals. */
static const WriterTypeObservation writer_types[WRITER_TYPE_COUNT] = {
    {69,
        1378,
        69,
        23,
        115,
        UINT16_MAX,
        35,
        223,
        "f46e8e30ecc293493f18ab27134210ee",
        NULL,
        "63da44b13e047af040a938345fc88f0c57cb4a1cf01927d87bf23270017a2a54",
        "2c92708220ac593118ea449e8a43ae664b9b13fa0ef4828feb249c87377d32f6"},
    {1447,
        323,
        92,
        23,
        49,
        UINT16_MAX,
        9,
        0,
        "486ba4e15dbd6aea8ac1a064305889c8",
        NULL,
        "ebe2b2c98cc892b44d662b24f8fb1fb57e7fc92e7f0b66844ebcd63a7a5c550a",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1770,
        492,
        115,
        39,
        114,
        0,
        13,
        25,
        "be616fca57d26c1a431845a85e585b2a",
        "5aa0388c641cf2a404cfc5fbb11ad65d",
        "88e3d673664384e44023199af4a86ee4365b03ac819f0374cb983b90bd50a6c5",
        "6784aa868c8dd6fe73721bfde9795062812dcb4a2f133093a94d1dd20c332c89"},
    {2262,
        492,
        154,
        39,
        114,
        1,
        13,
        25,
        "141a3c207a0fe842e4eaf12701a39d32",
        "42a30af819ae0fef10aa08a58d882384",
        "88e3d673664384e44023199af4a86ee4365b03ac819f0374cb983b90bd50a6c5",
        "6784aa868c8dd6fe73721bfde9795062812dcb4a2f133093a94d1dd20c332c89"}};

typedef struct WriterObjectObservation {
    uint64_t path_id;
    uint64_t relative_offset;
    uint32_t byte_size;
    uint32_t type_ordinal;
} WriterObjectObservation;

static const WriterObjectObservation writer_objects[WRITER_OBJECT_COUNT] = {{1001, 192, 52, 1},
    {2003, 248, 56, 2},
    {3005, 304, 56, 3},
    {4007, 0, 96, 0},
    {5009, 96, 96, 0}};

static bool digest_matches(const uint8_t* bytes, size_t size, const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    CHECK(strcmp(hex, expected) == 0);
    return true;
}

static bool hash128_matches(SerializedFilePrefixSpan span, const char* expected) {
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    CHECK(span.size == 16U);
    for (size_t index = 0; index < span.size; ++index) {
        hex[2U * index] = digits[span.data[index] >> 4U];
        hex[2U * index + 1U] = digits[span.data[index] & 15U];
    }
    hex[32] = '\0';
    CHECK(strcmp(hex, expected) == 0);
    return true;
}

static bool source_matches(
    SerializedFilePrefixSpan span, const uint8_t* bytes, size_t offset, size_t size) {
    CHECK(span.data == bytes + offset);
    CHECK(span.offset == offset && span.size == size);
    return true;
}

static bool check_types(
    const SerializedFileDirectory* directory, const uint8_t* bytes, bool with_tree) {
    for (size_t ordinal = 0; ordinal < WRITER_TYPE_COUNT; ++ordinal) {
        const WriterTypeObservation* expected = &writer_types[ordinal];
        const SerializedFileDirectoryTypeRow* row =
            serialized_file_directory_type(directory, ordinal);
        const size_t offset =
            with_tree ? expected->with_tree_offset : expected->without_tree_offset;
        const size_t size = with_tree ? expected->with_tree_size : expected->without_tree_size;
        const size_t hash_offset = offset + (expected->script_hash ? 23U : 7U);
        CHECK(row && row->ordinal == ordinal);
        CHECK(source_matches(row->source, bytes, offset, size));
        CHECK(source_matches(row->class_id_source, bytes, offset, 4));
        CHECK(source_matches(row->stripped_source, bytes, offset + 4U, 1));
        CHECK(source_matches(row->script_index_source, bytes, offset + 5U, 2));
        CHECK(row->class_id_bits == expected->class_id && row->stripped_raw == 0);
        CHECK(row->script_index_bits == expected->script_index);
        CHECK(row->has_script_hash == (expected->script_hash != NULL));
        if (expected->script_hash) {
            CHECK(source_matches(row->script_hash_source, bytes, offset + 7U, 16));
            CHECK(hash128_matches(row->script_hash_source, expected->script_hash));
        } else {
            CHECK(row->script_hash_source.data == NULL);
            CHECK(
                row->script_hash_source.offset == UINT64_MAX && row->script_hash_source.size == 0);
        }
        CHECK(source_matches(row->type_hash_source, bytes, hash_offset, 16));
        CHECK(hash128_matches(row->type_hash_source, expected->type_hash));
        CHECK(row->has_tree == with_tree && row->dependency_count == 0);
        if (!with_tree) {
            CHECK(row->tree.nodes_source.data == NULL);
            CHECK(row->dependency_words_source.data == NULL);
            continue;
        }
        const size_t tree_offset = hash_offset + 16U;
        const size_t node_bytes = (size_t)expected->node_count * 32U;
        const size_t strings_offset = tree_offset + 8U + node_bytes;
        CHECK(row->tree.node_count == expected->node_count);
        CHECK(row->tree.string_byte_count == expected->string_bytes);
        CHECK(source_matches(row->tree.node_count_source, bytes, tree_offset, 4));
        CHECK(source_matches(row->tree.string_count_source, bytes, tree_offset + 4U, 4));
        CHECK(source_matches(row->tree.nodes_source, bytes, tree_offset + 8U, node_bytes));
        CHECK(source_matches(
            row->tree.strings_source, bytes, strings_offset, expected->string_bytes));
        CHECK(digest_matches(row->tree.nodes_source.data, node_bytes, expected->nodes_sha256));
        CHECK(digest_matches(
            row->tree.strings_source.data, expected->string_bytes, expected->strings_sha256));
        CHECK(source_matches(row->dependency_count_source, bytes, offset + size - 4U, 4));
        CHECK(source_matches(row->dependency_words_source, bytes, offset + size, 0));
    }
    CHECK(serialized_file_directory_type(directory, WRITER_TYPE_COUNT) == NULL);
    return true;
}

static bool check_directory_rows(const SerializedFileDirectory* directory,
    const uint8_t* bytes,
    bool with_tree,
    SerializedFileDirectoryEngineVersion engine_version) {
    const SerializedFileDirectoryView* view = serialized_file_directory_view(directory);
    const size_t objects_start = with_tree ? 2760U : 200U;
    const size_t object_count_offset = with_tree ? 2754U : 193U;
    const size_t directory_end = with_tree ? 2880U : 320U;
    CHECK(view && view->engine_version == engine_version);
    CHECK(view->prefix.header.file_size == WRITER_FILE_SIZE);
    CHECK(view->prefix.header.metadata_size == (with_tree ? 2869U : 309U));
    CHECK(view->prefix.header.data_offset == 4096U);
    CHECK(view->prefix.header.endian_selector == 0 && view->prefix.target_platform == 19U);
    CHECK(view->prefix.type_tree_enabled_raw == (with_tree ? 1U : 0U));
    CHECK(view->type_count == WRITER_TYPE_COUNT && view->object_count == WRITER_OBJECT_COUNT);
    CHECK(view->node_record_count == (with_tree ? 70U : 0U));
    CHECK(view->string_byte_count == (with_tree ? 273U : 0U));
    CHECK(view->dependency_word_count == 0);
    CHECK(source_matches(view->type_count_source, bytes, 65, 4));
    CHECK(source_matches(view->type_rows_source, bytes, 69, object_count_offset - 69U));
    CHECK(source_matches(view->object_count_source, bytes, object_count_offset, 4));
    CHECK(source_matches(view->object_padding_source,
        bytes,
        object_count_offset + 4U,
        objects_start - object_count_offset - 4U));
    CHECK(
        source_matches(view->object_rows_source, bytes, objects_start, WRITER_OBJECT_COUNT * 24U));
    CHECK(source_matches(view->parsed_metadata_source, bytes, 48, directory_end - 48U));
    CHECK(view->remaining_metadata.offset == directory_end && view->remaining_metadata.size == 37U);
    CHECK(check_types(directory, bytes, with_tree));
    for (size_t ordinal = 0; ordinal < WRITER_OBJECT_COUNT; ++ordinal) {
        const WriterObjectObservation* expected = &writer_objects[ordinal];
        const SerializedFileDirectoryObjectRow* row =
            serialized_file_directory_object(directory, ordinal);
        CHECK(row && row->ordinal == ordinal);
        CHECK(source_matches(row->source, bytes, objects_start + 24U * ordinal, 24));
        CHECK(row->path_id_bits == expected->path_id);
        CHECK(row->relative_data_offset == expected->relative_offset);
        CHECK(row->byte_size == expected->byte_size && row->type_ordinal == expected->type_ordinal);
        CHECK(row->payload.offset == 4096U + expected->relative_offset);
        CHECK(row->payload.size == expected->byte_size);
    }
    CHECK(serialized_file_directory_object(directory, WRITER_OBJECT_COUNT) == NULL);
    return true;
}

static bool check_directory(
    const uint8_t* bytes, bool with_tree, SerializedFileDirectoryEngineVersion engine_version) {
    const SerializedFileDirectoryLimits limits = {12, 4, 5, 70, 273, 0, 2869, 8192, 0, 32768};
    SerializedFileDirectory directory;
    serialized_file_directory_init(&directory);
    const SerializedFileDirectoryResult result = serialized_file_directory_create(
        bytes, WRITER_FILE_SIZE, WRITER_FILE_SIZE, engine_version, &limits, &directory);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_OK);
    const bool matches = check_directory_rows(&directory, bytes, with_tree, engine_version);
    serialized_file_directory_dispose(&directory);
    CHECK(matches);
    return true;
}

static bool check_full_metadata(const uint8_t* bytes, bool with_tree) {
    SerializedFile file;
    CHECK(serialized_file_open_metadata(&file, bytes, WRITER_FILE_SIZE));
    bool matches = file.type_count == WRITER_TYPE_COUNT && file.object_count == WRITER_OBJECT_COUNT;
    for (size_t ordinal = 0; matches && ordinal < WRITER_TYPE_COUNT; ++ordinal) {
        const TypeTreeType* type = &file.types[ordinal];
        matches = type->type_id == (int32_t)writer_types[ordinal].class_id &&
            type->script_type_index == writer_types[ordinal].script_index &&
            type->node_count == (with_tree ? (int)writer_types[ordinal].node_count : 0);
    }
    for (size_t ordinal = 0; matches && ordinal < WRITER_OBJECT_COUNT; ++ordinal) {
        const AssetObjectInfo* object = &file.objects[ordinal];
        const WriterObjectObservation* expected = &writer_objects[ordinal];
        matches = object->path_id == (int64_t)expected->path_id &&
            object->byte_offset == expected->relative_offset &&
            object->byte_size == expected->byte_size &&
            object->type_id_or_index == (int32_t)expected->type_ordinal &&
            object->type_id == (int32_t)writer_types[expected->type_ordinal].class_id;
    }
    serialized_file_close(&file);
    CHECK(matches);
    return true;
}

static bool check_file(const char* path, bool with_tree) {
    uint8_t bytes[WRITER_FILE_SIZE];
    FILE* input = fopen(path, "rb");
    CHECK(input != NULL);
    const size_t size = fread(bytes, 1, sizeof(bytes), input);
    const bool exact_size = size == sizeof(bytes) && fgetc(input) == EOF && !ferror(input);
    const int closed = fclose(input);
    CHECK(exact_size && closed == 0);
    CHECK(digest_matches(bytes,
        size,
        with_tree ? "58955a4e1cb8c769315fab4a96483094263adaa0bd37771dd7e8468d552cf7f6"
                  : "de05057a639dcac017f1f816b6830f767a3977cc736aa1228533df4931f1b0c8"));
    CHECK(check_directory(bytes, with_tree, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1));
    CHECK(check_full_metadata(bytes, with_tree));

    /* A version-only copy tests the separately evidenced physical selection.
     * Common metadata support does not certify the pinned35 converter's later
     * version policy: that converter rejects the tree-free29 copy. */
    bytes[55] = '2';
    bytes[56] = '9';
    CHECK(check_directory(bytes, with_tree, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1));
    CHECK(check_full_metadata(bytes, with_tree));
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3 || !check_file(argv[1], true) || !check_file(argv[2], false)) {
        return 1;
    }
    puts(
        "Unity writer directory observations match both physical versions and metadata consumers.");
    return 0;
}
