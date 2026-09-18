// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif

#include "io/typetree_schema_registry.h"

#include "serialized_type_shape_internal.h"

#include "common/sha256.h"
#include "io/typetree_schema_profile.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define SCHEMA_REGISTRY_HEADER_SIZE 64U
#define SCHEMA_REGISTRY_MAX_FILE_SIZE (UINT64_C(512) * 1024U * 1024U)
#define SCHEMA_REGISTRY_MAX_ENTRIES 65536U
#define SCHEMA_REGISTRY_MAX_NODES 1048576U
#define SCHEMA_REGISTRY_MAX_STRING_BUFFER (UINT32_C(256) * 1024U * 1024U)
#define SCHEMA_REGISTRY_MAX_DEPENDENCIES 1048576U
#define SCHEMA_REGISTRY_MAX_REF_STRING_SIZE 1048576U

#define SCHEMA_KEY_FLAG_SCRIPT_HASH UINT16_C(1)
#define SCHEMA_KEY_FLAG_REF_TYPE UINT16_C(2)
#define SCHEMA_KEY_FLAG_STRIPPED UINT16_C(4)
#define SCHEMA_KEY_KNOWN_FLAGS \
    (SCHEMA_KEY_FLAG_SCRIPT_HASH | SCHEMA_KEY_FLAG_REF_TYPE | \
     SCHEMA_KEY_FLAG_STRIPPED)
#define SCHEMA_VALUE_FLAG_STRIPPED UINT32_C(1)
#define SCHEMA_VALUE_KNOWN_FLAGS SCHEMA_VALUE_FLAG_STRIPPED

static const uint8_t k_schema_registry_magic[8] = {
    'D', 'X', 'T', 'T', 'S', 'R', '0', '1'
};

/*
 * File header (64 bytes, all integers little endian):
 *   magic[8], version:u16, header_size:u16, flags:u32, entry_count:u32,
 *   reserved:u32, total_size:u64, payload_sha256[32].
 * Each length-delimited record stores the complete key, schema flags/counts,
 * 32-byte Unity TypeTree node records, the local string table, dependencies,
 * and reference-type strings.  Strings are length-prefixed opaque bytes;
 * embedded NULs and implicit terminators are forbidden in the encoding.
 */

typedef struct {
    uint32_t serialized_file_version;
    char* unity_version;
    size_t unity_version_size;
    int32_t class_id;
    int32_t serialized_type_id;
    uint16_t script_type_index;
    bool has_script_id_hash;
    bool is_stripped;
    bool is_ref_type;
    uint8_t script_id_hash[16];
    uint8_t type_hash[16];
} OwnedSchemaKey;

struct TypeTreeSchemaRegistryEntry {
    OwnedSchemaKey key;
    TypeTreeType schema;
};

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
    TypeTreeSchemaStatus status;
} SchemaWriter;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t position;
} SchemaReader;

static bool checked_add_size(size_t left, size_t right, size_t* out) {
    if (left > SIZE_MAX - right) return false;
    *out = left + right;
    return true;
}

static bool checked_multiply_size(size_t left, size_t right, size_t* out) {
    if (left != 0U && right > SIZE_MAX / left) return false;
    *out = left * right;
    return true;
}

static bool bounded_string_size(const char* value, size_t limit,
                                size_t* out_size) {
    if (!value || !out_size) return false;
    const char* end = (const char*)memchr(value, '\0', limit + 1U);
    if (!end) return false;
    *out_size = (size_t)(end - value);
    return true;
}

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    if (!bytes && size != 0U) return false;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0U) return false;
    }
    return true;
}

static uint16_t load_le16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static uint32_t load_le32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t load_le64(const uint8_t* bytes) {
    uint64_t value = 0U;
    for (unsigned i = 0; i < 8U; ++i) {
        value |= (uint64_t)bytes[i] << (i * 8U);
    }
    return value;
}

static void store_le16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void store_le32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void store_le64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void schema_writer_dispose(SchemaWriter* writer) {
    if (!writer) return;
    mem_free(writer->data, writer->capacity);
    memset(writer, 0, sizeof(*writer));
}

static bool schema_writer_reserve(SchemaWriter* writer, size_t extra) {
    size_t required;
    if (!writer || writer->status != TYPETREE_SCHEMA_OK ||
        !checked_add_size(writer->size, extra, &required)) {
        if (writer) writer->status = TYPETREE_SCHEMA_SIZE_OVERFLOW;
        return false;
    }
    if ((uint64_t)required > SCHEMA_REGISTRY_MAX_FILE_SIZE) {
        writer->status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        return false;
    }
    if (required <= writer->capacity) return true;

    size_t capacity = writer->capacity == 0U ? 256U : writer->capacity;
    while (capacity < required) {
        if (capacity > (size_t)SCHEMA_REGISTRY_MAX_FILE_SIZE / 2U) {
            capacity = (size_t)SCHEMA_REGISTRY_MAX_FILE_SIZE;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < required) {
        writer->status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        return false;
    }
    uint8_t* replacement = (uint8_t*)mem_realloc(
        writer->data, writer->capacity, capacity);
    if (!replacement) {
        writer->status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        return false;
    }
    writer->data = replacement;
    writer->capacity = capacity;
    return true;
}

static bool schema_writer_bytes(SchemaWriter* writer, const void* data,
                                size_t size) {
    if ((!data && size != 0U) || !schema_writer_reserve(writer, size)) {
        if (writer && writer->status == TYPETREE_SCHEMA_OK) {
            writer->status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
        }
        return false;
    }
    if (size != 0U) memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return true;
}

static bool schema_writer_zeros(SchemaWriter* writer, size_t size) {
    if (!schema_writer_reserve(writer, size)) return false;
    memset(writer->data + writer->size, 0, size);
    writer->size += size;
    return true;
}

static bool schema_writer_u8(SchemaWriter* writer, uint8_t value) {
    return schema_writer_bytes(writer, &value, sizeof(value));
}

static bool schema_writer_u16(SchemaWriter* writer, uint16_t value) {
    uint8_t encoded[2];
    store_le16(encoded, value);
    return schema_writer_bytes(writer, encoded, sizeof(encoded));
}

static bool schema_writer_u32(SchemaWriter* writer, uint32_t value) {
    uint8_t encoded[4];
    store_le32(encoded, value);
    return schema_writer_bytes(writer, encoded, sizeof(encoded));
}

static bool schema_writer_u64(SchemaWriter* writer, uint64_t value) {
    uint8_t encoded[8];
    store_le64(encoded, value);
    return schema_writer_bytes(writer, encoded, sizeof(encoded));
}

static bool schema_reader_bytes(SchemaReader* reader, void* destination,
                                size_t size) {
    if (!reader || (!destination && size != 0U) ||
        reader->position > reader->size ||
        size > reader->size - reader->position) {
        return false;
    }
    if (size != 0U) memcpy(destination, reader->data + reader->position, size);
    reader->position += size;
    return true;
}

static bool schema_reader_view(SchemaReader* reader, size_t size,
                               const uint8_t** out_view) {
    if (!reader || !out_view || reader->position > reader->size ||
        size > reader->size - reader->position) {
        return false;
    }
    *out_view = reader->data + reader->position;
    reader->position += size;
    return true;
}

static bool schema_reader_u8(SchemaReader* reader, uint8_t* value) {
    return schema_reader_bytes(reader, value, sizeof(*value));
}

static bool schema_reader_u16(SchemaReader* reader, uint16_t* value) {
    uint8_t encoded[2];
    if (!schema_reader_bytes(reader, encoded, sizeof(encoded))) return false;
    *value = load_le16(encoded);
    return true;
}

static bool schema_reader_u32(SchemaReader* reader, uint32_t* value) {
    uint8_t encoded[4];
    if (!schema_reader_bytes(reader, encoded, sizeof(encoded))) return false;
    *value = load_le32(encoded);
    return true;
}

static bool schema_reader_u64(SchemaReader* reader, uint64_t* value) {
    uint8_t encoded[8];
    if (!schema_reader_bytes(reader, encoded, sizeof(encoded))) return false;
    *value = load_le64(encoded);
    return true;
}

static TypeTreeSchemaKey schema_key_view(const OwnedSchemaKey* key) {
    TypeTreeSchemaKey view;
    memset(&view, 0, sizeof(view));
    if (!key) return view;
    view.serialized_file_version = key->serialized_file_version;
    view.unity_version = key->unity_version;
    view.unity_version_size = key->unity_version_size;
    view.class_id = key->class_id;
    view.serialized_type_id = key->serialized_type_id;
    view.script_type_index = key->script_type_index;
    view.has_script_id_hash = key->has_script_id_hash;
    view.is_stripped = key->is_stripped;
    view.is_ref_type = key->is_ref_type;
    memcpy(view.script_id_hash, key->script_id_hash,
           sizeof(view.script_id_hash));
    memcpy(view.type_hash, key->type_hash, sizeof(view.type_hash));
    return view;
}

static TypeTreeSchemaStatus validate_key(const TypeTreeSchemaKey* key) {
    if (!key || !key->unity_version || key->unity_version_size == 0U) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    if (key->unity_version_size >
        TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE) {
        return TYPETREE_SCHEMA_LIMIT_EXCEEDED;
    }
    if (memchr(key->unity_version, '\0', key->unity_version_size) != NULL) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    if (!key->has_script_id_hash &&
        !bytes_are_zero(key->script_id_hash, sizeof(key->script_id_hash))) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    return TYPETREE_SCHEMA_OK;
}

static int compare_byte_strings(const char* left, size_t left_size,
                                const char* right, size_t right_size) {
    size_t common = left_size < right_size ? left_size : right_size;
    int comparison = common == 0U ? 0 : memcmp(left, right, common);
    if (comparison != 0) return comparison;
    if (left_size < right_size) return -1;
    if (left_size > right_size) return 1;
    return 0;
}

static int compare_keys(const TypeTreeSchemaKey* left,
                        const TypeTreeSchemaKey* right) {
#define COMPARE_SCALAR(field) do { \
    if (left->field < right->field) return -1; \
    if (left->field > right->field) return 1; \
} while (0)
    COMPARE_SCALAR(serialized_file_version);
    int comparison = compare_byte_strings(
        left->unity_version, left->unity_version_size,
        right->unity_version, right->unity_version_size);
    if (comparison != 0) return comparison;
    COMPARE_SCALAR(class_id);
    COMPARE_SCALAR(serialized_type_id);
    COMPARE_SCALAR(script_type_index);
    COMPARE_SCALAR(has_script_id_hash);
    COMPARE_SCALAR(is_stripped);
    COMPARE_SCALAR(is_ref_type);
    comparison = memcmp(left->script_id_hash, right->script_id_hash,
                        sizeof(left->script_id_hash));
    if (comparison != 0) return comparison;
    return memcmp(left->type_hash, right->type_hash,
                  sizeof(left->type_hash));
#undef COMPARE_SCALAR
}

static void owned_key_dispose(OwnedSchemaKey* key) {
    if (!key) return;
    mem_free(key->unity_version, key->unity_version_size + 1U);
    memset(key, 0, sizeof(*key));
}

static TypeTreeSchemaStatus owned_key_copy(OwnedSchemaKey* destination,
                                           const TypeTreeSchemaKey* source) {
    TypeTreeSchemaStatus status = validate_key(source);
    if (!destination || status != TYPETREE_SCHEMA_OK) {
        return destination ? status : TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    memset(destination, 0, sizeof(*destination));
    destination->unity_version = (char*)mem_alloc(
        source->unity_version_size + 1U);
    if (!destination->unity_version) {
        return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    }
    memcpy(destination->unity_version, source->unity_version,
           source->unity_version_size);
    destination->unity_version[source->unity_version_size] = '\0';
    destination->serialized_file_version = source->serialized_file_version;
    destination->unity_version_size = source->unity_version_size;
    destination->class_id = source->class_id;
    destination->serialized_type_id = source->serialized_type_id;
    destination->script_type_index = source->script_type_index;
    destination->has_script_id_hash = source->has_script_id_hash;
    destination->is_stripped = source->is_stripped;
    destination->is_ref_type = source->is_ref_type;
    memcpy(destination->script_id_hash, source->script_id_hash,
           sizeof(destination->script_id_hash));
    memcpy(destination->type_hash, source->type_hash,
           sizeof(destination->type_hash));
    return TYPETREE_SCHEMA_OK;
}

static TypeTreeSchemaStatus validate_schema(const TypeTreeSchemaKey* key,
                                            const TypeTreeType* schema) {
    TypeTreeSchemaStatus status = validate_key(key);
    if (status != TYPETREE_SCHEMA_OK) return status;
    if (!schema || schema->type_id != key->serialized_type_id ||
        schema->script_type_index != key->script_type_index ||
        schema->is_stripped != key->is_stripped ||
        schema->is_ref_type != key->is_ref_type ||
        memcmp(schema->type_hash, key->type_hash,
               sizeof(schema->type_hash)) != 0 ||
        memcmp(schema->script_id_hash, key->script_id_hash,
               sizeof(schema->script_id_hash)) != 0) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    if (schema->node_count <= 0 || !schema->nodes ||
        (uint32_t)schema->node_count > SCHEMA_REGISTRY_MAX_NODES ||
        schema->string_buffer_size > SCHEMA_REGISTRY_MAX_STRING_BUFFER ||
        (schema->string_buffer_size != 0U) !=
            (schema->string_buffer != NULL) ||
        schema->dependency_count < 0 ||
        (uint32_t)schema->dependency_count >
            SCHEMA_REGISTRY_MAX_DEPENDENCIES ||
        (schema->dependency_count != 0) !=
            (schema->dependencies != NULL)) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    if ((key->serialized_file_version < 21U || schema->is_ref_type) &&
        schema->dependency_count != 0) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    if (schema->is_ref_type) {
        size_t ignored;
        if (!bounded_string_size(schema->ref_class_name,
                                 SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                                 &ignored) ||
            !bounded_string_size(schema->ref_namespace,
                                 SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                                 &ignored) ||
            !bounded_string_size(schema->ref_asm_name,
                                 SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                                 &ignored)) {
            return TYPETREE_SCHEMA_INVALID_SCHEMA;
        }
    } else if (schema->ref_class_name || schema->ref_namespace ||
               schema->ref_asm_name) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }

    if (!typetree_validate_schema(schema)) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }

    for (int i = 0; i < schema->node_count; ++i) {
        const TypeTreeNode* node = &schema->nodes[i];
        const char* resolved_type = typetree_resolve_string(
            schema->string_buffer, schema->string_buffer_size,
            node->type_str_offset);
        const char* resolved_name = typetree_resolve_string(
            schema->string_buffer, schema->string_buffer_size,
            node->name_str_offset);
        if (!resolved_type || !resolved_name ||
            strcmp(resolved_type, node->type_str) != 0 ||
            strcmp(resolved_name, node->name_str) != 0) {
            return TYPETREE_SCHEMA_INVALID_SCHEMA;
        }
    }
    if (typetree_schema_validate_known_profile(
            key->unity_version, key->unity_version_size, key->class_id,
            key->type_hash, schema) == TYPETREE_SCHEMA_PROFILE_INVALID) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    return TYPETREE_SCHEMA_OK;
}

static char* clone_string(const char* source, size_t limit,
                          TypeTreeSchemaStatus* status) {
    size_t size;
    if (!bounded_string_size(source, limit, &size)) {
        *status = TYPETREE_SCHEMA_INVALID_SCHEMA;
        return NULL;
    }
    char* clone = (char*)mem_alloc(size + 1U);
    if (!clone) {
        *status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        return NULL;
    }
    memcpy(clone, source, size + 1U);
    return clone;
}

static TypeTreeSchemaStatus clone_schema(TypeTreeType* destination,
                                         const TypeTreeType* source) {
    if (!destination || !source) return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    memset(destination, 0, sizeof(*destination));
    destination->type_id = source->type_id;
    destination->is_stripped = source->is_stripped;
    destination->script_type_index = source->script_type_index;
    memcpy(destination->script_id_hash, source->script_id_hash,
           sizeof(destination->script_id_hash));
    memcpy(destination->type_hash, source->type_hash,
           sizeof(destination->type_hash));
    destination->node_count = source->node_count;
    destination->string_buffer_size = source->string_buffer_size;
    destination->dependency_count = source->dependency_count;
    destination->is_ref_type = source->is_ref_type;

    size_t allocation_size;
    if (!checked_multiply_size((size_t)source->node_count,
                               sizeof(TypeTreeNode), &allocation_size)) {
        return TYPETREE_SCHEMA_SIZE_OVERFLOW;
    }
    destination->nodes = (TypeTreeNode*)mem_alloc(allocation_size);
    if (!destination->nodes) goto allocation_failed;
    memcpy(destination->nodes, source->nodes, allocation_size);

    if (source->string_buffer_size != 0U) {
        destination->string_buffer = (uint8_t*)mem_alloc(
            source->string_buffer_size);
        if (!destination->string_buffer) goto allocation_failed;
        memcpy(destination->string_buffer, source->string_buffer,
               source->string_buffer_size);
    }
    if (!typetree_bind_node_strings(destination)) {
        typetree_free_type(destination);
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    if (source->dependency_count != 0) {
        if (!checked_multiply_size((size_t)source->dependency_count,
                                   sizeof(int32_t), &allocation_size)) {
            typetree_free_type(destination);
            return TYPETREE_SCHEMA_SIZE_OVERFLOW;
        }
        destination->dependencies = (int32_t*)mem_alloc(allocation_size);
        if (!destination->dependencies) goto allocation_failed;
        memcpy(destination->dependencies, source->dependencies,
               allocation_size);
    }
    if (source->is_ref_type) {
        TypeTreeSchemaStatus status = TYPETREE_SCHEMA_OK;
        destination->ref_class_name = clone_string(
            source->ref_class_name, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!destination->ref_class_name) goto string_failed;
        destination->ref_namespace = clone_string(
            source->ref_namespace, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!destination->ref_namespace) goto string_failed;
        destination->ref_asm_name = clone_string(
            source->ref_asm_name, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!destination->ref_asm_name) goto string_failed;
        return TYPETREE_SCHEMA_OK;
string_failed:
        typetree_free_type(destination);
        return status;
    }
    return TYPETREE_SCHEMA_OK;

allocation_failed:
    typetree_free_type(destination);
    return TYPETREE_SCHEMA_ALLOCATION_FAILED;
}

static bool nodes_equal(const TypeTreeNode* left,
                        const TypeTreeNode* right) {
    return left->version == right->version &&
           left->level == right->level &&
           left->type_flags == right->type_flags &&
           left->type_str_offset == right->type_str_offset &&
           left->name_str_offset == right->name_str_offset &&
           left->byte_size == right->byte_size &&
           left->index == right->index &&
           left->meta_flags == right->meta_flags &&
           left->ref_type_hash == right->ref_type_hash &&
           strcmp(left->type_str, right->type_str) == 0 &&
           strcmp(left->name_str, right->name_str) == 0;
}

static bool schemas_equal(const TypeTreeType* left,
                          const TypeTreeType* right) {
    if (left->type_id != right->type_id ||
        left->is_stripped != right->is_stripped ||
        left->script_type_index != right->script_type_index ||
        memcmp(left->script_id_hash, right->script_id_hash,
               sizeof(left->script_id_hash)) != 0 ||
        memcmp(left->type_hash, right->type_hash,
               sizeof(left->type_hash)) != 0 ||
        left->node_count != right->node_count ||
        left->string_buffer_size != right->string_buffer_size ||
        left->dependency_count != right->dependency_count ||
        left->is_ref_type != right->is_ref_type) {
        return false;
    }
    for (int i = 0; i < left->node_count; ++i) {
        if (!nodes_equal(&left->nodes[i], &right->nodes[i])) return false;
    }
    if (left->string_buffer_size != 0U &&
        memcmp(left->string_buffer, right->string_buffer,
               left->string_buffer_size) != 0) {
        return false;
    }
    if (left->dependency_count != 0 &&
        memcmp(left->dependencies, right->dependencies,
               (size_t)left->dependency_count * sizeof(int32_t)) != 0) {
        return false;
    }
    if (left->is_ref_type) {
        return strcmp(left->ref_class_name, right->ref_class_name) == 0 &&
               strcmp(left->ref_namespace, right->ref_namespace) == 0 &&
               strcmp(left->ref_asm_name, right->ref_asm_name) == 0;
    }
    return true;
}

static void registry_entry_dispose(TypeTreeSchemaRegistryEntry* entry) {
    if (!entry) return;
    owned_key_dispose(&entry->key);
    typetree_free_type(&entry->schema);
    memset(entry, 0, sizeof(*entry));
}

void typetree_schema_registry_init(TypeTreeSchemaRegistry* registry) {
    if (registry) memset(registry, 0, sizeof(*registry));
}

void typetree_schema_registry_dispose(TypeTreeSchemaRegistry* registry) {
    if (!registry) return;
    for (size_t i = 0; i < registry->count; ++i) {
        registry_entry_dispose(&registry->entries[i]);
    }
    mem_free(registry->entries,
             registry->capacity * sizeof(*registry->entries));
    memset(registry, 0, sizeof(*registry));
}

size_t typetree_schema_registry_count(const TypeTreeSchemaRegistry* registry) {
    return registry ? registry->count : 0U;
}

TypeTreeSchemaStatus typetree_schema_registry_entry_view(
    const TypeTreeSchemaRegistry* registry, size_t canonical_index,
    TypeTreeSchemaKey* out_key, const TypeTreeType** out_schema) {
    if (!registry || !out_key || !out_schema ||
        registry->count > registry->capacity ||
        (registry->count != 0U && !registry->entries)) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    memset(out_key, 0, sizeof(*out_key));
    *out_schema = NULL;
    if (canonical_index >= registry->count) {
        return TYPETREE_SCHEMA_NOT_FOUND;
    }

    const TypeTreeSchemaRegistryEntry* selected =
        &registry->entries[canonical_index];
    *out_key = schema_key_view(&selected->key);
    *out_schema = &selected->schema;
    return TYPETREE_SCHEMA_OK;
}

TypeTreeSchemaStatus typetree_schema_key_from_type(
    TypeTreeSchemaKey* out_key,
    uint32_t serialized_file_version,
    const char* unity_version,
    int32_t class_id,
    const TypeTreeType* type) {
    if (!out_key || !unity_version || !type) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    size_t unity_version_size;
    if (!bounded_string_size(
            unity_version,
            TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE,
            &unity_version_size) || unity_version_size == 0U) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    memset(out_key, 0, sizeof(*out_key));
    out_key->serialized_file_version = serialized_file_version;
    out_key->unity_version = unity_version;
    out_key->unity_version_size = unity_version_size;
    out_key->class_id = class_id;
    out_key->serialized_type_id = type->type_id;
    out_key->script_type_index = type->script_type_index;
    out_key->is_stripped = type->is_stripped;
    out_key->is_ref_type = type->is_ref_type;
    const bool exact_physical_type = serialized_file_version == 22U && unity_version_size == 11U &&
        (memcmp(unity_version, "2021.3.35f1", 11U) == 0 ||
            memcmp(unity_version, "2021.3.29f1", 11U) == 0);
    if (exact_physical_type) {
        if (!serialized_type_v22_hash_shape(
                (uint32_t)type->type_id, type->script_type_index, &out_key->has_script_id_hash)) {
            return TYPETREE_SCHEMA_INVALID_SCHEMA;
        }
    } else {
        /* Other physical/engine versions retain their separate legacy policy. */
        out_key->has_script_id_hash = (serialized_file_version < 17U && type->type_id < 0) ||
            (serialized_file_version >= 17U && type->type_id == 114) ||
            (type->is_ref_type && type->script_type_index != UINT16_MAX);
    }
    if (out_key->has_script_id_hash) {
        memcpy(out_key->script_id_hash, type->script_id_hash, sizeof(out_key->script_id_hash));
    }
    memcpy(out_key->type_hash, type->type_hash,
           sizeof(out_key->type_hash));
    return validate_key(out_key);
}

static TypeTreeSchemaStatus registry_reserve(
    TypeTreeSchemaRegistry* registry, size_t required) {
    if (required > SCHEMA_REGISTRY_MAX_ENTRIES) {
        return TYPETREE_SCHEMA_LIMIT_EXCEEDED;
    }
    if (required <= registry->capacity) return TYPETREE_SCHEMA_OK;
    size_t capacity = registry->capacity == 0U ? 8U : registry->capacity;
    while (capacity < required) {
        if (capacity > SCHEMA_REGISTRY_MAX_ENTRIES / 2U) {
            capacity = SCHEMA_REGISTRY_MAX_ENTRIES;
            break;
        }
        capacity *= 2U;
    }
    size_t allocation_size;
    if (!checked_multiply_size(capacity, sizeof(*registry->entries),
                               &allocation_size)) {
        return TYPETREE_SCHEMA_SIZE_OVERFLOW;
    }
    size_t old_size = registry->capacity * sizeof(*registry->entries);
    TypeTreeSchemaRegistryEntry* replacement =
        (TypeTreeSchemaRegistryEntry*)mem_realloc(
            registry->entries, old_size, allocation_size);
    if (!replacement) return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    memset((uint8_t*)replacement + old_size, 0,
           allocation_size - old_size);
    registry->entries = replacement;
    registry->capacity = capacity;
    return TYPETREE_SCHEMA_OK;
}

TypeTreeSchemaStatus typetree_schema_registry_learn(
    TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* key,
    const TypeTreeType* schema,
    TypeTreeSchemaProvenance provenance) {
    if (!registry ||
        (provenance != TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT &&
         provenance != TYPETREE_SCHEMA_PROVENANCE_IMPORTED_REGISTRY)) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    TypeTreeSchemaStatus status = validate_schema(key, schema);
    if (status != TYPETREE_SCHEMA_OK) return status;

    for (size_t i = 0; i < registry->count; ++i) {
        TypeTreeSchemaKey existing = schema_key_view(&registry->entries[i].key);
        if (compare_keys(&existing, key) == 0) {
            return schemas_equal(&registry->entries[i].schema, schema)
                ? TYPETREE_SCHEMA_OK
                : TYPETREE_SCHEMA_KEY_CONFLICT;
        }
    }

    TypeTreeSchemaRegistryEntry pending;
    memset(&pending, 0, sizeof(pending));
    status = owned_key_copy(&pending.key, key);
    if (status != TYPETREE_SCHEMA_OK) return status;
    status = clone_schema(&pending.schema, schema);
    if (status != TYPETREE_SCHEMA_OK) {
        registry_entry_dispose(&pending);
        return status;
    }
    status = registry_reserve(registry, registry->count + 1U);
    if (status != TYPETREE_SCHEMA_OK) {
        registry_entry_dispose(&pending);
        return status;
    }
    size_t insertion_index = 0U;
    while (insertion_index < registry->count) {
        TypeTreeSchemaKey existing = schema_key_view(
            &registry->entries[insertion_index].key);
        if (compare_keys(&existing, key) > 0) break;
        ++insertion_index;
    }
    if (insertion_index < registry->count) {
        memmove(&registry->entries[insertion_index + 1U],
                &registry->entries[insertion_index],
                (registry->count - insertion_index) *
                    sizeof(*registry->entries));
    }
    registry->entries[insertion_index] = pending;
    ++registry->count;
    return TYPETREE_SCHEMA_OK;
}

TypeTreeSchemaStatus typetree_schema_registry_lookup(
    const TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* key,
    TypeTreeType* out_schema) {
    if (!registry || !out_schema) return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    memset(out_schema, 0, sizeof(*out_schema));
    TypeTreeSchemaStatus status = validate_key(key);
    if (status != TYPETREE_SCHEMA_OK) return status;
    for (size_t i = 0; i < registry->count; ++i) {
        TypeTreeSchemaKey existing = schema_key_view(&registry->entries[i].key);
        if (compare_keys(&existing, key) == 0) {
            return clone_schema(out_schema, &registry->entries[i].schema);
        }
    }
    return TYPETREE_SCHEMA_NOT_FOUND;
}

static bool schema_keys_match_except_unity_version(
    const TypeTreeSchemaKey* left, const TypeTreeSchemaKey* right) {
    return left->serialized_file_version == right->serialized_file_version &&
        left->class_id == right->class_id &&
        left->serialized_type_id == right->serialized_type_id &&
        left->script_type_index == right->script_type_index &&
        left->has_script_id_hash == right->has_script_id_hash &&
        left->is_stripped == right->is_stripped &&
        left->is_ref_type == right->is_ref_type &&
        memcmp(left->script_id_hash, right->script_id_hash,
               sizeof(left->script_id_hash)) == 0 &&
        memcmp(left->type_hash, right->type_hash,
               sizeof(left->type_hash)) == 0;
}

TypeTreeSchemaStatus typetree_schema_registry_bind_exact_version(
    TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* destination_key,
    TypeTreeSchemaProvenance evidence_provenance) {
    if (!registry ||
        evidence_provenance != TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    TypeTreeSchemaStatus status = validate_key(destination_key);
    if (status != TYPETREE_SCHEMA_OK) return status;

    const TypeTreeType* selected = NULL;
    for (size_t i = 0U; i < registry->count; ++i) {
        TypeTreeSchemaKey existing =
            schema_key_view(&registry->entries[i].key);
        if (compare_keys(&existing, destination_key) == 0) {
            return TYPETREE_SCHEMA_OK;
        }
        if (!schema_keys_match_except_unity_version(
                &existing, destination_key)) {
            continue;
        }
        if (selected &&
            !schemas_equal(selected, &registry->entries[i].schema)) {
            return TYPETREE_SCHEMA_KEY_CONFLICT;
        }
        selected = &registry->entries[i].schema;
    }
    if (!selected) return TYPETREE_SCHEMA_NOT_FOUND;

    TypeTreeSchemaProfileResult profile =
        typetree_schema_validate_known_profile(
            destination_key->unity_version,
            destination_key->unity_version_size,
            destination_key->class_id, destination_key->type_hash, selected);
    if (profile != TYPETREE_SCHEMA_PROFILE_VALID) {
        return TYPETREE_SCHEMA_INVALID_SCHEMA;
    }
    return typetree_schema_registry_learn(
        registry, destination_key, selected, evidence_provenance);
}

static int compare_entry_pointers(const void* left, const void* right) {
    const TypeTreeSchemaRegistryEntry* a =
        *(const TypeTreeSchemaRegistryEntry* const*)left;
    const TypeTreeSchemaRegistryEntry* b =
        *(const TypeTreeSchemaRegistryEntry* const*)right;
    TypeTreeSchemaKey a_key = schema_key_view(&a->key);
    TypeTreeSchemaKey b_key = schema_key_view(&b->key);
    return compare_keys(&a_key, &b_key);
}

static bool write_schema_record(SchemaWriter* writer,
                                const TypeTreeSchemaRegistryEntry* entry) {
    TypeTreeSchemaKey key = schema_key_view(&entry->key);
    const TypeTreeType* schema = &entry->schema;
    size_t class_name_size = 0U;
    size_t namespace_size = 0U;
    size_t assembly_size = 0U;
    if (schema->is_ref_type &&
        (!bounded_string_size(schema->ref_class_name,
                              SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                              &class_name_size) ||
         !bounded_string_size(schema->ref_namespace,
                              SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                              &namespace_size) ||
         !bounded_string_size(schema->ref_asm_name,
                              SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
                              &assembly_size))) {
        writer->status = TYPETREE_SCHEMA_INVALID_SCHEMA;
        return false;
    }

    size_t record_start = writer->size;
    uint16_t key_flags =
        (key.has_script_id_hash ? SCHEMA_KEY_FLAG_SCRIPT_HASH : 0U) |
        (key.is_ref_type ? SCHEMA_KEY_FLAG_REF_TYPE : 0U) |
        (key.is_stripped ? SCHEMA_KEY_FLAG_STRIPPED : 0U);
    uint32_t schema_flags =
        schema->is_stripped ? SCHEMA_VALUE_FLAG_STRIPPED : 0U;
    if (!schema_writer_u64(writer, 0U) ||
        !schema_writer_u32(writer, key.serialized_file_version) ||
        !schema_writer_u32(writer, (uint32_t)key.unity_version_size) ||
        !schema_writer_bytes(writer, key.unity_version,
                             key.unity_version_size) ||
        !schema_writer_u32(writer, (uint32_t)key.class_id) ||
        !schema_writer_u32(writer, (uint32_t)key.serialized_type_id) ||
        !schema_writer_u16(writer, key.script_type_index) ||
        !schema_writer_u16(writer, key_flags) ||
        !schema_writer_bytes(writer, key.script_id_hash,
                             sizeof(key.script_id_hash)) ||
        !schema_writer_bytes(writer, key.type_hash, sizeof(key.type_hash)) ||
        !schema_writer_u32(writer, schema_flags) ||
        !schema_writer_u32(writer, (uint32_t)schema->node_count) ||
        !schema_writer_u32(writer, schema->string_buffer_size) ||
        !schema_writer_u32(writer, (uint32_t)schema->dependency_count) ||
        !schema_writer_u32(writer, (uint32_t)class_name_size) ||
        !schema_writer_u32(writer, (uint32_t)namespace_size) ||
        !schema_writer_u32(writer, (uint32_t)assembly_size)) {
        return false;
    }
    for (int i = 0; i < schema->node_count; ++i) {
        const TypeTreeNode* node = &schema->nodes[i];
        if (!schema_writer_u16(writer, node->version) ||
            !schema_writer_u8(writer, node->level) ||
            !schema_writer_u8(writer, node->type_flags) ||
            !schema_writer_u32(writer, node->type_str_offset) ||
            !schema_writer_u32(writer, node->name_str_offset) ||
            !schema_writer_u32(writer, (uint32_t)node->byte_size) ||
            !schema_writer_u32(writer, node->index) ||
            !schema_writer_u32(writer, node->meta_flags) ||
            !schema_writer_u64(writer, node->ref_type_hash)) {
            return false;
        }
    }
    if (!schema_writer_bytes(writer, schema->string_buffer,
                             schema->string_buffer_size)) {
        return false;
    }
    for (int i = 0; i < schema->dependency_count; ++i) {
        if (!schema_writer_u32(writer, (uint32_t)schema->dependencies[i])) {
            return false;
        }
    }
    if (!schema_writer_bytes(writer, schema->ref_class_name,
                             class_name_size) ||
        !schema_writer_bytes(writer, schema->ref_namespace, namespace_size) ||
        !schema_writer_bytes(writer, schema->ref_asm_name, assembly_size)) {
        return false;
    }
    store_le64(writer->data + record_start,
               (uint64_t)(writer->size - record_start));
    return true;
}

TypeTreeSchemaStatus typetree_schema_registry_serialize(
    const TypeTreeSchemaRegistry* registry,
    uint8_t** out_data,
    size_t* out_size) {
    if (!registry || !out_data || !out_size ||
        registry->count > SCHEMA_REGISTRY_MAX_ENTRIES ||
        registry->count > registry->capacity ||
        (registry->count != 0U && !registry->entries)) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0U;

    const TypeTreeSchemaRegistryEntry** ordered = NULL;
    size_t ordered_size = 0U;
    if (registry->count != 0U) {
        if (!checked_multiply_size(registry->count, sizeof(*ordered),
                                   &ordered_size)) {
            return TYPETREE_SCHEMA_SIZE_OVERFLOW;
        }
        ordered = (const TypeTreeSchemaRegistryEntry**)mem_alloc(ordered_size);
        if (!ordered) return TYPETREE_SCHEMA_ALLOCATION_FAILED;
        for (size_t i = 0; i < registry->count; ++i) {
            ordered[i] = &registry->entries[i];
            TypeTreeSchemaKey key = schema_key_view(&ordered[i]->key);
            TypeTreeSchemaStatus status = validate_schema(
                &key, &ordered[i]->schema);
            if (status != TYPETREE_SCHEMA_OK) {
                mem_free(ordered, ordered_size);
                return status;
            }
        }
        qsort(ordered, registry->count, sizeof(*ordered),
              compare_entry_pointers);
        for (size_t i = 1; i < registry->count; ++i) {
            TypeTreeSchemaKey previous = schema_key_view(&ordered[i - 1]->key);
            TypeTreeSchemaKey current = schema_key_view(&ordered[i]->key);
            if (compare_keys(&previous, &current) == 0) {
                mem_free(ordered, ordered_size);
                return TYPETREE_SCHEMA_KEY_CONFLICT;
            }
        }
    }

    SchemaWriter writer;
    memset(&writer, 0, sizeof(writer));
    writer.status = TYPETREE_SCHEMA_OK;
    if (!schema_writer_zeros(&writer, SCHEMA_REGISTRY_HEADER_SIZE)) goto fail;
    for (size_t i = 0; i < registry->count; ++i) {
        if (!write_schema_record(&writer, ordered[i])) goto fail;
    }

    memcpy(writer.data, k_schema_registry_magic,
           sizeof(k_schema_registry_magic));
    store_le16(writer.data + 8U,
               (uint16_t)TYPETREE_SCHEMA_REGISTRY_FORMAT_VERSION);
    store_le16(writer.data + 10U, SCHEMA_REGISTRY_HEADER_SIZE);
    store_le32(writer.data + 12U, 0U);
    store_le32(writer.data + 16U, (uint32_t)registry->count);
    store_le32(writer.data + 20U, 0U);
    store_le64(writer.data + 24U, (uint64_t)writer.size);
    common_sha256(writer.data + SCHEMA_REGISTRY_HEADER_SIZE,
                  writer.size - SCHEMA_REGISTRY_HEADER_SIZE,
                  writer.data + 32U);

    uint8_t* result = (uint8_t*)mem_alloc(writer.size);
    if (!result) {
        writer.status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        goto fail;
    }
    memcpy(result, writer.data, writer.size);
    *out_data = result;
    *out_size = writer.size;
    mem_free(ordered, ordered_size);
    schema_writer_dispose(&writer);
    return TYPETREE_SCHEMA_OK;

fail: {
        TypeTreeSchemaStatus status = writer.status;
        if (status == TYPETREE_SCHEMA_OK) {
            status = TYPETREE_SCHEMA_INVALID_SCHEMA;
        }
        mem_free(ordered, ordered_size);
        schema_writer_dispose(&writer);
        return status;
    }
}

static char* read_owned_string(SchemaReader* reader, uint32_t size,
                               uint32_t limit,
                               TypeTreeSchemaStatus* out_status) {
    if (size > limit) {
        *out_status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        return NULL;
    }
    char* string = (char*)mem_alloc((size_t)size + 1U);
    if (!string) {
        *out_status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        return NULL;
    }
    if (!schema_reader_bytes(reader, string, size) ||
        memchr(string, '\0', size) != NULL) {
        mem_free(string, (size_t)size + 1U);
        *out_status = TYPETREE_SCHEMA_INVALID_FORMAT;
        return NULL;
    }
    string[size] = '\0';
    return string;
}

static TypeTreeSchemaStatus read_schema_record(
    SchemaReader* input, TypeTreeSchemaRegistry* registry) {
    uint64_t record_size_u64;
    if (!schema_reader_u64(input, &record_size_u64) ||
        record_size_u64 < 8U || record_size_u64 > SIZE_MAX) {
        return TYPETREE_SCHEMA_INVALID_FORMAT;
    }
    size_t record_payload_size = (size_t)record_size_u64 - 8U;
    const uint8_t* record_payload;
    if (!schema_reader_view(input, record_payload_size, &record_payload)) {
        return TYPETREE_SCHEMA_INVALID_FORMAT;
    }
    SchemaReader reader = {record_payload, record_payload_size, 0U};
    TypeTreeSchemaKey key;
    TypeTreeType schema;
    memset(&key, 0, sizeof(key));
    memset(&schema, 0, sizeof(schema));
    char* unity_version = NULL;
    TypeTreeSchemaStatus status = TYPETREE_SCHEMA_INVALID_FORMAT;

    uint32_t unity_version_size = 0U;
    uint32_t class_id;
    uint32_t serialized_type_id;
    uint16_t key_flags;
    uint32_t schema_flags;
    uint32_t node_count;
    uint32_t string_buffer_size;
    uint32_t dependency_count;
    uint32_t class_name_size;
    uint32_t namespace_size;
    uint32_t assembly_size;
    if (!schema_reader_u32(&reader, &key.serialized_file_version) ||
        !schema_reader_u32(&reader, &unity_version_size) ||
        unity_version_size == 0U ||
        unity_version_size >
            TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE) {
        if (unity_version_size >
            TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE) {
            status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        }
        goto fail;
    }
    unity_version = read_owned_string(
        &reader, unity_version_size,
        TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE, &status);
    if (!unity_version) goto fail;
    key.unity_version = unity_version;
    key.unity_version_size = unity_version_size;
    if (!schema_reader_u32(&reader, &class_id) ||
        !schema_reader_u32(&reader, &serialized_type_id) ||
        !schema_reader_u16(&reader, &key.script_type_index) ||
        !schema_reader_u16(&reader, &key_flags) ||
        (key_flags & ~SCHEMA_KEY_KNOWN_FLAGS) != 0U ||
        !schema_reader_bytes(&reader, key.script_id_hash,
                             sizeof(key.script_id_hash)) ||
        !schema_reader_bytes(&reader, key.type_hash,
                             sizeof(key.type_hash)) ||
        !schema_reader_u32(&reader, &schema_flags) ||
        (schema_flags & ~SCHEMA_VALUE_KNOWN_FLAGS) != 0U ||
        !schema_reader_u32(&reader, &node_count) ||
        !schema_reader_u32(&reader, &string_buffer_size) ||
        !schema_reader_u32(&reader, &dependency_count) ||
        !schema_reader_u32(&reader, &class_name_size) ||
        !schema_reader_u32(&reader, &namespace_size) ||
        !schema_reader_u32(&reader, &assembly_size)) {
        goto fail;
    }
    if (node_count == 0U || node_count > SCHEMA_REGISTRY_MAX_NODES ||
        node_count > INT_MAX ||
        string_buffer_size > SCHEMA_REGISTRY_MAX_STRING_BUFFER ||
        dependency_count > SCHEMA_REGISTRY_MAX_DEPENDENCIES ||
        dependency_count > INT_MAX ||
        class_name_size > SCHEMA_REGISTRY_MAX_REF_STRING_SIZE ||
        namespace_size > SCHEMA_REGISTRY_MAX_REF_STRING_SIZE ||
        assembly_size > SCHEMA_REGISTRY_MAX_REF_STRING_SIZE) {
        status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        goto fail;
    }
    key.class_id = (int32_t)class_id;
    key.serialized_type_id = (int32_t)serialized_type_id;
    key.has_script_id_hash =
        (key_flags & SCHEMA_KEY_FLAG_SCRIPT_HASH) != 0U;
    key.is_stripped = (key_flags & SCHEMA_KEY_FLAG_STRIPPED) != 0U;
    key.is_ref_type = (key_flags & SCHEMA_KEY_FLAG_REF_TYPE) != 0U;
    status = validate_key(&key);
    if (status != TYPETREE_SCHEMA_OK) goto fail;

    if (registry->count != 0U) {
        TypeTreeSchemaKey previous = schema_key_view(
            &registry->entries[registry->count - 1U].key);
        if (compare_keys(&previous, &key) >= 0) {
            status = TYPETREE_SCHEMA_INVALID_FORMAT;
            goto fail;
        }
    }

    /* All remaining fall-through parse failures are malformed records. */
    status = TYPETREE_SCHEMA_INVALID_FORMAT;

    schema.type_id = key.serialized_type_id;
    schema.is_stripped =
        (schema_flags & SCHEMA_VALUE_FLAG_STRIPPED) != 0U;
    schema.script_type_index = key.script_type_index;
    memcpy(schema.script_id_hash, key.script_id_hash,
           sizeof(schema.script_id_hash));
    memcpy(schema.type_hash, key.type_hash, sizeof(schema.type_hash));
    schema.node_count = (int)node_count;
    schema.string_buffer_size = string_buffer_size;
    schema.dependency_count = (int)dependency_count;
    schema.is_ref_type = key.is_ref_type;

    size_t allocation_size;
    if (!checked_multiply_size(node_count, sizeof(TypeTreeNode),
                               &allocation_size)) {
        status = TYPETREE_SCHEMA_SIZE_OVERFLOW;
        goto fail;
    }
    schema.nodes = (TypeTreeNode*)mem_alloc(allocation_size);
    if (!schema.nodes) {
        status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        goto fail;
    }
    memset(schema.nodes, 0, allocation_size);
    for (uint32_t i = 0; i < node_count; ++i) {
        TypeTreeNode* node = &schema.nodes[i];
        uint32_t byte_size;
        if (!schema_reader_u16(&reader, &node->version) ||
            !schema_reader_u8(&reader, &node->level) ||
            !schema_reader_u8(&reader, &node->type_flags) ||
            !schema_reader_u32(&reader, &node->type_str_offset) ||
            !schema_reader_u32(&reader, &node->name_str_offset) ||
            !schema_reader_u32(&reader, &byte_size) ||
            !schema_reader_u32(&reader, &node->index) ||
            !schema_reader_u32(&reader, &node->meta_flags) ||
            !schema_reader_u64(&reader, &node->ref_type_hash)) {
            goto fail;
        }
        node->byte_size = (int32_t)byte_size;
    }
    if (string_buffer_size != 0U) {
        schema.string_buffer = (uint8_t*)mem_alloc(string_buffer_size);
        if (!schema.string_buffer) {
            status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
            goto fail;
        }
        if (!schema_reader_bytes(&reader, schema.string_buffer,
                                 string_buffer_size)) {
            status = TYPETREE_SCHEMA_INVALID_FORMAT;
            goto fail;
        }
    }
    if (dependency_count != 0U) {
        if (!checked_multiply_size(dependency_count, sizeof(int32_t),
                                   &allocation_size)) {
            status = TYPETREE_SCHEMA_SIZE_OVERFLOW;
            goto fail;
        }
        schema.dependencies = (int32_t*)mem_alloc(allocation_size);
        if (!schema.dependencies) {
            status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
            goto fail;
        }
        for (uint32_t i = 0; i < dependency_count; ++i) {
            uint32_t dependency;
            if (!schema_reader_u32(&reader, &dependency)) goto fail;
            schema.dependencies[i] = (int32_t)dependency;
        }
    }
    if (schema.is_ref_type) {
        schema.ref_class_name = read_owned_string(
            &reader, class_name_size, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!schema.ref_class_name) goto fail;
        schema.ref_namespace = read_owned_string(
            &reader, namespace_size, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!schema.ref_namespace) goto fail;
        schema.ref_asm_name = read_owned_string(
            &reader, assembly_size, SCHEMA_REGISTRY_MAX_REF_STRING_SIZE,
            &status);
        if (!schema.ref_asm_name) goto fail;
    } else if (class_name_size != 0U || namespace_size != 0U ||
               assembly_size != 0U) {
        goto fail;
    }
    if (reader.position != reader.size) goto fail;

    if (!typetree_bind_node_strings(&schema)) goto fail;
    status = validate_schema(&key, &schema);
    if (status != TYPETREE_SCHEMA_OK) goto fail;
    status = typetree_schema_registry_learn(
        registry, &key, &schema,
        TYPETREE_SCHEMA_PROVENANCE_IMPORTED_REGISTRY);

fail:
    typetree_free_type(&schema);
    mem_free(unity_version, (size_t)unity_version_size + 1U);
    return status;
}

TypeTreeSchemaStatus typetree_schema_registry_deserialize_replace(
    TypeTreeSchemaRegistry* registry,
    const uint8_t* data,
    size_t size) {
    if (!registry || !data) return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    if (size < SCHEMA_REGISTRY_HEADER_SIZE) {
        return TYPETREE_SCHEMA_INVALID_FORMAT;
    }
    if ((uint64_t)size > SCHEMA_REGISTRY_MAX_FILE_SIZE) {
        return TYPETREE_SCHEMA_LIMIT_EXCEEDED;
    }
    if (memcmp(data, k_schema_registry_magic,
               sizeof(k_schema_registry_magic)) != 0 ||
        load_le16(data + 8U) !=
            TYPETREE_SCHEMA_REGISTRY_FORMAT_VERSION ||
        load_le16(data + 10U) != SCHEMA_REGISTRY_HEADER_SIZE ||
        load_le32(data + 12U) != 0U ||
        load_le32(data + 20U) != 0U ||
        load_le64(data + 24U) != (uint64_t)size) {
        return TYPETREE_SCHEMA_INVALID_FORMAT;
    }
    uint32_t entry_count = load_le32(data + 16U);
    if (entry_count > SCHEMA_REGISTRY_MAX_ENTRIES) {
        return TYPETREE_SCHEMA_LIMIT_EXCEEDED;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(data + SCHEMA_REGISTRY_HEADER_SIZE,
                  size - SCHEMA_REGISTRY_HEADER_SIZE, digest);
    if (memcmp(digest, data + 32U, sizeof(digest)) != 0) {
        return TYPETREE_SCHEMA_DIGEST_MISMATCH;
    }

    TypeTreeSchemaRegistry replacement;
    typetree_schema_registry_init(&replacement);
    TypeTreeSchemaStatus status = registry_reserve(&replacement, entry_count);
    if (status != TYPETREE_SCHEMA_OK) goto fail;
    SchemaReader reader = {data, size, SCHEMA_REGISTRY_HEADER_SIZE};
    for (uint32_t i = 0; i < entry_count; ++i) {
        status = read_schema_record(&reader, &replacement);
        if (status != TYPETREE_SCHEMA_OK) goto fail;
    }
    if (reader.position != reader.size || replacement.count != entry_count) {
        status = TYPETREE_SCHEMA_INVALID_FORMAT;
        goto fail;
    }

    typetree_schema_registry_dispose(registry);
    *registry = replacement;
    return TYPETREE_SCHEMA_OK;

fail:
    typetree_schema_registry_dispose(&replacement);
    return status;
}

#ifdef _WIN32

static bool registry_windows_regular_handle(HANDLE handle,
                                            uint64_t* out_size) {
    BY_HANDLE_FILE_INFORMATION information;
    LARGE_INTEGER size;
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, &information) ||
        !GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
          FILE_ATTRIBUTE_DEVICE)) != 0U) {
        return false;
    }
    if (out_size) *out_size = (uint64_t)size.QuadPart;
    return true;
}

static TypeTreeSchemaStatus registry_windows_conversion_status(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) {
        return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    }
    if (error == ERROR_NO_UNICODE_TRANSLATION ||
        error == ERROR_INVALID_PARAMETER) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    return TYPETREE_SCHEMA_IO_ERROR;
}

static bool registry_target_is_regular_or_missing(const wchar_t* path) {
    HANDLE handle = CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool regular = registry_windows_regular_handle(handle, NULL);
    if (!CloseHandle(handle)) regular = false;
    return regular;
}

static bool registry_write_all(HANDLE handle, const uint8_t* data,
                               size_t size) {
    while (size != 0U) {
        DWORD chunk = size > (size_t)UINT32_MAX
                          ? UINT32_MAX
                          : (DWORD)size;
        DWORD written = 0U;
        if (!WriteFile(handle, data, chunk, &written, NULL) ||
            written == 0U) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

static TypeTreeSchemaStatus registry_atomic_write(
    const char* path, const uint8_t* data, size_t size) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) {
        return registry_windows_conversion_status(GetLastError());
    }
    if (!registry_target_is_regular_or_missing(wide_path)) {
        free(wide_path);
        return TYPETREE_SCHEMA_IO_ERROR;
    }
    size_t path_size = strlen(path);
    if (path_size > SIZE_MAX - 96U) {
        free(wide_path);
        return TYPETREE_SCHEMA_SIZE_OVERFLOW;
    }
    size_t temporary_size = path_size + 96U;
    char* temporary = (char*)malloc(temporary_size);
    if (!temporary) {
        free(wide_path);
        return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    }
    temporary[0] = '\0';

    HANDLE handle = INVALID_HANDLE_VALUE;
    wchar_t* wide_temporary = NULL;
    TypeTreeSchemaStatus creation_status = TYPETREE_SCHEMA_IO_ERROR;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        int count = snprintf(
            temporary, temporary_size, "%s.tmp.%lu.%lu.%llu.%u", path,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)GetTickCount64(), attempt);
        if (count < 0 || (size_t)count >= temporary_size) break;
        wide_temporary = common_windows_utf8_to_wide(temporary);
        if (!wide_temporary) {
            creation_status = registry_windows_conversion_status(
                GetLastError());
            break;
        }
        handle = CreateFileW(
            wide_temporary, GENERIC_WRITE, 0U, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = handle == INVALID_HANDLE_VALUE
            ? GetLastError() : ERROR_SUCCESS;
        if (handle != INVALID_HANDLE_VALUE) break;
        free(wide_temporary);
        wide_temporary = NULL;
        if (error != ERROR_FILE_EXISTS) break;
    }

    bool success = handle != INVALID_HANDLE_VALUE;
    if (success) success = registry_write_all(handle, data, size);
    if (success) success = FlushFileBuffers(handle) != 0;
    if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) {
        success = false;
    }
    if (success) success = registry_target_is_regular_or_missing(wide_path);
    if (success) {
        success = MoveFileExW(
            wide_temporary, wide_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!success && wide_temporary) (void)DeleteFileW(wide_temporary);
    free(wide_temporary);
    free(temporary);
    free(wide_path);
    return success ? TYPETREE_SCHEMA_OK : creation_status;
}

static TypeTreeSchemaStatus registry_read_file(
    const char* path, uint8_t** out_data, size_t* out_size) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) {
        return registry_windows_conversion_status(GetLastError());
    }
    HANDLE handle = CreateFileW(
        wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) return TYPETREE_SCHEMA_IO_ERROR;

    uint64_t file_size = 0U;
    TypeTreeSchemaStatus status = TYPETREE_SCHEMA_IO_ERROR;
    uint8_t* data = NULL;
    if (!registry_windows_regular_handle(handle, &file_size)) goto done;
    if (file_size > SCHEMA_REGISTRY_MAX_FILE_SIZE || file_size > SIZE_MAX) {
        status = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        goto done;
    }
    if (file_size == 0U) {
        status = TYPETREE_SCHEMA_INVALID_FORMAT;
        goto done;
    }

    size_t size = (size_t)file_size;
    data = (uint8_t*)mem_alloc(size);
    if (!data) {
        status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        goto done;
    }
    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        DWORD chunk = remaining > (size_t)UINT32_MAX
                          ? UINT32_MAX
                          : (DWORD)remaining;
        DWORD received = 0U;
        if (!ReadFile(handle, data + position, chunk, &received, NULL) ||
            received == 0U) {
            goto done;
        }
        position += received;
    }
    uint8_t extra;
    DWORD received = 0U;
    uint64_t final_size = 0U;
    if (!ReadFile(handle, &extra, 1U, &received, NULL) || received != 0U ||
        !registry_windows_regular_handle(handle, &final_size) ||
        final_size != file_size) {
        goto done;
    }
    *out_data = data;
    *out_size = size;
    data = NULL;
    status = TYPETREE_SCHEMA_OK;

done:
    if (data) mem_free(data, (size_t)file_size);
    if (!CloseHandle(handle) && status == TYPETREE_SCHEMA_OK) {
        mem_free(*out_data, *out_size);
        *out_data = NULL;
        *out_size = 0U;
        status = TYPETREE_SCHEMA_IO_ERROR;
    }
    return status;
}

#else

static bool registry_target_is_regular_or_missing(const char* path) {
    struct stat status;
    if (lstat(path, &status) == 0) return S_ISREG(status.st_mode);
    return errno == ENOENT;
}

static bool registry_write_all(int descriptor, const uint8_t* data,
                               size_t size) {
    while (size != 0U) {
        size_t chunk = size > (size_t)INT_MAX ? (size_t)INT_MAX : size;
        ssize_t written = write(descriptor, data, chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool registry_sync_parent_directory(const char* path) {
    const char* separator = strrchr(path, '/');
    const char* directory = ".";
    char* owned_directory = NULL;
    if (separator) {
        size_t size = separator == path ? 1U : (size_t)(separator - path);
        owned_directory = (char*)malloc(size + 1U);
        if (!owned_directory) return false;
        memcpy(owned_directory, path, size);
        owned_directory[size] = '\0';
        directory = owned_directory;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = open(directory, flags);
    bool success = descriptor >= 0;
    if (success) {
        struct stat status;
        success = fstat(descriptor, &status) == 0 &&
                  S_ISDIR(status.st_mode) && fsync(descriptor) == 0;
        if (close(descriptor) != 0) success = false;
    }
    free(owned_directory);
    return success;
}

static TypeTreeSchemaStatus registry_atomic_write(
    const char* path, const uint8_t* data, size_t size) {
    if (!registry_target_is_regular_or_missing(path)) {
        return TYPETREE_SCHEMA_IO_ERROR;
    }
    static const char suffix[] = ".tmp.XXXXXX";
    size_t path_size = strlen(path);
    if (path_size > SIZE_MAX - sizeof(suffix)) {
        return TYPETREE_SCHEMA_SIZE_OVERFLOW;
    }
    size_t temporary_size = path_size + sizeof(suffix);
    char* temporary = (char*)malloc(temporary_size);
    if (!temporary) return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    memcpy(temporary, path, path_size);
    memcpy(temporary + path_size, suffix, sizeof(suffix));

    int descriptor = mkstemp(temporary);
    bool success = descriptor >= 0;
    if (success) {
        int descriptor_flags = fcntl(descriptor, F_GETFD);
        success = descriptor_flags >= 0 &&
                  fcntl(descriptor, F_SETFD,
                        descriptor_flags | FD_CLOEXEC) == 0;
    }
    if (success) success = registry_write_all(descriptor, data, size);
    if (success) success = fsync(descriptor) == 0;
    if (descriptor >= 0 && close(descriptor) != 0) success = false;
    if (success) success = registry_target_is_regular_or_missing(path);
    if (success) success = rename(temporary, path) == 0;
    if (!success) (void)unlink(temporary);
    if (success) success = registry_sync_parent_directory(path);
    free(temporary);
    return success ? TYPETREE_SCHEMA_OK : TYPETREE_SCHEMA_IO_ERROR;
}

static bool registry_read_all(int descriptor, uint8_t* data, size_t size) {
    while (size != 0U) {
        size_t chunk = size > (size_t)INT_MAX ? (size_t)INT_MAX : size;
        ssize_t received = read(descriptor, data, chunk);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        data += (size_t)received;
        size -= (size_t)received;
    }
    return true;
}

static int registry_open_regular_no_follow(const char* path) {
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    struct stat path_status;
    if (lstat(path, &path_status) != 0 ||
        !S_ISREG(path_status.st_mode)) {
        return -1;
    }
    flags |= O_NOFOLLOW;
    return open(path, flags);
#else
    /*
     * The inode check closes the lstat/open race on older POSIX targets.
     * A concurrent atomic writer may legitimately change the inode, so retry
     * instead of accepting an opened object whose path identity is unclear.
     */
    for (unsigned attempt = 0U; attempt < 64U; ++attempt) {
        struct stat path_status;
        if (lstat(path, &path_status) != 0 ||
            !S_ISREG(path_status.st_mode)) {
            return -1;
        }
        int descriptor = open(path, flags);
        if (descriptor < 0) continue;
        struct stat descriptor_status;
        if (fstat(descriptor, &descriptor_status) == 0 &&
            S_ISREG(descriptor_status.st_mode) &&
            path_status.st_dev == descriptor_status.st_dev &&
            path_status.st_ino == descriptor_status.st_ino) {
            return descriptor;
        }
        (void)close(descriptor);
    }
    return -1;
#endif
}

static TypeTreeSchemaStatus registry_read_file(
    const char* path, uint8_t** out_data, size_t* out_size) {
    int descriptor = registry_open_regular_no_follow(path);
    if (descriptor < 0) return TYPETREE_SCHEMA_IO_ERROR;

    TypeTreeSchemaStatus result = TYPETREE_SCHEMA_IO_ERROR;
    struct stat initial_status;
    uint8_t* data = NULL;
    size_t size = 0U;
    if (fstat(descriptor, &initial_status) != 0 ||
        !S_ISREG(initial_status.st_mode) || initial_status.st_size < 0) {
        goto done;
    }
    if ((uint64_t)initial_status.st_size > SCHEMA_REGISTRY_MAX_FILE_SIZE ||
        (uint64_t)initial_status.st_size > SIZE_MAX) {
        result = TYPETREE_SCHEMA_LIMIT_EXCEEDED;
        goto done;
    }
    size = (size_t)initial_status.st_size;
    if (size == 0U) {
        result = TYPETREE_SCHEMA_INVALID_FORMAT;
        goto done;
    }
    data = (uint8_t*)mem_alloc(size);
    if (!data) {
        result = TYPETREE_SCHEMA_ALLOCATION_FAILED;
        goto done;
    }
    if (!registry_read_all(descriptor, data, size)) goto done;
    uint8_t extra;
    ssize_t received;
    do {
        received = read(descriptor, &extra, 1U);
    } while (received < 0 && errno == EINTR);
    struct stat final_status;
    if (received != 0 || fstat(descriptor, &final_status) != 0 ||
        !S_ISREG(final_status.st_mode) ||
        final_status.st_dev != initial_status.st_dev ||
        final_status.st_ino != initial_status.st_ino ||
        final_status.st_size != initial_status.st_size) {
        goto done;
    }
    *out_data = data;
    *out_size = size;
    data = NULL;
    result = TYPETREE_SCHEMA_OK;

done:
    if (data) mem_free(data, size);
    if (close(descriptor) != 0 && result == TYPETREE_SCHEMA_OK) {
        mem_free(*out_data, *out_size);
        *out_data = NULL;
        *out_size = 0U;
        result = TYPETREE_SCHEMA_IO_ERROR;
    }
    return result;
}

#endif

TypeTreeSchemaStatus typetree_schema_registry_export_file(
    const TypeTreeSchemaRegistry* registry,
    const char* path) {
    if (!registry || !path || path[0] == '\0') {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    uint8_t* data = NULL;
    size_t size = 0U;
    TypeTreeSchemaStatus status = typetree_schema_registry_serialize(
        registry, &data, &size);
    if (status != TYPETREE_SCHEMA_OK) return status;
    status = registry_atomic_write(path, data, size);
    mem_free(data, size);
    return status;
}

TypeTreeSchemaStatus typetree_schema_registry_import_file_replace(
    TypeTreeSchemaRegistry* registry,
    const char* path) {
    if (!registry || !path || path[0] == '\0') {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    uint8_t* data = NULL;
    size_t size = 0U;
    TypeTreeSchemaStatus status = registry_read_file(
        path, &data, &size);
    if (status != TYPETREE_SCHEMA_OK) return status;
    status = typetree_schema_registry_deserialize_replace(
        registry, data, size);
    mem_free(data, size);
    return status;
}

const char* typetree_schema_status_name(TypeTreeSchemaStatus status) {
    switch (status) {
        case TYPETREE_SCHEMA_OK: return "ok";
        case TYPETREE_SCHEMA_NOT_FOUND: return "not_found";
        case TYPETREE_SCHEMA_INVALID_ARGUMENT: return "invalid_argument";
        case TYPETREE_SCHEMA_INVALID_SCHEMA: return "invalid_schema";
        case TYPETREE_SCHEMA_KEY_CONFLICT: return "key_conflict";
        case TYPETREE_SCHEMA_ALLOCATION_FAILED: return "allocation_failed";
        case TYPETREE_SCHEMA_SIZE_OVERFLOW: return "size_overflow";
        case TYPETREE_SCHEMA_IO_ERROR: return "io_error";
        case TYPETREE_SCHEMA_INVALID_FORMAT: return "invalid_format";
        case TYPETREE_SCHEMA_DIGEST_MISMATCH: return "digest_mismatch";
        case TYPETREE_SCHEMA_LIMIT_EXCEEDED: return "limit_exceeded";
        default: return "unknown";
    }
}
