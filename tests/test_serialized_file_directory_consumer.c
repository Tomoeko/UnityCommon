#include "io/serialized_file.h"

#include "io/serialized_file_directory.h"

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
    FIXTURE_SIZE = 260
};

static void store_scalar(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0; index < width; ++index) {
        const size_t destination = big_endian ? width - index - 1U : index;
        bytes[destination] = (uint8_t)value;
        value >>= 8U;
    }
}

static void initialize_header(uint8_t bytes[FIXTURE_SIZE],
    bool big_endian,
    bool with_tree,
    size_t metadata_end,
    size_t data_origin) {
    memset(bytes, 0, FIXTURE_SIZE);
    store_scalar(bytes + 8, 4, 22, true);
    store_scalar(bytes + 16, 8, metadata_end - 48U, true);
    store_scalar(bytes + 24, 8, FIXTURE_SIZE, true);
    store_scalar(bytes + 32, 8, data_origin, true);
    bytes[40] = big_endian ? 1U : 0U;
    memcpy(bytes + 48, "2021.3.35f1", 12);
    store_scalar(bytes + 60, 4, 19, big_endian);
    bytes[64] = with_tree ? 1U : 0U;
}

static void initialize_object(
    uint8_t* row, uint64_t path_id, uint64_t offset, uint32_t type_ordinal, bool big_endian) {
    store_scalar(row, 8, path_id, big_endian);
    store_scalar(row + 8, 8, offset, big_endian);
    store_scalar(row + 16, 4, 4, big_endian);
    store_scalar(row + 20, 4, type_ordinal, big_endian);
}

static void initialize_no_tree(uint8_t bytes[FIXTURE_SIZE], bool big_endian) {
    initialize_header(bytes, big_endian, false, 213, 224);
    store_scalar(bytes + 65, 4, 2, big_endian);
    /* The non114 row has a nonnegative script index and therefore a hash.
     * The class114 row has a negative index and still carries its script hash. */
    store_scalar(bytes + 69, 4, 49, big_endian);
    store_scalar(bytes + 74, 2, 0, big_endian);
    memset(bytes + 76, 0x31, 16);
    memset(bytes + 92, 0x32, 16);
    store_scalar(bytes + 108, 4, 114, big_endian);
    store_scalar(bytes + 113, 2, UINT16_MAX, big_endian);
    memset(bytes + 115, 0x41, 16);
    memset(bytes + 131, 0x42, 16);
    store_scalar(bytes + 147, 4, 2, big_endian);
    bytes[151] = 0xb7; /* Original alignment padding is not a zero requirement. */
    initialize_object(bytes + 152, UINT64_MAX, 0, 0, big_endian);
    initialize_object(bytes + 176, 17, 4, 1, big_endian);
    /* Three empty tail tables followed by empty user information. */
    memcpy(bytes + 224, "abcdefgh", 8);
}

static void initialize_node(uint8_t* node,
    uint8_t level,
    uint32_t type_offset,
    uint32_t name_offset,
    uint32_t index,
    bool big_endian) {
    store_scalar(node, 2, 1, big_endian);
    node[2] = level;
    store_scalar(node + 4, 4, type_offset, big_endian);
    store_scalar(node + 8, 4, name_offset, big_endian);
    store_scalar(node + 12, 4, 4, big_endian);
    store_scalar(node + 16, 4, index, big_endian);
    /* Official swap code leaves these eight bytes unchanged in both orders. */
    for (uint8_t byte = 0; byte < 8U; ++byte) {
        node[24U + byte] = (uint8_t)(byte + 1U);
    }
}

static void initialize_tree(uint8_t bytes[FIXTURE_SIZE], bool big_endian) {
    initialize_header(bytes, big_endian, true, 245, 256);
    store_scalar(bytes + 65, 4, 1, big_endian);
    store_scalar(bytes + 69, 4, 49, big_endian);
    store_scalar(bytes + 74, 2, UINT16_MAX, big_endian);
    memset(bytes + 76, 0x53, 16);
    store_scalar(bytes + 92, 4, 2, big_endian);
    store_scalar(bytes + 96, 4, 25, big_endian);
    initialize_node(bytes + 100, 0, 0, 10, 0, big_endian);
    initialize_node(bytes + 132, 1, 15, 19, 1, big_endian);
    memcpy(bytes + 164, "TextAsset\0Base\0int\0value\0", 25);
    store_scalar(bytes + 189, 4, 2, big_endian);
    store_scalar(bytes + 193, 4, UINT32_MAX, big_endian);
    store_scalar(bytes + 197, 4, UINT32_C(0x80000000), big_endian);
    store_scalar(bytes + 201, 4, 1, big_endian);
    memset(bytes + 205, 0xc7, 3);
    initialize_object(bytes + 208, UINT64_MAX, 0, 0, big_endian);
    store_scalar(bytes + 256, 4, 27, big_endian);
}

static bool physical_directory_accepts(const uint8_t bytes[FIXTURE_SIZE]) {
    const SerializedFileDirectoryLimits limits = {12, 2, 2, 2, 25, 2, 212, 8192, 0, 32768};
    SerializedFileDirectory directory;
    serialized_file_directory_init(&directory);
    const SerializedFileDirectoryResult result = serialized_file_directory_create(bytes,
        FIXTURE_SIZE,
        FIXTURE_SIZE,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        &directory);
    serialized_file_directory_dispose(&directory);
    CHECK(result.status == SERIALIZED_FILE_DIRECTORY_OK);
    return true;
}

static bool ordinary_hashes_and_payloads(bool big_endian) {
    uint8_t bytes[FIXTURE_SIZE];
    initialize_no_tree(bytes, big_endian);
    SerializedFile file;
    CHECK(serialized_file_open_metadata(&file, bytes, sizeof(bytes)));
    const bool matches = file.type_count == 2 && file.object_count == 2 &&
        file.types[0].type_id == 49 && file.types[0].script_type_index == 0 &&
        file.types[0].script_id_hash[0] == 0x31 && file.types[0].type_hash[0] == 0x32 &&
        file.types[1].type_id == 114 && file.types[1].script_type_index == UINT16_MAX &&
        file.types[1].script_id_hash[0] == 0x41 && file.types[1].type_hash[0] == 0x42 &&
        file.objects[0].path_id == -1 && file.objects[0].type_id == 49 &&
        file.objects[1].path_id == 17 && file.objects[1].type_id == 114 && file.script_count == 0 &&
        file.external_count == 0 && file.ref_type_count == 0 && file.user_information &&
        file.user_information[0] == '\0';
    size_t payload_size = 0;
    const uint8_t* payload =
        serialized_file_get_object_data(&file, &file.objects[1], &payload_size);
    const bool payload_matches =
        payload == bytes + 228 && payload_size == 4 && memcmp(payload, "efgh", 4) == 0;
    serialized_file_close(&file);
    CHECK(matches && payload_matches);
    return true;
}

static bool semantic_policy_remains_stricter(bool big_endian) {
    uint8_t bytes[FIXTURE_SIZE];
    SerializedFile file;
    initialize_no_tree(bytes, big_endian);
    bytes[73] = 2; /* Physical raw flag versus semantic boolean. */
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open_metadata(&file, bytes, sizeof(bytes)));
    CHECK(file.unity_version == NULL && file.types == NULL && file.objects == NULL);

    initialize_no_tree(bytes, big_endian);
    store_scalar(bytes + 176, 8, UINT64_MAX, big_endian);
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open_metadata(&file, bytes, sizeof(bytes)));

    initialize_no_tree(bytes, big_endian);
    store_scalar(bytes + 184, 8, 2, big_endian);
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open_metadata(&file, bytes, sizeof(bytes)));

    initialize_no_tree(bytes, big_endian);
    store_scalar(bytes + 16, 8, 200U - 48U, true);
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open_metadata(&file, bytes, sizeof(bytes)));

    initialize_no_tree(bytes, big_endian);
    bytes[56] = '6';
    CHECK(!serialized_file_open_metadata(&file, bytes, sizeof(bytes)));
    return true;
}

static bool tree_materialization_and_opaque_tail(bool big_endian) {
    uint8_t bytes[FIXTURE_SIZE];
    initialize_tree(bytes, big_endian);
    SerializedFile file;
    CHECK(serialized_file_open(&file, bytes, sizeof(bytes)));
    const TypeTreeType* type = &file.types[0];
    const bool matches = file.type_count == 1 && file.object_count == 1 && type->node_count == 2 &&
        type->dependency_count == 2 && type->dependencies[0] == -1 &&
        type->dependencies[1] == INT32_MIN && strcmp(type->nodes[0].type_str, "TextAsset") == 0 &&
        strcmp(type->nodes[1].name_str, "value") == 0 &&
        type->nodes[0].ref_type_hash == UINT64_C(0x0807060504030201) &&
        type->nodes[1].ref_type_hash == UINT64_C(0x0807060504030201);
    serialized_file_close(&file);
    CHECK(matches);

    initialize_tree(bytes, big_endian);
    bytes[134] = 3; /* Invalid preorder level remains a schema rejection. */
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open(&file, bytes, sizeof(bytes)));

    initialize_tree(bytes, big_endian);
    store_scalar(bytes + 140, 4, 20, big_endian); /* Mid-string name offset. */
    CHECK(physical_directory_accepts(bytes));
    CHECK(!serialized_file_open(&file, bytes, sizeof(bytes)));
    return true;
}

int main(void) {
    for (unsigned order = 0; order < 2; ++order) {
        const bool big_endian = order != 0;
        if (!ordinary_hashes_and_payloads(big_endian) ||
            !semantic_policy_remains_stricter(big_endian) ||
            !tree_materialization_and_opaque_tail(big_endian)) {
            return 1;
        }
    }
    puts("Ordinary directory materialization preserves semantic policy and opaque node tails.");
    return 0;
}
