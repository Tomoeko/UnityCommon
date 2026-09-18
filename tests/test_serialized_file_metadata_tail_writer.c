#include "io/serialized_file_metadata_tail.h"

#include "../src/io/typetree_directory_internal.h"

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
    WRITER_FILE_SIZE = 4336
};

typedef struct WriterTailObservation {
    bool tree;
    size_t metadata_end;
    size_t tail_start;
    size_t external_start;
    size_t reference_start;
    size_t reference_size;
    const char* file_sha256;
    const char* version29_sha256;
    const char* tail_sha256;
} WriterTailObservation;

/* Fixed raw observations of the two official exact35 writer outputs. These
 * hashes authenticate regression data; they are never product format rules. */
static const WriterTailObservation observations[] = {
    {true,
        2339U,
        1956U,
        1976U,
        2022U,
        316U,
        "0a2dbcb88979c18afb5331524fe5184f44af13c847279f9cefb514d6f585fea0",
        "164b986b9c944f69f9abc273203914709aba8b497b36d39894f7f4f27cb9d61f",
        "bb72c6350685c3c1cd20f4a75ba800e6fe5ebf0b6ab79116a7c3ddc0cf0f2b38"},
    {false,
        290U,
        184U,
        204U,
        250U,
        39U,
        "8e462110aa97dd2979c10ea20834e16ceb4c7fa35b41210ce913e86fca942e2e",
        "59aff4d064c04c844bee6822a473d4b2c6b9832c6a26c9f61ff8bb9d8b5ab5af",
        "78e7bf746e39e230313f469fc3d41f6fc5f712b4d046655943facaaf38ceb36a"}};

static bool digest_matches(const uint8_t* bytes, size_t size, const char* expected) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hexadecimal[COMMON_SHA256_HEX_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hexadecimal);
    CHECK(strcmp(hexadecimal, expected) == 0);
    return true;
}

static bool source_matches(
    SerializedFilePrefixSpan source, const uint8_t* bytes, size_t offset, size_t size) {
    CHECK(source.data == bytes + offset && source.offset == offset && source.size == size);
    return true;
}

static bool absent(SerializedFilePrefixSpan source) {
    CHECK(!source.data && source.offset == UINT64_MAX && !source.size);
    return true;
}

static bool zero_bytes(const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        CHECK(bytes[index] == 0U);
    }
    return true;
}

static bool reference_matches(const SerializedFileMetadataTailReferenceTypeRow* row,
    const uint8_t* bytes,
    const WriterTailObservation* expected) {
    const size_t start = expected->reference_start;
    CHECK(row && !row->ordinal && row->class_id_bits == UINT32_MAX && !row->stripped_raw &&
        !row->script_index_bits && row->has_script_hash && row->has_tree == expected->tree);
    CHECK(source_matches(row->source, bytes, start, expected->reference_size));
    CHECK(source_matches(row->class_id_source, bytes, start, 4U));
    CHECK(source_matches(row->stripped_source, bytes, start + 4U, 1U));
    CHECK(source_matches(row->script_index_source, bytes, start + 5U, 2U));
    CHECK(source_matches(row->script_hash_source, bytes, start + 7U, 16U));
    CHECK(source_matches(row->type_hash_source, bytes, start + 23U, 16U));
    CHECK(zero_bytes(row->script_hash_source.data, 16U));
    CHECK(zero_bytes(row->type_hash_source.data, 16U));
    if (!expected->tree) {
        CHECK(!row->tree.node_count && !row->tree.string_byte_count);
        CHECK(absent(row->tree.node_count_source) && absent(row->tree.string_count_source));
        CHECK(absent(row->tree.nodes_source) && absent(row->tree.strings_source));
        CHECK(absent(row->class_name_source) && absent(row->namespace_source) &&
            absent(row->assembly_name_source));
        return true;
    }
    CHECK(row->tree.node_count == 6U && row->tree.string_byte_count == 25U);
    CHECK(source_matches(row->tree.node_count_source, bytes, 2061U, 4U));
    CHECK(source_matches(row->tree.string_count_source, bytes, 2065U, 4U));
    CHECK(source_matches(row->tree.nodes_source, bytes, 2069U, 192U));
    CHECK(source_matches(row->tree.strings_source, bytes, 2261U, 25U));
    CHECK(digest_matches(row->tree.nodes_source.data,
        row->tree.nodes_source.size,
        "276d6f248a9243e02bcdc6220e1ed77ea340f8cb69643e944b76408df6a59ef9"));
    CHECK(digest_matches(row->tree.strings_source.data,
        row->tree.strings_source.size,
        "62a39695d74df985be31831a7c592359d229fa35203a85e7802747f82e1bd4f4"));
    CHECK(source_matches(row->class_name_source, bytes, 2286U, 12U));
    CHECK(source_matches(row->namespace_source, bytes, 2298U, 24U));
    CHECK(source_matches(row->assembly_name_source, bytes, 2322U, 16U));
    CHECK(memcmp(row->class_name_source.data, "TailPayload", 12U) == 0);
    CHECK(memcmp(row->namespace_source.data, "UnityRecoverTailFixture", 24U) == 0);
    CHECK(memcmp(row->assembly_name_source.data, "Assembly-CSharp", 16U) == 0);
    return true;
}

static bool materialized_reference_matches(
    const SerializedFileMetadataTailReferenceTypeRow* row, bool tree) {
    TypeTreeType type = {0};
    CHECK(typetree_materialize_metadata_tail_reference_type(&type, row, false));
    bool matches = type.is_ref_type && type.type_id == -1 && !type.is_stripped &&
        !type.script_type_index && !type.dependency_count && !type.dependencies &&
        zero_bytes(type.script_id_hash, 16U) && zero_bytes(type.type_hash, 16U);
    if (tree) {
        static const uint8_t levels[] = {0, 1, 1, 2, 3, 3};
        static const uint32_t type_offsets[] = {0U,
            UINT32_C(0x800000de),
            UINT32_C(0x80000348),
            UINT32_C(0x80000031),
            UINT32_C(0x800000de),
            UINT32_C(0x80000051)};
        static const uint32_t name_offsets[] = {UINT32_C(0x80000037),
            12U,
            19U,
            UINT32_C(0x80000031),
            UINT32_C(0x8000031b),
            UINT32_C(0x8000006a)};
        static const int32_t sizes[] = {-1, 4, -1, -1, 4, 1};
        static const uint32_t flags[] = {32768U, 0U, 32768U, 16385U, 1U, 1U};
        static const char* const types[] = {"TailPayload", "int", "string", "Array", "int", "char"};
        static const char* const names[] = {"Base", "marker", "label", "Array", "size", "data"};
        matches = matches && type.node_count == 6 && type.string_buffer_size == 25U && type.nodes &&
            type.string_buffer && type.ref_class_name && type.ref_namespace && type.ref_asm_name &&
            strcmp(type.ref_class_name, "TailPayload") == 0 &&
            strcmp(type.ref_namespace, "UnityRecoverTailFixture") == 0 &&
            strcmp(type.ref_asm_name, "Assembly-CSharp") == 0 &&
            memcmp(type.string_buffer, "TailPayload\0marker\0label", 25U) == 0;
        for (size_t index = 0U; matches && index < 6U; ++index) {
            const TypeTreeNode* node = &type.nodes[index];
            matches = node->version == 1U && node->level == levels[index] &&
                node->type_flags == (index == 3U ? 1U : 0U) &&
                node->type_str_offset == type_offsets[index] &&
                node->name_str_offset == name_offsets[index] && node->byte_size == sizes[index] &&
                node->index == index && node->meta_flags == flags[index] && !node->ref_type_hash &&
                node->type_str && node->name_str && strcmp(node->type_str, types[index]) == 0 &&
                strcmp(node->name_str, names[index]) == 0;
        }
    } else {
        matches = matches && !type.node_count && !type.nodes && !type.string_buffer_size &&
            !type.string_buffer && !type.ref_class_name && !type.ref_namespace &&
            !type.ref_asm_name;
    }
    typetree_free_type(&type);
    CHECK(matches);
    return true;
}

static bool tail_matches(const SerializedFileMetadataTail* tail,
    const uint8_t* bytes,
    const WriterTailObservation* expected,
    SerializedFileDirectoryEngineVersion version) {
    const SerializedFileMetadataTailView* view = serialized_file_metadata_tail_view(tail);
    const size_t start = expected->tail_start;
    CHECK(view && view->script_count == 1U && view->external_count == 1U &&
        view->reference_type_count == 1U && view->node_record_count == (expected->tree ? 6U : 0U) &&
        view->tree_string_byte_count == (expected->tree ? 25U : 0U) &&
        view->terminated_string_byte_count == (expected->tree ? 75U : 23U));
    CHECK(view->directory.engine_version == version && view->directory.type_count == 2U &&
        view->directory.object_count == 2U && view->directory.prefix.target_platform == 19U &&
        view->directory.prefix.header.endian_selector == 0U &&
        view->directory.prefix.header.file_size == WRITER_FILE_SIZE &&
        view->directory.prefix.header.metadata_size == expected->metadata_end - 48U &&
        view->directory.prefix.header.data_offset == 4096U &&
        view->directory.prefix.type_tree_enabled_raw == (expected->tree ? 1U : 0U));
    CHECK(source_matches(view->source, bytes, start, expected->metadata_end - start));
    CHECK(digest_matches(view->source.data, view->source.size, expected->tail_sha256));
    CHECK(source_matches(view->script_count_source, bytes, start, 4U));
    CHECK(source_matches(view->script_rows_source, bytes, start + 4U, 12U));
    CHECK(source_matches(view->external_count_source, bytes, start + 16U, 4U));
    CHECK(source_matches(view->external_rows_source, bytes, expected->external_start, 42U));
    CHECK(source_matches(
        view->reference_type_count_source, bytes, expected->reference_start - 4U, 4U));
    CHECK(source_matches(view->reference_type_rows_source,
        bytes,
        expected->reference_start,
        expected->reference_size));
    CHECK(source_matches(view->user_information_source, bytes, expected->metadata_end - 1U, 1U));
    CHECK(view->user_information_source.data[0] == 0U);
    const SerializedFileMetadataTailScriptRow* script =
        serialized_file_metadata_tail_script(tail, 0U);
    CHECK(script && !script->ordinal && script->file_index_bits == 1U &&
        script->local_identifier_bits == 5009U);
    CHECK(source_matches(script->source, bytes, start + 4U, 12U));
    CHECK(source_matches(script->file_index_source, bytes, start + 4U, 4U));
    CHECK(source_matches(script->alignment_source, bytes, start + 8U, 0U));
    CHECK(source_matches(script->local_identifier_source, bytes, start + 8U, 8U));
    const SerializedFileMetadataTailExternalRow* external =
        serialized_file_metadata_tail_external(tail, 0U);
    const size_t external_start = expected->external_start;
    CHECK(external && !external->ordinal && !external->type_bits);
    CHECK(source_matches(external->source, bytes, external_start, 42U));
    CHECK(source_matches(external->leading_string_source, bytes, external_start, 1U));
    CHECK(source_matches(external->guid_source, bytes, external_start + 1U, 16U));
    CHECK(source_matches(external->type_source, bytes, external_start + 17U, 4U));
    CHECK(source_matches(external->path_source, bytes, external_start + 21U, 21U));
    CHECK(external->leading_string_source.data[0] == 0U &&
        zero_bytes(external->guid_source.data, 16U));
    CHECK(memcmp(external->path_source.data, "tail-external.assets", 21U) == 0);
    for (size_t word = 0U; word < 4U; ++word) {
        CHECK(!external->guid_words[word]);
    }
    const SerializedFileMetadataTailReferenceTypeRow* reference =
        serialized_file_metadata_tail_reference_type(tail, 0U);
    CHECK(reference_matches(reference, bytes, expected));
    CHECK(materialized_reference_matches(reference, expected->tree));
    CHECK(!serialized_file_metadata_tail_script(tail, 1U));
    CHECK(!serialized_file_metadata_tail_external(tail, 1U));
    CHECK(!serialized_file_metadata_tail_reference_type(tail, 1U));
    return true;
}

static bool physical_tail(const uint8_t* bytes,
    const WriterTailObservation* expected,
    SerializedFileDirectoryEngineVersion version) {
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 2U, 2U, 128U, 512U, 16U, 4096U, 8192U, 0U, 65536U};
    const SerializedFileMetadataTailLimits tail_limits = {
        1U, 1U, 1U, 6U, 25U, 75U, 383U, 8192U, 0U, 32768U};
    SerializedFileDirectory directory;
    serialized_file_directory_init(&directory);
    const SerializedFileDirectoryResult admitted = serialized_file_directory_create(
        bytes, expected->tail_start, WRITER_FILE_SIZE, version, &directory_limits, &directory);
    CHECK(admitted.status == SERIALIZED_FILE_DIRECTORY_OK);
    if (expected->tree) {
        /* The official file's ordinary managed-reference schema is outside
         * the existing semantic grammar. Tail physical acceptance must not
         * silently widen that policy, while its reference schema is supported. */
        for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
            TypeTreeType ordinary = {0};
            const bool materialized = typetree_materialize_directory_type(
                &ordinary, serialized_file_directory_type(&directory, ordinal), false);
            typetree_free_type(&ordinary);
            CHECK(materialized == (ordinal == 0U));
        }
    }
    SerializedFileMetadataTail tail;
    serialized_file_metadata_tail_init(&tail);
    const SerializedFileMetadataTailResult result = serialized_file_metadata_tail_create(
        &directory, bytes, expected->metadata_end, WRITER_FILE_SIZE, &tail_limits, &tail);
    CHECK(result.status == SERIALIZED_FILE_METADATA_TAIL_OK);
    serialized_file_directory_dispose(&directory);
    const bool matches = tail_matches(&tail, bytes, expected, version);
    serialized_file_metadata_tail_dispose(&tail);
    CHECK(matches);
    return true;
}

static bool metadata_consumer(const uint8_t* bytes, bool tree) {
    SerializedFile file;
    const bool opened = serialized_file_open_metadata(&file, bytes, WRITER_FILE_SIZE);
    if (tree) {
        CHECK(!opened);
        serialized_file_close(&file);
        CHECK(!g_allocations_count && !g_allocated_bytes);
        return true;
    }
    CHECK(opened);
    bool matches = file.type_count == 2 && file.object_count == 2 && file.script_count == 1 &&
        file.external_count == 1 && file.ref_type_count == 1 && file.types && file.objects &&
        file.scripts && file.externals && file.ref_types && file.user_information &&
        !file.user_information[0] && file.scripts[0].file_id == 1 &&
        file.scripts[0].path_id == 5009 && file.externals[0].virtual_path &&
        !file.externals[0].virtual_path[0] && file.externals[0].path_name &&
        strcmp(file.externals[0].path_name, "tail-external.assets") == 0 &&
        file.externals[0].type == 0 && zero_bytes(file.externals[0].guid, 16U) &&
        file.ref_types[0].is_ref_type && file.ref_types[0].type_id == -1 &&
        !file.ref_types[0].script_type_index && !file.ref_types[0].node_count &&
        !file.ref_types[0].ref_class_name && !file.ref_types[0].ref_namespace &&
        !file.ref_types[0].ref_asm_name && zero_bytes(file.ref_types[0].script_id_hash, 16U) &&
        zero_bytes(file.ref_types[0].type_hash, 16U);
    matches = matches && file.objects[0].path_id == 1001 && file.objects[0].byte_offset == 0U &&
        file.objects[0].byte_size == 52U && file.objects[1].path_id == 2003 &&
        file.objects[1].byte_offset == 56U && file.objects[1].byte_size == 184U;
    serialized_file_close(&file);
    CHECK(matches && !g_allocations_count && !g_allocated_bytes);
    return true;
}

static bool writer_file(const char* path, const WriterTailObservation* expected) {
    uint8_t bytes[WRITER_FILE_SIZE];
    FILE* input = fopen(path, "rb");
    CHECK(input);
    const size_t count = fread(bytes, 1U, sizeof(bytes), input);
    const bool exact_size = count == sizeof(bytes) && fgetc(input) == EOF && !ferror(input);
    const int closed = fclose(input);
    CHECK(exact_size && closed == 0);
    CHECK(digest_matches(bytes, sizeof(bytes), expected->file_sha256));
    CHECK(physical_tail(bytes, expected, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1));
    CHECK(metadata_consumer(bytes, expected->tree));

    /* This controlled copy selects only separately evidenced physical29
     * compatibility. The pinned35 converter rejects its tree-free version;
     * no complete29 reader or project-recovery certificate is claimed. */
    bytes[55] = '2';
    bytes[56] = '9';
    CHECK(digest_matches(bytes, sizeof(bytes), expected->version29_sha256));
    CHECK(physical_tail(bytes, expected, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1));
    CHECK(!g_allocations_count && !g_allocated_bytes);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3 || !writer_file(argv[1], &observations[0]) ||
        !writer_file(argv[2], &observations[1])) {
        return 1;
    }
    puts("Unity writer metadata tails: exact35 and physical29 observations match.");
    return 0;
}
