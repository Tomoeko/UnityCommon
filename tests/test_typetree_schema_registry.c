#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "io/typetree_schema_registry.h"
#include "io/serialized_file.h"

#include "common/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static const uint8_t k_local_strings[] =
    "Shader\0Base\0int\0value\0";

typedef struct {
    uint8_t bytes[1024];
    size_t size;
    size_t type_hash_offset;
} SerializedFileFixture;

static bool fixture_bytes(SerializedFileFixture* fixture, const void* bytes,
                          size_t size) {
    if (!fixture || (!bytes && size != 0U) ||
        size > sizeof(fixture->bytes) - fixture->size) {
        return false;
    }
    memcpy(fixture->bytes + fixture->size, bytes, size);
    fixture->size += size;
    return true;
}

static bool fixture_u8(SerializedFileFixture* fixture, uint8_t value) {
    return fixture_bytes(fixture, &value, sizeof(value));
}

static bool fixture_le16(SerializedFileFixture* fixture, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8U)};
    return fixture_bytes(fixture, bytes, sizeof(bytes));
}

static bool fixture_le32(SerializedFileFixture* fixture, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U), (uint8_t)(value >> 24U)};
    return fixture_bytes(fixture, bytes, sizeof(bytes));
}

static bool fixture_le64(SerializedFileFixture* fixture, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8U; i++) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
    return fixture_bytes(fixture, bytes, sizeof(bytes));
}

static void fixture_be32_at(uint8_t* bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)(value >> 24U);
    bytes[offset + 1U] = (uint8_t)(value >> 16U);
    bytes[offset + 2U] = (uint8_t)(value >> 8U);
    bytes[offset + 3U] = (uint8_t)value;
}

static void fixture_be64_at(uint8_t* bytes, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8U; i++) {
        bytes[offset + i] = (uint8_t)(value >> ((7U - i) * 8U));
    }
}

static bool build_serialized_file_fixture(SerializedFileFixture* fixture,
                                          bool type_tree_enabled,
                                          uint32_t node_meta_flags) {
    memset(fixture, 0, sizeof(*fixture));
    const uint8_t header[48] = {0};
    CHECK(fixture_bytes(fixture, header, sizeof(header)));
    const size_t metadata_start = fixture->size;
    static const char unity_version[] = "2021.3.35f1";
    CHECK(fixture_bytes(fixture, unity_version, sizeof(unity_version)));
    CHECK(fixture_le32(fixture, 19U));
    CHECK(fixture_u8(fixture, type_tree_enabled ? 1U : 0U));
    CHECK(fixture_le32(fixture, 1U)); /* type count */
    CHECK(fixture_le32(fixture, 48U)); /* class/type ID */
    CHECK(fixture_u8(fixture, 0U)); /* not stripped */
    CHECK(fixture_le16(fixture, UINT16_MAX));
    fixture->type_hash_offset = fixture->size;
    for (uint8_t i = 0; i < 16U; i++) {
        CHECK(fixture_u8(fixture, (uint8_t)(0x40U + i)));
    }
    if (type_tree_enabled) {
        static const uint8_t strings[] = "int\0Base\0";
        CHECK(fixture_le32(fixture, 1U)); /* node count */
        CHECK(fixture_le32(fixture, (uint32_t)sizeof(strings)));
        CHECK(fixture_le16(fixture, 1U)); /* node version */
        CHECK(fixture_u8(fixture, 0U)); /* level */
        CHECK(fixture_u8(fixture, 0U)); /* type flags */
        CHECK(fixture_le32(fixture, 0U)); /* int */
        CHECK(fixture_le32(fixture, 4U)); /* Base */
        CHECK(fixture_le32(fixture, 4U)); /* byte size */
        CHECK(fixture_le32(fixture, 0U)); /* index */
        CHECK(fixture_le32(fixture, node_meta_flags));
        CHECK(fixture_le64(fixture, 0U)); /* ref type hash */
        CHECK(fixture_bytes(fixture, strings, sizeof(strings)));
        CHECK(fixture_le32(fixture, 0U)); /* dependencies */
    }
    CHECK(fixture_le32(fixture, 0U)); /* objects */
    CHECK(fixture_le32(fixture, 0U)); /* scripts */
    CHECK(fixture_le32(fixture, 0U)); /* externals */
    CHECK(fixture_le32(fixture, 0U)); /* reference types */
    CHECK(fixture_u8(fixture, 0U)); /* user information */
    const size_t metadata_size = fixture->size - metadata_start;
    while ((fixture->size & 15U) != 0U) CHECK(fixture_u8(fixture, 0U));

    fixture_be32_at(fixture->bytes, 8U, 22U);
    fixture_be32_at(fixture->bytes, 20U, (uint32_t)metadata_size);
    fixture_be64_at(fixture->bytes, 24U, fixture->size);
    fixture_be64_at(fixture->bytes, 32U, fixture->size);
    return true;
}

static uint64_t test_load_le64(const uint8_t* bytes) {
    uint64_t value = 0U;
    for (unsigned i = 0; i < 8U; ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}

static void write_le64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> (8U * i));
    }
}

static void refresh_payload_digest(uint8_t* bytes, size_t size) {
    common_sha256(bytes + 64U, size - 64U, bytes + 32U);
}

static void initialize_schema(TypeTreeType* schema, int32_t type_id,
                              uint8_t hash_seed) {
    memset(schema, 0, sizeof(*schema));
    schema->type_id = type_id;
    schema->script_type_index = UINT16_MAX;
    for (size_t i = 0; i < sizeof(schema->type_hash); ++i) {
        schema->type_hash[i] = (uint8_t)(hash_seed + i);
    }
    schema->node_count = 2;
    schema->nodes = (TypeTreeNode*)mem_alloc(
        (size_t)schema->node_count * sizeof(*schema->nodes));
    memset(schema->nodes, 0,
           (size_t)schema->node_count * sizeof(*schema->nodes));
    schema->string_buffer_size = sizeof(k_local_strings);
    schema->string_buffer = (uint8_t*)mem_alloc(schema->string_buffer_size);
    memcpy(schema->string_buffer, k_local_strings,
           schema->string_buffer_size);
    schema->dependency_count = 1;
    schema->dependencies = (int32_t*)mem_alloc(sizeof(*schema->dependencies));
    schema->dependencies[0] = 48;

    TypeTreeNode* root = &schema->nodes[0];
    root->version = 1;
    root->level = 0;
    root->type_str_offset = 0;
    root->name_str_offset = 7;
    root->byte_size = 4;
    root->index = 0;
    root->type_str = "Shader";
    root->name_str = "Base";

    TypeTreeNode* child = &schema->nodes[1];
    child->version = 1;
    child->level = 1;
    child->type_str_offset = 12;
    child->name_str_offset = 16;
    child->byte_size = 4;
    child->index = 1;
    child->meta_flags = 0x4000U;
    child->type_str = "int";
    child->name_str = "value";
}

static TypeTreeSchemaStatus make_key(TypeTreeSchemaKey* key,
                                     const TypeTreeType* schema,
                                     const char* unity_version) {
    return typetree_schema_key_from_type(
        key, 22U, unity_version, schema->type_id, schema);
}

static bool verify_deep_copy_and_exact_key(void) {
    TypeTreeType source;
    initialize_schema(&source, 48, 7U);
    TypeTreeSchemaKey key;
    CHECK(make_key(&key, &source, "2021.3.35f1") == TYPETREE_SCHEMA_OK);

    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_learn(
              &registry, &key, &source,
              TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT) ==
          TYPETREE_SCHEMA_INVALID_ARGUMENT);
    CHECK(typetree_schema_registry_learn(
              &registry, &key, &source,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&registry) == 1U);
    CHECK(typetree_schema_registry_learn(
              &registry, &key, &source,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&registry) == 1U);

    source.nodes[1].meta_flags = 0U;
    TypeTreeType copy;
    CHECK(typetree_schema_registry_lookup(&registry, &key, &copy) ==
          TYPETREE_SCHEMA_OK);
    CHECK(copy.nodes != source.nodes);
    CHECK(copy.string_buffer != source.string_buffer);
    CHECK(copy.dependencies != source.dependencies);
    CHECK(copy.nodes[1].meta_flags == 0x4000U);
    copy.nodes[1].meta_flags = 99U;
    typetree_free_type(&copy);

    CHECK(typetree_schema_registry_learn(
              &registry, &key, &source,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_KEY_CONFLICT);
    source.nodes[1].meta_flags = 0x4000U;

    TypeTreeSchemaKey miss = key;
    const char different_version[] = "2021.3.36f1";
    miss.unity_version = different_version;
    miss.unity_version_size = strlen(different_version);
    CHECK(typetree_schema_registry_lookup(&registry, &miss, &copy) ==
          TYPETREE_SCHEMA_NOT_FOUND);
    miss = key;
    miss.type_hash[0] ^= 1U;
    CHECK(typetree_schema_registry_lookup(&registry, &miss, &copy) ==
          TYPETREE_SCHEMA_NOT_FOUND);

    typetree_schema_registry_dispose(&registry);
    typetree_free_type(&source);
    return true;
}

static void initialize_string_schema(TypeTreeType* schema) {
    memset(schema, 0, sizeof(*schema));
    schema->node_count = 5;
    schema->nodes = (TypeTreeNode*)mem_alloc(
        (size_t)schema->node_count * sizeof(*schema->nodes));
    memset(schema->nodes, 0,
           (size_t)schema->node_count * sizeof(*schema->nodes));

    TypeTreeNode* root = &schema->nodes[0];
    root->level = 0;
    root->byte_size = -1;
    root->type_str = "Shader";
    root->name_str = "Base";

    TypeTreeNode* string = &schema->nodes[1];
    string->level = 1;
    string->byte_size = -1;
    string->type_str = "string";
    string->name_str = "text";

    TypeTreeNode* array = &schema->nodes[2];
    array->level = 2;
    array->byte_size = -1;
    array->type_str = "Array";
    array->name_str = "Array";

    TypeTreeNode* size = &schema->nodes[3];
    size->level = 3;
    size->byte_size = 4;
    size->type_str = "int";
    size->name_str = "size";

    TypeTreeNode* data = &schema->nodes[4];
    data->level = 3;
    data->byte_size = 1;
    data->type_str = "char";
    data->name_str = "data";
}

static bool verify_schema_grammar_rejection(void) {
    TypeTreeType schema;
    initialize_schema(&schema, 48, 7U);
    CHECK(typetree_validate_schema(&schema));
    TypeTreeSchemaKey key;
    CHECK(make_key(&key, &schema, "2021.3.35f1") == TYPETREE_SCHEMA_OK);
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);

    schema.nodes[1].level = 0;
    CHECK(!typetree_validate_schema(&schema));
    CHECK(typetree_schema_registry_learn(
              &registry, &key, &schema,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_INVALID_SCHEMA);
    CHECK(typetree_schema_registry_count(&registry) == 0U);

    schema.nodes[1].level = 2;
    CHECK(!typetree_validate_schema(&schema));
    schema.nodes[1].level = 1;
    schema.nodes[1].byte_size = 8;
    CHECK(!typetree_validate_schema(&schema));

    uint8_t payload[8] = {0};
    ByteStream stream;
    stream_init(&stream, payload, sizeof(payload));
    int node_index = 0;
    TypeTreeValue value;
    memset(&value, 0, sizeof(value));
    CHECK(!typetree_parse_value(&schema, &node_index, &stream, &value));
    CHECK(stream.position == 0U && node_index == 0);
    schema.nodes[1].byte_size = 4;
    typetree_schema_registry_dispose(&registry);
    typetree_free_type(&schema);

    initialize_string_schema(&schema);
    CHECK(typetree_validate_schema(&schema));
    schema.nodes[2].name_str = "Broken";
    CHECK(!typetree_validate_schema(&schema));
    schema.nodes[2].name_str = "Array";
    schema.nodes[3].type_str = "UInt32";
    CHECK(!typetree_validate_schema(&schema));
    schema.nodes[3].type_str = "int";
    schema.nodes[4].byte_size = 2;
    CHECK(!typetree_validate_schema(&schema));
    typetree_free_type(&schema);
    return true;
}

static bool verify_long_schema_names_roundtrip(void) {
    enum { LONG_NAME_SIZE = 257 };
    char long_name[LONG_NAME_SIZE];
    for (size_t i = 0; i + 1U < sizeof(long_name); ++i) {
        long_name[i] = (char)('a' + (i % 26U));
    }
    long_name[sizeof(long_name) - 1U] = '\0';

    static const char prefix[] = "Shader\0Base\0int\0";
    TypeTreeType schema;
    memset(&schema, 0, sizeof(schema));
    schema.type_id = 48;
    schema.script_type_index = UINT16_MAX;
    schema.type_hash[0] = 0x91U;
    schema.node_count = 2;
    schema.nodes = (TypeTreeNode*)mem_alloc(
        (size_t)schema.node_count * sizeof(*schema.nodes));
    CHECK(schema.nodes != NULL);
    memset(schema.nodes, 0,
           (size_t)schema.node_count * sizeof(*schema.nodes));
    schema.string_buffer_size =
        (uint32_t)(sizeof(prefix) + sizeof(long_name));
    schema.string_buffer = (uint8_t*)mem_alloc(schema.string_buffer_size);
    CHECK(schema.string_buffer != NULL);
    memcpy(schema.string_buffer, prefix, sizeof(prefix));
    memcpy(schema.string_buffer + sizeof(prefix), long_name,
           sizeof(long_name));

    TypeTreeNode* root = &schema.nodes[0];
    root->level = 0;
    root->type_str_offset = 0U;
    root->name_str_offset = 7U;
    root->byte_size = 4;
    root->type_str = "unbound-root-type";
    root->name_str = "unbound-root-name";
    TypeTreeNode* child = &schema.nodes[1];
    child->level = 1;
    child->type_str_offset = 12U;
    child->name_str_offset = schema.string_buffer_size;
    child->byte_size = 4;
    child->index = 1U;
    child->type_str = "unbound-child-type";
    child->name_str = "unbound-child-name";

    CHECK(!typetree_bind_node_strings(&schema));
    CHECK(strcmp(root->type_str, "unbound-root-type") == 0);
    CHECK(strcmp(child->name_str, "unbound-child-name") == 0);
    child->name_str_offset = (uint32_t)sizeof(prefix);
    CHECK(typetree_bind_node_strings(&schema));
    CHECK(strcmp(child->name_str, long_name) == 0);
    CHECK(strlen(child->name_str) == sizeof(long_name) - 1U);
    CHECK(typetree_validate_schema(&schema));

    TypeTreeSchemaKey key;
    CHECK(make_key(&key, &schema, "2021.3.35f1") == TYPETREE_SCHEMA_OK);
    TypeTreeSchemaRegistry registry;
    TypeTreeSchemaRegistry loaded;
    typetree_schema_registry_init(&registry);
    typetree_schema_registry_init(&loaded);
    CHECK(typetree_schema_registry_learn(
              &registry, &key, &schema,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);

    uint8_t* bytes = NULL;
    size_t byte_count = 0U;
    CHECK(typetree_schema_registry_serialize(&registry, &bytes,
                                             &byte_count) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, bytes, byte_count) == TYPETREE_SCHEMA_OK);
    TypeTreeType copy;
    CHECK(typetree_schema_registry_lookup(&loaded, &key, &copy) ==
          TYPETREE_SCHEMA_OK);
    CHECK(strcmp(copy.nodes[1].name_str, long_name) == 0);
    CHECK(copy.nodes[1].name_str != schema.nodes[1].name_str);

    const uint8_t object_bytes[] = {0x78U, 0x56U, 0x34U, 0x12U};
    ByteStream stream;
    stream_init(&stream, object_bytes, sizeof(object_bytes));
    TypeTreeValue value;
    memset(&value, 0, sizeof(value));
    int node_index = 0;
    CHECK(typetree_parse_value(&copy, &node_index, &stream, &value));
    CHECK(value.type == VAL_TYPE_STRUCT);
    const TypeTreeValue* long_value =
        typetree_find_path(&value, long_name);
    CHECK(long_value != NULL);
    CHECK(long_value->name == copy.nodes[1].name_str);
    CHECK(long_value->type == VAL_TYPE_INT);
    CHECK(long_value->int_val == 0x12345678);

    typetree_free_value(&value);
    typetree_free_type(&copy);
    mem_free(bytes, byte_count);
    typetree_schema_registry_dispose(&loaded);
    typetree_schema_registry_dispose(&registry);
    typetree_free_type(&schema);
    return true;
}

static bool learn_schema(TypeTreeSchemaRegistry* registry,
                         TypeTreeType* schema,
                         const char* unity_version) {
    TypeTreeSchemaKey key;
    CHECK(make_key(&key, schema, unity_version) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_learn(
              registry, &key, schema,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    return true;
}

static bool verify_canonical_roundtrip_and_corruption(void) {
    TypeTreeType first;
    TypeTreeType second;
    initialize_schema(&first, 48, 1U);
    initialize_schema(&second, 49, 33U);

    TypeTreeSchemaRegistry forward;
    TypeTreeSchemaRegistry reverse;
    typetree_schema_registry_init(&forward);
    typetree_schema_registry_init(&reverse);
    CHECK(learn_schema(&forward, &first, "2021.3.35f1"));
    CHECK(learn_schema(&forward, &second, "2022.3.0f1"));
    CHECK(learn_schema(&reverse, &second, "2022.3.0f1"));
    CHECK(learn_schema(&reverse, &first, "2021.3.35f1"));

    uint8_t* forward_bytes = NULL;
    uint8_t* reverse_bytes = NULL;
    size_t forward_size = 0U;
    size_t reverse_size = 0U;
    CHECK(typetree_schema_registry_serialize(
              &forward, &forward_bytes, &forward_size) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_serialize(
              &reverse, &reverse_bytes, &reverse_size) ==
          TYPETREE_SCHEMA_OK);
    CHECK(forward_size == reverse_size);
    CHECK(memcmp(forward_bytes, reverse_bytes, forward_size) == 0);
    TypeTreeSchemaKey view_key;
    const TypeTreeType* view_schema = NULL;
    CHECK(typetree_schema_registry_entry_view(
              &reverse, 0U, &view_key, &view_schema) == TYPETREE_SCHEMA_OK);
    CHECK(view_key.class_id == 48 && view_schema != NULL &&
          view_schema->type_id == 48);
    CHECK(typetree_schema_registry_entry_view(
              &reverse, 1U, &view_key, &view_schema) == TYPETREE_SCHEMA_OK);
    CHECK(view_key.class_id == 49 && view_schema != NULL &&
          view_schema->type_id == 49);
    CHECK(typetree_schema_registry_entry_view(
              &reverse, 2U, &view_key, &view_schema) ==
          TYPETREE_SCHEMA_NOT_FOUND);
    CHECK(view_schema == NULL);

    TypeTreeSchemaRegistry loaded;
    typetree_schema_registry_init(&loaded);
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, forward_bytes, forward_size) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&loaded) == 2U);
    TypeTreeSchemaKey second_key;
    CHECK(make_key(&second_key, &second, "2022.3.0f1") ==
          TYPETREE_SCHEMA_OK);
    TypeTreeType copy;
    CHECK(typetree_schema_registry_lookup(&loaded, &second_key, &copy) ==
          TYPETREE_SCHEMA_OK);
    CHECK(copy.type_id == second.type_id);
    CHECK(copy.nodes[1].meta_flags == second.nodes[1].meta_flags);
    typetree_free_type(&copy);

    size_t prior_count = typetree_schema_registry_count(&loaded);

    /* A valid registry digest is integrity, not permission to bypass schema
     * grammar. Make the second node another level-0 root and verify the
     * imported replacement is rejected transactionally. */
    const size_t first_unity_version_size = strlen("2021.3.35f1");
    const size_t first_node_offset =
        64U + 16U + first_unity_version_size + 72U;
    const size_t second_node_level_offset = first_node_offset + 32U + 2U;
    CHECK(second_node_level_offset < forward_size);
    CHECK(forward_bytes[second_node_level_offset] == 1U);
    forward_bytes[second_node_level_offset] = 0U;
    refresh_payload_digest(forward_bytes, forward_size);
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, forward_bytes, forward_size) ==
          TYPETREE_SCHEMA_INVALID_SCHEMA);
    CHECK(typetree_schema_registry_count(&loaded) == prior_count);
    forward_bytes[second_node_level_offset] = 1U;
    refresh_payload_digest(forward_bytes, forward_size);

    forward_bytes[forward_size - 1U] ^= 0x80U;
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, forward_bytes, forward_size) ==
          TYPETREE_SCHEMA_DIGEST_MISMATCH);
    CHECK(typetree_schema_registry_count(&loaded) == prior_count);
    CHECK(typetree_schema_registry_lookup(&loaded, &second_key, &copy) ==
          TYPETREE_SCHEMA_OK);
    typetree_free_type(&copy);
    forward_bytes[forward_size - 1U] ^= 0x80U;

    uint8_t original_total_size[8];
    memcpy(original_total_size, forward_bytes + 24U,
           sizeof(original_total_size));
    forward_bytes[24U] ^= 1U;
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, forward_bytes, forward_size) ==
          TYPETREE_SCHEMA_INVALID_FORMAT);
    CHECK(typetree_schema_registry_count(&loaded) == prior_count);
    memcpy(forward_bytes + 24U, original_total_size,
           sizeof(original_total_size));

    uint64_t first_record_size = test_load_le64(forward_bytes + 64U);
    CHECK(first_record_size > 8U);
    write_le64(forward_bytes + 64U, first_record_size - 1U);
    refresh_payload_digest(forward_bytes, forward_size);
    CHECK(typetree_schema_registry_deserialize_replace(
              &loaded, forward_bytes, forward_size) ==
          TYPETREE_SCHEMA_INVALID_FORMAT);
    CHECK(typetree_schema_registry_count(&loaded) == prior_count);
    write_le64(forward_bytes + 64U, first_record_size);
    refresh_payload_digest(forward_bytes, forward_size);

    mem_free(forward_bytes, forward_size);
    mem_free(reverse_bytes, reverse_size);
    typetree_schema_registry_dispose(&loaded);
    typetree_schema_registry_dispose(&reverse);
    typetree_schema_registry_dispose(&forward);
    typetree_free_type(&second);
    typetree_free_type(&first);
    return true;
}

#define REGISTRY_TEST_PATH_CAPACITY 1024U

static bool registry_test_make_directory(char* path, size_t capacity) {
    if (!path || capacity == 0U) return false;
#ifdef _WIN32
    char temporary_root[MAX_PATH];
    char temporary_file[MAX_PATH];
    DWORD root_size = GetTempPathA(MAX_PATH, temporary_root);
    if (root_size == 0U || root_size >= MAX_PATH ||
        GetTempFileNameA(temporary_root, "dxt", 0U, temporary_file) == 0U ||
        !DeleteFileA(temporary_file) ||
        !CreateDirectoryA(temporary_file, NULL)) {
        return false;
    }
    int count = snprintf(path, capacity, "%s", temporary_file);
    if (count < 0 || (size_t)count >= capacity) {
        (void)RemoveDirectoryA(temporary_file);
        return false;
    }
    return true;
#else
    char pattern[] = "/tmp/unity_common_typetree_registry.XXXXXX";
    int descriptor = mkstemp(pattern);
    if (descriptor < 0) return false;
    bool created = close(descriptor) == 0 && unlink(pattern) == 0 &&
                   mkdir(pattern, 0700) == 0;
    if (!created) {
        (void)unlink(pattern);
        (void)rmdir(pattern);
        return false;
    }
    int count = snprintf(path, capacity, "%s", pattern);
    if (count < 0 || (size_t)count >= capacity) {
        (void)rmdir(pattern);
        return false;
    }
    return true;
#endif
}

static bool registry_test_join_path(char* output, size_t capacity,
                                    const char* directory,
                                    const char* name) {
    int count = snprintf(output, capacity, "%s/%s", directory, name);
    return count >= 0 && (size_t)count < capacity;
}

#ifdef _WIN32
static bool registry_test_delete_utf8(const char* path) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    bool deleted = DeleteFileW(wide_path) != 0;
    free(wide_path);
    return deleted;
}
#endif

static bool registry_has_schema(const TypeTreeSchemaRegistry* registry,
                                const TypeTreeType* schema,
                                const char* unity_version) {
    TypeTreeSchemaKey key;
    if (make_key(&key, schema, unity_version) != TYPETREE_SCHEMA_OK) {
        return false;
    }
    TypeTreeType copy;
    TypeTreeSchemaStatus status = typetree_schema_registry_lookup(
        registry, &key, &copy);
    if (status == TYPETREE_SCHEMA_OK) typetree_free_type(&copy);
    return status == TYPETREE_SCHEMA_OK;
}

#ifndef _WIN32
static bool registry_is_exactly_one_of(
    const TypeTreeSchemaRegistry* registry,
    const TypeTreeType* first,
    const TypeTreeType* second) {
    bool has_first = registry_has_schema(registry, first, "2021.3.35f1");
    bool has_second = registry_has_schema(registry, second, "2022.3.0f1");
    return typetree_schema_registry_count(registry) == 1U &&
           has_first != has_second;
}
#endif

static bool verify_atomic_file_rewrite_and_restrictions(void) {
    TypeTreeType first;
    TypeTreeType second;
    initialize_schema(&first, 48, 1U);
    initialize_schema(&second, 49, 33U);
    TypeTreeSchemaRegistry first_registry;
    TypeTreeSchemaRegistry second_registry;
    TypeTreeSchemaRegistry loaded;
    typetree_schema_registry_init(&first_registry);
    typetree_schema_registry_init(&second_registry);
    typetree_schema_registry_init(&loaded);
    CHECK(learn_schema(&first_registry, &first, "2021.3.35f1"));
    CHECK(learn_schema(&second_registry, &second, "2022.3.0f1"));

    char directory[REGISTRY_TEST_PATH_CAPACITY];
    char registry_path[REGISTRY_TEST_PATH_CAPACITY];
    char directory_path[REGISTRY_TEST_PATH_CAPACITY];
    char corrupt_path[REGISTRY_TEST_PATH_CAPACITY];
    CHECK(registry_test_make_directory(directory, sizeof(directory)));
    CHECK(registry_test_join_path(registry_path, sizeof(registry_path),
                                  directory, "schemas.bin"));
    CHECK(registry_test_join_path(directory_path, sizeof(directory_path),
                                  directory, "not-a-file"));
    CHECK(registry_test_join_path(corrupt_path, sizeof(corrupt_path),
                                  directory, "corrupt.bin"));

    CHECK(typetree_schema_registry_export_file(
              &first_registry, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&loaded) == 1U);
    CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));

    /* Rewriting the same path replaces the complete canonical image. */
    CHECK(typetree_schema_registry_export_file(
              &second_registry, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&loaded) == 1U);
    CHECK(registry_has_schema(&loaded, &second, "2022.3.0f1"));
    CHECK(!registry_has_schema(&loaded, &first, "2021.3.35f1"));

#ifdef _WIN32
    char unicode_registry_path[REGISTRY_TEST_PATH_CAPACITY];
    CHECK(registry_test_join_path(
        unicode_registry_path, sizeof(unicode_registry_path), directory,
        "schemas_caf\xc3\xa9_\xe9\x9b\xaa.bin"));
    CHECK(typetree_schema_registry_export_file(
              &second_registry, unicode_registry_path) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, unicode_registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(registry_has_schema(&loaded, &second, "2022.3.0f1"));
    CHECK(registry_test_delete_utf8(unicode_registry_path));

    static const char invalid_utf8[] = {(char)0xc3, '(', '\0'};
    CHECK(typetree_schema_registry_export_file(
              &second_registry, invalid_utf8) ==
          TYPETREE_SCHEMA_INVALID_ARGUMENT);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, invalid_utf8) == TYPETREE_SCHEMA_INVALID_ARGUMENT);

    /* Denying delete sharing forces replacement to fail after temp writing. */
    CHECK(typetree_schema_registry_export_file(
              &first_registry, registry_path) == TYPETREE_SCHEMA_OK);
    HANDLE lock = CreateFileA(registry_path, GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(typetree_schema_registry_export_file(
              &second_registry, registry_path) ==
          TYPETREE_SCHEMA_IO_ERROR);
    CHECK(CloseHandle(lock) != 0);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));
    CHECK(CreateDirectoryA(directory_path, NULL) != 0);
#else
    /* A pre-commit I/O failure leaves the existing registry untouched. */
    CHECK(typetree_schema_registry_export_file(
              &first_registry, registry_path) == TYPETREE_SCHEMA_OK);
    if (geteuid() != 0) {
        CHECK(chmod(directory, 0500) == 0);
        TypeTreeSchemaStatus failed_write =
            typetree_schema_registry_export_file(
                &second_registry, registry_path);
        CHECK(chmod(directory, 0700) == 0);
        CHECK(failed_write == TYPETREE_SCHEMA_IO_ERROR);
        CHECK(typetree_schema_registry_import_file_replace(
                  &loaded, registry_path) == TYPETREE_SCHEMA_OK);
        CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));
    }

    char victim_path[REGISTRY_TEST_PATH_CAPACITY];
    char link_path[REGISTRY_TEST_PATH_CAPACITY];
    char fifo_path[REGISTRY_TEST_PATH_CAPACITY];
    CHECK(registry_test_join_path(victim_path, sizeof(victim_path),
                                  directory, "victim.bin"));
    CHECK(registry_test_join_path(link_path, sizeof(link_path),
                                  directory, "schemas.link"));
    CHECK(registry_test_join_path(fifo_path, sizeof(fifo_path),
                                  directory, "schemas.fifo"));
    CHECK(typetree_schema_registry_export_file(
              &first_registry, victim_path) == TYPETREE_SCHEMA_OK);
    CHECK(symlink(victim_path, link_path) == 0);
    CHECK(typetree_schema_registry_export_file(
              &second_registry, link_path) == TYPETREE_SCHEMA_IO_ERROR);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, link_path) == TYPETREE_SCHEMA_IO_ERROR);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, victim_path) == TYPETREE_SCHEMA_OK);
    CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));
    CHECK(mkdir(directory_path, 0700) == 0);
    CHECK(mkfifo(fifo_path, 0600) == 0);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, fifo_path) == TYPETREE_SCHEMA_IO_ERROR);
    CHECK(typetree_schema_registry_export_file(
              &second_registry, fifo_path) == TYPETREE_SCHEMA_IO_ERROR);
    CHECK(unlink(fifo_path) == 0);
    CHECK(unlink(link_path) == 0);
    CHECK(unlink(victim_path) == 0);
#endif

    /* Directories and other special objects are never accepted as files. */
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, directory_path) == TYPETREE_SCHEMA_IO_ERROR);
    CHECK(typetree_schema_registry_export_file(
              &second_registry, directory_path) ==
          TYPETREE_SCHEMA_IO_ERROR);
    CHECK(typetree_schema_registry_count(&loaded) == 1U);
    CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));

    /* A malformed regular file also cannot partially replace memory state. */
    static const uint8_t malformed[] = {'b', 'a', 'd'};
    FILE* corrupt_file = fopen(corrupt_path, "wb");
    CHECK(corrupt_file != NULL);
    CHECK(fwrite(malformed, 1U, sizeof(malformed), corrupt_file) ==
          sizeof(malformed));
    CHECK(fclose(corrupt_file) == 0);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, corrupt_path) == TYPETREE_SCHEMA_INVALID_FORMAT);
    CHECK(typetree_schema_registry_count(&loaded) == 1U);
    CHECK(registry_has_schema(&loaded, &first, "2021.3.35f1"));

#ifdef _WIN32
    CHECK(RemoveDirectoryA(directory_path) != 0);
    CHECK(DeleteFileA(corrupt_path) != 0);
    CHECK(DeleteFileA(registry_path) != 0);
    CHECK(RemoveDirectoryA(directory) != 0);
#else
    CHECK(rmdir(directory_path) == 0);
    CHECK(unlink(corrupt_path) == 0);
    CHECK(unlink(registry_path) == 0);
    CHECK(rmdir(directory) == 0);
#endif
    typetree_schema_registry_dispose(&loaded);
    typetree_schema_registry_dispose(&second_registry);
    typetree_schema_registry_dispose(&first_registry);
    typetree_free_type(&second);
    typetree_free_type(&first);
    return true;
}

#ifndef _WIN32
static bool verify_concurrent_atomic_writers(void) {
    TypeTreeType first;
    TypeTreeType second;
    initialize_schema(&first, 48, 1U);
    initialize_schema(&second, 49, 33U);
    TypeTreeSchemaRegistry first_registry;
    TypeTreeSchemaRegistry second_registry;
    TypeTreeSchemaRegistry loaded;
    typetree_schema_registry_init(&first_registry);
    typetree_schema_registry_init(&second_registry);
    typetree_schema_registry_init(&loaded);
    CHECK(learn_schema(&first_registry, &first, "2021.3.35f1"));
    CHECK(learn_schema(&second_registry, &second, "2022.3.0f1"));

    char directory[REGISTRY_TEST_PATH_CAPACITY];
    char registry_path[REGISTRY_TEST_PATH_CAPACITY];
    CHECK(registry_test_make_directory(directory, sizeof(directory)));
    CHECK(registry_test_join_path(registry_path, sizeof(registry_path),
                                  directory, "concurrent.bin"));
    CHECK(typetree_schema_registry_export_file(
              &first_registry, registry_path) == TYPETREE_SCHEMA_OK);

    pid_t first_writer = fork();
    CHECK(first_writer >= 0);
    if (first_writer == 0) {
        for (unsigned i = 0U; i < 16U; ++i) {
            if (typetree_schema_registry_export_file(
                    &first_registry, registry_path) != TYPETREE_SCHEMA_OK) {
                _exit(1);
            }
        }
        _exit(0);
    }
    pid_t second_writer = fork();
    CHECK(second_writer >= 0);
    if (second_writer == 0) {
        for (unsigned i = 0U; i < 16U; ++i) {
            if (typetree_schema_registry_export_file(
                    &second_registry, registry_path) !=
                TYPETREE_SCHEMA_OK) {
                _exit(1);
            }
        }
        _exit(0);
    }

    /* Every image observed while writers race must be one complete file. */
    for (unsigned i = 0U; i < 64U; ++i) {
        TypeTreeSchemaStatus import_status =
            typetree_schema_registry_import_file_replace(
                &loaded, registry_path);
        if (import_status != TYPETREE_SCHEMA_OK) {
            fprintf(stderr, "concurrent import failed: %s\n",
                    typetree_schema_status_name(import_status));
        }
        CHECK(import_status == TYPETREE_SCHEMA_OK);
        CHECK(registry_is_exactly_one_of(&loaded, &first, &second));
    }
    int first_status = 0;
    int second_status = 0;
    CHECK(waitpid(first_writer, &first_status, 0) == first_writer);
    CHECK(waitpid(second_writer, &second_status, 0) == second_writer);
    CHECK(WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0);
    CHECK(WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0);
    CHECK(typetree_schema_registry_import_file_replace(
              &loaded, registry_path) == TYPETREE_SCHEMA_OK);
    CHECK(registry_is_exactly_one_of(&loaded, &first, &second));

    CHECK(unlink(registry_path) == 0);
    CHECK(rmdir(directory) == 0);
    typetree_schema_registry_dispose(&loaded);
    typetree_schema_registry_dispose(&second_registry);
    typetree_schema_registry_dispose(&first_registry);
    typetree_free_type(&second);
    typetree_free_type(&first);
    return true;
}
#endif

static bool verify_serialized_file_registry_integration(void) {
    size_t initial_allocations = g_allocations_count;
    size_t initial_bytes = g_allocated_bytes;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);

    SerializedFileFixture enabled;
    CHECK(build_serialized_file_fixture(&enabled, true, 0x4000U));
    SerializedFile file;
    /* Embedded TypeTrees are usable locally but cannot become registry
     * authority merely because this is the first enabled asset observed. */
    CHECK(serialized_file_open_with_schema_registry(
        &file, enabled.bytes, enabled.size, &registry));
    CHECK(file.type_tree_enabled);
    CHECK(file.type_count == 1 && file.types[0].node_count == 1);
    serialized_file_close(&file);
    CHECK(typetree_schema_registry_count(&registry) == 0U);

    SerializedFileFixture disabled;
    CHECK(build_serialized_file_fixture(&disabled, false, 0U));
    CHECK(serialized_file_open_metadata(
        &file, disabled.bytes, disabled.size));
    CHECK(!file.type_tree_enabled);
    CHECK(file.types[0].node_count == 0);
    CHECK(serialized_file_resolve_class_schema(
              &file, 48, NULL) == TYPETREE_SCHEMA_INVALID_ARGUMENT);
    CHECK(file.types[0].node_count == 0);
    CHECK(serialized_file_resolve_class_schema(
              &file, 48, &registry) == TYPETREE_SCHEMA_NOT_FOUND);
    CHECK(file.types[0].node_count == 0);
    serialized_file_close(&file);
    CHECK(!serialized_file_open(&file, disabled.bytes, disabled.size));
    CHECK(!serialized_file_open_with_schema_registry(
        &file, disabled.bytes, disabled.size, &registry));

    CHECK(serialized_file_open_with_schema_registry_ex(
        &file, enabled.bytes, enabled.size, &registry,
        TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT));
    serialized_file_close(&file);
    CHECK(typetree_schema_registry_count(&registry) == 1U);

    CHECK(serialized_file_open_metadata(
        &file, disabled.bytes, disabled.size));
    CHECK(serialized_file_resolve_class_schema(
              &file, 48, &registry) == TYPETREE_SCHEMA_OK);
    CHECK(file.types[0].node_count == 1);
    CHECK(file.types[0].nodes[0].meta_flags == 0x4000U);
    CHECK(strcmp(file.types[0].nodes[0].type_str, "int") == 0);
    serialized_file_close(&file);

    CHECK(serialized_file_open_with_schema_registry(
        &file, disabled.bytes, disabled.size, &registry));
    CHECK(!file.type_tree_enabled);
    CHECK(file.types[0].node_count == 1);
    CHECK(file.types[0].nodes[0].meta_flags == 0x4000U);
    CHECK(strcmp(file.types[0].nodes[0].type_str, "int") == 0);
    serialized_file_close(&file);

    disabled.bytes[disabled.type_hash_offset] ^= 1U;
    CHECK(!serialized_file_open_with_schema_registry(
        &file, disabled.bytes, disabled.size, &registry));
    disabled.bytes[disabled.type_hash_offset] ^= 1U;

    /* A conflicting enabled schema cannot partially replace the registry. */
    SerializedFileFixture conflict;
    CHECK(build_serialized_file_fixture(&conflict, true, 0U));
    CHECK(serialized_file_open_with_schema_registry(
        &file, conflict.bytes, conflict.size, &registry));
    serialized_file_close(&file);
    CHECK(!serialized_file_open_with_schema_registry_ex(
        &file, conflict.bytes, conflict.size, &registry,
        TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT));
    CHECK(typetree_schema_registry_count(&registry) == 1U);
    CHECK(serialized_file_open_with_schema_registry(
        &file, disabled.bytes, disabled.size, &registry));
    CHECK(file.types[0].nodes[0].meta_flags == 0x4000U);
    serialized_file_close(&file);

    CHECK(!serialized_file_open_with_schema_registry_ex(&file,
        enabled.bytes,
        enabled.size,
        &registry,
        TYPETREE_SCHEMA_PROVENANCE_IMPORTED_REGISTRY));

    typetree_schema_registry_dispose(&registry);
    CHECK(g_allocations_count == initial_allocations);
    CHECK(g_allocated_bytes == initial_bytes);
    return true;
}

static bool verify_exact_script_hash_rows(
    TypeTreeSchemaRegistry* registry, TypeTreeType* source, size_t* expected_entries) {
    static const struct {
        int32_t class_id;
        uint16_t script_index;
        bool has_hash;
    } cases[] = {{49, 0, true},
        {49, UINT16_C(0x7fff), true},
        {49, UINT16_C(0x8000), false},
        {49, UINT16_C(0xfffe), false},
        {49, UINT16_MAX, false},
        {114, UINT16_C(0x8000), true},
        {114, UINT16_C(0xfffe), true},
        {114, UINT16_MAX, true}};

    static const char* const versions[] = {"2021.3.35f1", "2021.3.29f1"};
    for (size_t version = 0U; version < sizeof(versions) / sizeof(versions[0]); ++version) {
        for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            source->type_id = cases[index].class_id;
            source->script_type_index = cases[index].script_index;
            const size_t variants = cases[index].has_hash ? 2U : 1U;
            for (size_t variant = 0U; variant < variants; ++variant) {
                memset(source->script_id_hash,
                    cases[index].has_hash ? (int)(0x31U + variant) : 0,
                    sizeof(source->script_id_hash));
                TypeTreeSchemaKey key;
                CHECK(make_key(&key, source, versions[version]) == TYPETREE_SCHEMA_OK);
                CHECK(key.has_script_id_hash == cases[index].has_hash);
                CHECK(memcmp(key.script_id_hash,
                          source->script_id_hash,
                          sizeof(key.script_id_hash)) == 0);
                CHECK(typetree_schema_registry_learn(
                          registry, &key, source, TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
                    TYPETREE_SCHEMA_OK);
                ++*expected_entries;
                CHECK(typetree_schema_registry_count(registry) == *expected_entries);
                TypeTreeType found;
                CHECK(
                    typetree_schema_registry_lookup(registry, &key, &found) == TYPETREE_SCHEMA_OK);
                const bool exact_hash = memcmp(found.script_id_hash,
                                            source->script_id_hash,
                                            sizeof(found.script_id_hash)) == 0;
                typetree_free_type(&found);
                CHECK(exact_hash);
            }
        }
    }

    return true;
}

static char* copy_schema_fixture_name(const char* name) {
    const size_t size = strlen(name) + 1U;
    char* copy = mem_alloc(size);
    if (copy) {
        memcpy(copy, name, size);
    }
    return copy;
}

static bool verify_exact_script_hash_identity(void) {
    static const char* const versions[] = {"2021.3.35f1", "2021.3.29f1"};
    const size_t initial_allocations = g_allocations_count;
    const size_t initial_bytes = g_allocated_bytes;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    TypeTreeType source;
    initialize_schema(&source, 49, 0x67U);
    size_t expected_entries = 0U;
    CHECK(verify_exact_script_hash_rows(&registry, &source, &expected_entries));
    CHECK(expected_entries == 26U);

    /* Reference rows use the same signed script-index predicate, but their
     * schema names replace the ordinary dependency vector. */
    mem_free(source.dependencies, sizeof(*source.dependencies));
    source.dependencies = NULL;
    source.dependency_count = 0;
    source.ref_class_name = copy_schema_fixture_name("Payload");
    source.ref_namespace = copy_schema_fixture_name("TailFixture");
    source.ref_asm_name = copy_schema_fixture_name("Fixture.Assembly");
    if (!source.ref_class_name || !source.ref_namespace || !source.ref_asm_name) {
        typetree_free_type(&source);
        typetree_schema_registry_dispose(&registry);
        CHECK(false);
    }
    source.is_ref_type = true;
    CHECK(verify_exact_script_hash_rows(&registry, &source, &expected_entries));
    CHECK(expected_entries == 52U);

    source.type_id = -1;
    source.script_type_index = UINT16_MAX;
    TypeTreeSchemaKey key;
    for (size_t version = 0U; version < sizeof(versions) / sizeof(versions[0]); ++version) {
        CHECK(make_key(&key, &source, versions[version]) == TYPETREE_SCHEMA_INVALID_SCHEMA);
    }
    /* Adjacent versions retain their existing registry policy. */
    source.type_id = 49;
    source.script_type_index = UINT16_C(0x8000);
    source.is_ref_type = true;
    CHECK(make_key(&key, &source, versions[0]) == TYPETREE_SCHEMA_OK && !key.has_script_id_hash);
    CHECK(make_key(&key, &source, "2021.3.34f1") == TYPETREE_SCHEMA_OK && key.has_script_id_hash);
    source.is_ref_type = false;
    source.script_type_index = 0U;
    CHECK(make_key(&key, &source, "2021.3.34f1") == TYPETREE_SCHEMA_OK && !key.has_script_id_hash);
    CHECK(typetree_schema_key_from_type(&key, 21U, versions[0], source.type_id, &source) ==
            TYPETREE_SCHEMA_OK &&
        !key.has_script_id_hash);

    uint8_t* encoded = NULL;
    size_t encoded_size = 0U;
    CHECK(typetree_schema_registry_serialize(&registry, &encoded, &encoded_size) ==
        TYPETREE_SCHEMA_OK);
    TypeTreeSchemaRegistry restored;
    typetree_schema_registry_init(&restored);
    CHECK(typetree_schema_registry_deserialize_replace(&restored, encoded, encoded_size) ==
        TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&restored) == expected_entries);
    uint8_t* repeated = NULL;
    size_t repeated_size = 0U;
    CHECK(typetree_schema_registry_serialize(&restored, &repeated, &repeated_size) ==
        TYPETREE_SCHEMA_OK);
    CHECK(repeated_size == encoded_size && memcmp(repeated, encoded, encoded_size) == 0);
    mem_free(repeated, repeated_size);
    mem_free(encoded, encoded_size);
    typetree_schema_registry_dispose(&restored);
    typetree_schema_registry_dispose(&registry);
    typetree_free_type(&source);
    CHECK(g_allocations_count == initial_allocations && g_allocated_bytes == initial_bytes);
    return true;
}

int main(void) {
    if (!verify_deep_copy_and_exact_key() || !verify_schema_grammar_rejection() ||
        !verify_long_schema_names_roundtrip() || !verify_canonical_roundtrip_and_corruption() ||
        !verify_atomic_file_rewrite_and_restrictions() ||
#ifndef _WIN32
        !verify_concurrent_atomic_writers() ||
#endif
        !verify_serialized_file_registry_integration() || !verify_exact_script_hash_identity()) {
        return 1;
    }
    if (g_allocations_count != 0U || g_allocated_bytes != 0U) {
        fprintf(
            stderr, "leak: %zu allocations, %zu bytes\n", g_allocations_count, g_allocated_bytes);
        return 1;
    }
    puts("UnityCommon TypeTree schema registry tests passed.");
    return 0;
}
