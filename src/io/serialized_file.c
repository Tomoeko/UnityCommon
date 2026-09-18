// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file.h"

#include "serialized_metadata_materialization_internal.h"
#include "typetree_directory_internal.h"

#include "io/serialized_file_directory.h"
#include "io/serialized_file_metadata_tail.h"

#include <limits.h>
#include <stdlib.h>

typedef struct {
    int64_t path_id;
    uint64_t byte_offset;
    uint64_t byte_end;
    uint32_t byte_size;
} ObjectValidationRecord;

/* Convenience-open policy, not Unity format limits. These ceilings cover the
 * configured fixtures and leave finite headroom for ordinary materialization.
 * Raw owners are released before object-validation scratch or registry work.
 * During tail construction both indexes coexist; semantic rows remain owned
 * by the file. These limits bound raw construction, not semantic schema work. */
enum {
    DIRECTORY_ADAPTER_VERSION_BYTES = 12,
    DIRECTORY_ADAPTER_MAX_TYPES = 65536,
    DIRECTORY_ADAPTER_MAX_OBJECTS = 1048576,
    DIRECTORY_ADAPTER_MAX_NODES = 1048576,
    DIRECTORY_ADAPTER_MAX_STRING_BYTES = 64U * 1024U * 1024U,
    DIRECTORY_ADAPTER_MAX_DEPENDENCIES = 1048576,
    DIRECTORY_ADAPTER_MAX_METADATA_BYTES = 256U * 1024U * 1024U,
    DIRECTORY_ADAPTER_MAX_RETAINED_BYTES = 128U * 1024U * 1024U,
    DIRECTORY_ADAPTER_MAX_WORK = 1024U * 1024U * 1024U
};

enum {
    TAIL_ADAPTER_MAX_SCRIPTS = 1048576,
    TAIL_ADAPTER_MAX_EXTERNALS = 65536,
    TAIL_ADAPTER_MAX_REFERENCE_TYPES = 65536,
    TAIL_ADAPTER_MAX_NODES = 1048576,
    TAIL_ADAPTER_MAX_TREE_STRING_BYTES = 64U * 1024U * 1024U,
    TAIL_ADAPTER_MAX_TERMINATED_STRING_BYTES = 64U * 1024U * 1024U,
    TAIL_ADAPTER_MAX_TAIL_BYTES = 256U * 1024U * 1024U,
    TAIL_ADAPTER_MAX_RETAINED_BYTES = 128U * 1024U * 1024U,
    TAIL_ADAPTER_MAX_WORK = 1024U * 1024U * 1024U
};

static bool allocate_zeroed_array(void** out, size_t count,
                                  size_t element_size) {
    if (!out || element_size == 0 ||
        dxbc_size_multiply_overflows(count, element_size)) {
        return false;
    }
    *out = NULL;
    if (count == 0) return true;
    size_t allocation_size = count * element_size;
    void* values = mem_alloc(allocation_size);
    if (!values) return false;
    memset(values, 0, allocation_size);
    *out = values;
    return true;
}

static int compare_object_path_id(const void* left, const void* right) {
    const ObjectValidationRecord* a =
        (const ObjectValidationRecord*)left;
    const ObjectValidationRecord* b =
        (const ObjectValidationRecord*)right;
    return a->path_id < b->path_id ? -1 : a->path_id > b->path_id ? 1 : 0;
}

static int compare_object_range(const void* left, const void* right) {
    const ObjectValidationRecord* a =
        (const ObjectValidationRecord*)left;
    const ObjectValidationRecord* b =
        (const ObjectValidationRecord*)right;
    if (a->byte_offset != b->byte_offset) {
        return a->byte_offset < b->byte_offset ? -1 : 1;
    }
    if (a->byte_end != b->byte_end) {
        return a->byte_end < b->byte_end ? -1 : 1;
    }
    return 0;
}

static bool validate_object_table(const SerializedFile* file) {
    if (!file || file->object_count < 0 ||
        (file->object_count > 0 && !file->objects)) {
        return false;
    }
    if (file->object_count < 2) return true;

    ObjectValidationRecord* records = NULL;
    if (!allocate_zeroed_array((void**)&records,
                               (size_t)file->object_count,
                               sizeof(*records))) {
        return false;
    }
    const size_t allocation_size =
        (size_t)file->object_count * sizeof(*records);
    for (int i = 0; i < file->object_count; i++) {
        const AssetObjectInfo* object = &file->objects[i];
        records[i].path_id = object->path_id;
        records[i].byte_offset = object->byte_offset;
        records[i].byte_end = object->byte_offset + object->byte_size;
        records[i].byte_size = object->byte_size;
    }

    qsort(records, (size_t)file->object_count, sizeof(*records),
          compare_object_path_id);
    for (int i = 1; i < file->object_count; i++) {
        if (records[i - 1].path_id == records[i].path_id) {
            LOG_ERROR("SerializedFile has duplicate PathID %lld",
                      (long long)records[i].path_id);
            mem_free(records, allocation_size);
            return false;
        }
    }

    qsort(records, (size_t)file->object_count, sizeof(*records),
          compare_object_range);
    bool have_nonempty_range = false;
    uint64_t previous_end = 0;
    for (int i = 0; i < file->object_count; i++) {
        if (records[i].byte_size == 0) continue;
        if (have_nonempty_range && records[i].byte_offset < previous_end) {
            LOG_ERROR("SerializedFile object byte ranges overlap");
            mem_free(records, allocation_size);
            return false;
        }
        previous_end = records[i].byte_end;
        have_nonempty_range = true;
    }

    mem_free(records, allocation_size);
    return true;
}

static TypeTreeSchemaStatus clone_schema_registry(
    TypeTreeSchemaRegistry* destination,
    const TypeTreeSchemaRegistry* source) {
    if (!destination || !source) return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    typetree_schema_registry_init(destination);
    uint8_t* bytes = NULL;
    size_t size = 0;
    TypeTreeSchemaStatus status = typetree_schema_registry_serialize(
        source, &bytes, &size);
    if (status == TYPETREE_SCHEMA_OK) {
        status = typetree_schema_registry_deserialize_replace(
            destination, bytes, size);
    }
    if (bytes) mem_free(bytes, size);
    if (status != TYPETREE_SCHEMA_OK) {
        typetree_schema_registry_dispose(destination);
    }
    return status;
}

static TypeTreeSchemaStatus learn_type_array(
    TypeTreeSchemaRegistry* registry, uint32_t file_version,
    const char* unity_version, TypeTreeType* types, int type_count,
    TypeTreeSchemaProvenance provenance) {
    for (int i = 0; i < type_count; i++) {
        TypeTreeSchemaKey key;
        TypeTreeSchemaStatus status = typetree_schema_key_from_type(
            &key, file_version, unity_version, types[i].type_id, &types[i]);
        if (status != TYPETREE_SCHEMA_OK) return status;
        status = typetree_schema_registry_learn(
            registry, &key, &types[i], provenance);
        if (status != TYPETREE_SCHEMA_OK) return status;
    }
    return TYPETREE_SCHEMA_OK;
}

static TypeTreeSchemaStatus resolve_type_array(
    const TypeTreeSchemaRegistry* registry, uint32_t file_version,
    const char* unity_version, TypeTreeType* types, int type_count) {
    for (int i = 0; i < type_count; i++) {
        TypeTreeSchemaKey key;
        TypeTreeSchemaStatus status = typetree_schema_key_from_type(
            &key, file_version, unity_version, types[i].type_id, &types[i]);
        if (status != TYPETREE_SCHEMA_OK) return status;
        TypeTreeType resolved;
        status = typetree_schema_registry_lookup(registry, &key, &resolved);
        if (status != TYPETREE_SCHEMA_OK) return status;
        typetree_free_type(&types[i]);
        types[i] = resolved;
    }
    return TYPETREE_SCHEMA_OK;
}

static bool apply_schema_registry(SerializedFile* file,
                                  TypeTreeSchemaRegistry* registry,
                                  TypeTreeSchemaProvenance provenance) {
    if (!file) return false;
    if (!file->type_tree_enabled) {
        if (!registry) {
            LOG_ERROR("SerializedFile has no TypeTree and no exact schema registry was provided");
            return false;
        }
        TypeTreeSchemaStatus status = resolve_type_array(
            registry, file->version, file->unity_version,
            file->types, file->type_count);
        if (status == TYPETREE_SCHEMA_OK) {
            status = resolve_type_array(
                registry, file->version, file->unity_version,
                file->ref_types, file->ref_type_count);
        }
        if (status != TYPETREE_SCHEMA_OK) {
            LOG_ERROR("TypeTree-disabled schema resolution failed: %s",
                      typetree_schema_status_name(status));
            return false;
        }
        return true;
    }

    if (!registry ||
        provenance == TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT) {
        return true;
    }
    if (provenance != TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) {
        LOG_ERROR("Invalid provenance for a SerializedFile input");
        return false;
    }
    /* Do not partially mutate the caller's registry when a later schema in
     * the same file conflicts or fails validation. */
    TypeTreeSchemaRegistry transaction;
    TypeTreeSchemaStatus status = clone_schema_registry(&transaction,
                                                        registry);
    if (status == TYPETREE_SCHEMA_OK) {
        status = learn_type_array(
            &transaction, file->version, file->unity_version,
            file->types, file->type_count, provenance);
    }
    if (status == TYPETREE_SCHEMA_OK) {
        status = learn_type_array(
            &transaction, file->version, file->unity_version,
            file->ref_types, file->ref_type_count, provenance);
    }
    if (status != TYPETREE_SCHEMA_OK) {
        typetree_schema_registry_dispose(&transaction);
        LOG_ERROR("Authoritative TypeTree schema learning failed: %s",
                  typetree_schema_status_name(status));
        return false;
    }
    typetree_schema_registry_dispose(registry);
    *registry = transaction;
    return true;
}

typedef struct {
    TypeTreeType* target;
    TypeTreeType resolved;
} ResolvedTypeReplacement;

static size_t unresolved_class_schema_count(TypeTreeType* types,
                                            int type_count,
                                            int32_t class_id) {
    size_t count = 0U;
    for (int i = 0; i < type_count; ++i) {
        if (types[i].type_id == class_id &&
            (types[i].node_count == 0 || !types[i].nodes)) {
            ++count;
        }
    }
    return count;
}

static TypeTreeSchemaStatus resolve_class_schema_array(
    const SerializedFile* file, TypeTreeType* types, int type_count,
    int32_t class_id, const TypeTreeSchemaRegistry* registry,
    ResolvedTypeReplacement* replacements, size_t replacement_capacity,
    size_t* replacement_count) {
    for (int i = 0; i < type_count; ++i) {
        TypeTreeType* type = &types[i];
        if (type->type_id != class_id ||
            (type->node_count > 0 && type->nodes)) {
            continue;
        }
        if (*replacement_count >= replacement_capacity) {
            return TYPETREE_SCHEMA_SIZE_OVERFLOW;
        }
        TypeTreeSchemaKey key;
        TypeTreeSchemaStatus status = typetree_schema_key_from_type(
            &key, file->version, file->unity_version, class_id, type);
        if (status != TYPETREE_SCHEMA_OK) return status;
        ResolvedTypeReplacement* replacement =
            &replacements[*replacement_count];
        memset(replacement, 0, sizeof(*replacement));
        replacement->target = type;
        status = typetree_schema_registry_lookup(
            registry, &key, &replacement->resolved);
        if (status != TYPETREE_SCHEMA_OK) return status;
        ++*replacement_count;
    }
    return TYPETREE_SCHEMA_OK;
}

TypeTreeSchemaStatus serialized_file_resolve_class_schema(
    SerializedFile* file, int32_t class_id,
    const TypeTreeSchemaRegistry* schema_registry) {
    if (!file || !file->unity_version || class_id < 0) {
        return TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }
    size_t replacement_capacity = unresolved_class_schema_count(
        file->types, file->type_count, class_id);
    if (replacement_capacity > SIZE_MAX - unresolved_class_schema_count(
            file->ref_types, file->ref_type_count, class_id)) {
        return TYPETREE_SCHEMA_SIZE_OVERFLOW;
    }
    replacement_capacity += unresolved_class_schema_count(
        file->ref_types, file->ref_type_count, class_id);
    if (replacement_capacity == 0U) return TYPETREE_SCHEMA_OK;
    if (!schema_registry || dxbc_size_multiply_overflows(
            replacement_capacity, sizeof(ResolvedTypeReplacement))) {
        return schema_registry ? TYPETREE_SCHEMA_SIZE_OVERFLOW
                               : TYPETREE_SCHEMA_INVALID_ARGUMENT;
    }

    size_t allocation_size =
        replacement_capacity * sizeof(ResolvedTypeReplacement);
    ResolvedTypeReplacement* replacements =
        (ResolvedTypeReplacement*)mem_alloc(allocation_size);
    if (!replacements) return TYPETREE_SCHEMA_ALLOCATION_FAILED;
    memset(replacements, 0, allocation_size);

    size_t replacement_count = 0U;
    TypeTreeSchemaStatus status = resolve_class_schema_array(
        file, file->types, file->type_count, class_id, schema_registry,
        replacements, replacement_capacity, &replacement_count);
    if (status == TYPETREE_SCHEMA_OK) {
        status = resolve_class_schema_array(
            file, file->ref_types, file->ref_type_count, class_id,
            schema_registry, replacements, replacement_capacity,
            &replacement_count);
    }
    if (status == TYPETREE_SCHEMA_OK) {
        for (size_t i = 0U; i < replacement_count; ++i) {
            typetree_free_type(replacements[i].target);
            *replacements[i].target = replacements[i].resolved;
            memset(&replacements[i].resolved, 0,
                   sizeof(replacements[i].resolved));
        }
    }
    for (size_t i = 0U; i < replacement_count; ++i) {
        typetree_free_type(&replacements[i].resolved);
    }
    mem_free(replacements, allocation_size);
    return status;
}

static bool materialize_directory(SerializedFile* file, const SerializedFileDirectory* directory) {
    const SerializedFileDirectoryView* view = serialized_file_directory_view(directory);
    if (!view || view->type_count > DIRECTORY_ADAPTER_MAX_TYPES ||
        view->object_count > DIRECTORY_ADAPTER_MAX_OBJECTS) {
        return false;
    }
    const SerializedFilePrefixView* prefix = &view->prefix;
    file->version = prefix->header.format_version;
    file->metadata_size = prefix->header.metadata_size;
    file->file_size = prefix->header.file_size;
    file->data_offset = prefix->header.data_offset;
    file->big_endian = prefix->header.endian_selector != 0U;
    file->target_platform = prefix->target_platform;
    file->type_tree_enabled = prefix->type_tree_enabled_raw != 0U;
    const size_t version_bytes = prefix->version_source.size + 1U;
    file->unity_version = mem_alloc(version_bytes);
    if (!file->unity_version) {
        return false;
    }
    memcpy(file->unity_version, prefix->version_source.data, version_bytes);

    file->type_count = (int)view->type_count;
    if (!allocate_zeroed_array((void**)&file->types, view->type_count, sizeof(*file->types))) {
        return false;
    }
    for (size_t ordinal = 0U; ordinal < view->type_count; ++ordinal) {
        const SerializedFileDirectoryTypeRow* row =
            serialized_file_directory_type(directory, ordinal);
        if (!typetree_materialize_directory_type(&file->types[ordinal], row, file->big_endian)) {
            LOG_ERROR("Failed to materialize ordinary TypeTree row %zu", ordinal);
            return false;
        }
    }

    file->object_count = (int)view->object_count;
    if (!allocate_zeroed_array(
            (void**)&file->objects, view->object_count, sizeof(*file->objects))) {
        return false;
    }
    for (size_t ordinal = 0U; ordinal < view->object_count; ++ordinal) {
        const SerializedFileDirectoryObjectRow* row =
            serialized_file_directory_object(directory, ordinal);
        /* This semantic API retains its signed-offset policy; the physical
         * directory deliberately preserves the wider unsigned wire domain. */
        if (!row || row->relative_data_offset > INT64_MAX ||
            row->type_ordinal >= view->type_count || !file->types) {
            return false;
        }
        AssetObjectInfo* object = &file->objects[ordinal];
        object->path_id = serialized_metadata_signed64(row->path_id_bits);
        object->byte_offset = row->relative_data_offset;
        object->byte_size = row->byte_size;
        object->type_id_or_index = (int32_t)row->type_ordinal;
        const TypeTreeType* type = &file->types[row->type_ordinal];
        object->type_id = type->type_id;
        object->script_type_index = type->script_type_index;
    }
    return true;
}

static bool materialize_scripts(SerializedFile* file,
    const SerializedFileMetadataTail* tail,
    const SerializedFileMetadataTailView* view) {
    file->script_count = (int)view->script_count;
    if (!allocate_zeroed_array(
            (void**)&file->scripts, view->script_count, sizeof(*file->scripts))) {
        return false;
    }
    for (size_t ordinal = 0U; ordinal < view->script_count; ++ordinal) {
        const SerializedFileMetadataTailScriptRow* row =
            serialized_file_metadata_tail_script(tail, ordinal);
        if (!row) {
            return false;
        }
        file->scripts[ordinal].file_id = serialized_metadata_signed32(row->file_index_bits);
        file->scripts[ordinal].path_id = serialized_metadata_signed64(row->local_identifier_bits);
    }
    return true;
}

static bool materialize_externals(SerializedFile* file,
    const SerializedFileMetadataTail* tail,
    const SerializedFileMetadataTailView* view) {
    file->external_count = (int)view->external_count;
    if (!allocate_zeroed_array(
            (void**)&file->externals, view->external_count, sizeof(*file->externals))) {
        return false;
    }
    for (size_t ordinal = 0U; ordinal < view->external_count; ++ordinal) {
        const SerializedFileMetadataTailExternalRow* row =
            serialized_file_metadata_tail_external(tail, ordinal);
        if (!row) {
            return false;
        }
        AssetFileExternal* external = &file->externals[ordinal];
        external->virtual_path =
            serialized_metadata_copy_terminated_string(row->leading_string_source);
        if (!external->virtual_path) {
            return false;
        }
        /* Preserve this API's original GUID byte array and raw type/path
         * representation. The Editor's separate normalization is not applied. */
        memcpy(external->guid, row->guid_source.data, sizeof(external->guid));
        external->type = serialized_metadata_signed32(row->type_bits);
        external->path_name = serialized_metadata_copy_terminated_string(row->path_source);
        if (!external->path_name) {
            return false;
        }
    }
    return true;
}

static bool materialize_reference_types(SerializedFile* file,
    const SerializedFileMetadataTail* tail,
    const SerializedFileMetadataTailView* view) {
    file->ref_type_count = (int)view->reference_type_count;
    if (!allocate_zeroed_array(
            (void**)&file->ref_types, view->reference_type_count, sizeof(*file->ref_types))) {
        return false;
    }
    for (size_t ordinal = 0U; ordinal < view->reference_type_count; ++ordinal) {
        const SerializedFileMetadataTailReferenceTypeRow* row =
            serialized_file_metadata_tail_reference_type(tail, ordinal);
        if (!typetree_materialize_metadata_tail_reference_type(
                &file->ref_types[ordinal], row, file->big_endian)) {
            LOG_ERROR("Failed to materialize reference TypeTree row %zu", ordinal);
            return false;
        }
    }
    return true;
}

static bool materialize_tail(SerializedFile* file, const SerializedFileMetadataTail* tail) {
    const SerializedFileMetadataTailView* view = serialized_file_metadata_tail_view(tail);
    if (!view || view->script_count > INT_MAX || view->external_count > INT_MAX ||
        view->reference_type_count > INT_MAX) {
        return false;
    }
    if (!materialize_scripts(file, tail, view) || !materialize_externals(file, tail, view) ||
        !materialize_reference_types(file, tail, view)) {
        return false;
    }
    file->user_information =
        serialized_metadata_copy_terminated_string(view->user_information_source);
    return file->user_information != NULL;
}

static bool read_metadata(SerializedFile* file, const uint8_t* file_data, size_t file_size) {
    const SerializedFileDirectoryLimits limits = {DIRECTORY_ADAPTER_VERSION_BYTES,
        DIRECTORY_ADAPTER_MAX_TYPES,
        DIRECTORY_ADAPTER_MAX_OBJECTS,
        DIRECTORY_ADAPTER_MAX_NODES,
        DIRECTORY_ADAPTER_MAX_STRING_BYTES,
        DIRECTORY_ADAPTER_MAX_DEPENDENCIES,
        DIRECTORY_ADAPTER_MAX_METADATA_BYTES,
        DIRECTORY_ADAPTER_MAX_RETAINED_BYTES,
        0U,
        DIRECTORY_ADAPTER_MAX_WORK};
    SerializedFileDirectory directory;
    serialized_file_directory_init(&directory);
    SerializedFileMetadataTail tail;
    serialized_file_metadata_tail_init(&tail);
    SerializedFileDirectoryResult result = serialized_file_directory_create(file_data,
        file_size,
        file_size,
        SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
        &limits,
        &directory);
    if (result.status == SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION) {
        /* Only the explicit alternate engine profile is retried. The first
         * attempt stops before scanning or allocating; both attempts are
         * independently capped, so their total work is at most twice policy. */
        result = serialized_file_directory_create(file_data,
            file_size,
            file_size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1,
            &limits,
            &directory);
    }
    bool valid = false;
    if (result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        LOG_ERROR("SerializedFile ordinary directory rejected (status %d, field %d)",
            (int)result.status,
            (int)result.field);
        goto cleanup;
    }
    if (!materialize_directory(file, &directory)) {
        goto cleanup;
    }
    const SerializedFileMetadataTailLimits tail_limits = {TAIL_ADAPTER_MAX_SCRIPTS,
        TAIL_ADAPTER_MAX_EXTERNALS,
        TAIL_ADAPTER_MAX_REFERENCE_TYPES,
        TAIL_ADAPTER_MAX_NODES,
        TAIL_ADAPTER_MAX_TREE_STRING_BYTES,
        TAIL_ADAPTER_MAX_TERMINATED_STRING_BYTES,
        TAIL_ADAPTER_MAX_TAIL_BYTES,
        TAIL_ADAPTER_MAX_RETAINED_BYTES,
        0U,
        TAIL_ADAPTER_MAX_WORK};
    const SerializedFileMetadataTailResult tail_result = serialized_file_metadata_tail_create(
        &directory, file_data, file_size, file_size, &tail_limits, &tail);
    /* Tail owns independent descriptors and has copied the Directory view. */
    serialized_file_directory_dispose(&directory);
    if (tail_result.status != SERIALIZED_FILE_METADATA_TAIL_OK) {
        LOG_ERROR("SerializedFile metadata tail rejected (status %d, field %d)",
            (int)tail_result.status,
            (int)tail_result.field);
        goto cleanup;
    }
    valid = materialize_tail(file, &tail);

cleanup:
    serialized_file_metadata_tail_dispose(&tail);
    serialized_file_directory_dispose(&directory);
    return valid;
}

static bool serialized_file_open_internal(
    SerializedFile* file, const uint8_t* file_data, size_t file_size,
    TypeTreeSchemaRegistry* schema_registry,
    TypeTreeSchemaProvenance input_provenance,
    bool permit_unresolved_schemas) {
    if (!file || !file_data || file_size < 20u) return false;
    if (input_provenance != TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT &&
        input_provenance != TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) {
        return false;
    }
    memset(file, 0, sizeof(SerializedFile));
    file->raw_data = file_data;
    file->raw_size = file_size;

    /* Complete physical tail admission and semantic materialization precede
     * object uniqueness/range checks. Registry publication remains last. */
    if (!read_metadata(file, file_data, file_size) || !validate_object_table(file)) {
        goto fail;
    }

    if (!permit_unresolved_schemas &&
        !apply_schema_registry(file, schema_registry,
                               input_provenance)) goto fail;
    
    return true;

fail:
    serialized_file_close(file);
    return false;
}

bool serialized_file_open_with_schema_registry_ex(
    SerializedFile* file, const uint8_t* file_data, size_t file_size,
    TypeTreeSchemaRegistry* schema_registry,
    TypeTreeSchemaProvenance input_provenance) {
    return serialized_file_open_internal(
        file, file_data, file_size, schema_registry, input_provenance, false);
}

bool serialized_file_open_metadata(SerializedFile* file,
                                   const uint8_t* file_data,
                                   size_t file_size) {
    return serialized_file_open_internal(
        file, file_data, file_size, NULL,
        TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT, true);
}

bool serialized_file_open(SerializedFile* file, const uint8_t* file_data,
                          size_t file_size) {
    return serialized_file_open_with_schema_registry(
        file, file_data, file_size, NULL);
}

bool serialized_file_open_with_schema_registry(
    SerializedFile* file, const uint8_t* file_data, size_t file_size,
    TypeTreeSchemaRegistry* schema_registry) {
    return serialized_file_open_with_schema_registry_ex(
        file, file_data, file_size, schema_registry,
        TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT);
}

void serialized_file_close(SerializedFile* file) {
    if (file->unity_version) {
        mem_free(file->unity_version, strlen(file->unity_version) + 1);
        file->unity_version = NULL;
    }
    if (file->types) {
        for (int i = 0; i < file->type_count; i++) {
            typetree_free_type(&file->types[i]);
        }
        mem_free(file->types, file->type_count * sizeof(TypeTreeType));
        file->types = NULL;
    }
    if (file->objects) {
        mem_free(file->objects, file->object_count * sizeof(AssetObjectInfo));
        file->objects = NULL;
    }
    if (file->scripts) {
        mem_free(file->scripts, file->script_count * sizeof(AssetPPtr));
        file->scripts = NULL;
    }
    if (file->externals) {
        for (int i = 0; i < file->external_count; i++) {
            if (file->externals[i].virtual_path) {
                mem_free(file->externals[i].virtual_path, strlen(file->externals[i].virtual_path) + 1);
            }
            if (file->externals[i].path_name) {
                mem_free(file->externals[i].path_name, strlen(file->externals[i].path_name) + 1);
            }
        }
        mem_free(file->externals, file->external_count * sizeof(AssetFileExternal));
        file->externals = NULL;
    }
    if (file->ref_types) {
        for (int i = 0; i < file->ref_type_count; i++) {
            typetree_free_type(&file->ref_types[i]);
        }
        mem_free(file->ref_types, file->ref_type_count * sizeof(TypeTreeType));
        file->ref_types = NULL;
    }
    if (file->user_information) {
        mem_free(file->user_information, strlen(file->user_information) + 1);
        file->user_information = NULL;
    }
}

const AssetObjectInfo* serialized_file_get_object(SerializedFile* file, int64_t path_id) {
    if (!file) return NULL;
    for (int i = 0; i < file->object_count; i++) {
        if (file->objects[i].path_id == path_id) {
            return &file->objects[i];
        }
    }
    return NULL;
}

const uint8_t* serialized_file_get_object_data(SerializedFile* file, const AssetObjectInfo* obj, size_t* out_size) {
    if (!file || !obj) return NULL;
    if (obj->byte_offset > UINT64_MAX - file->data_offset) {
        LOG_ERROR("Object absolute offset overflow");
        return NULL;
    }
    uint64_t abs_offset = file->data_offset + obj->byte_offset;
    if (abs_offset > file->file_size ||
        obj->byte_size > file->file_size - abs_offset) {
        LOG_ERROR("Object offset/size out of declared file bounds");
        return NULL;
    }
    if (out_size) {
        *out_size = obj->byte_size;
    }
    return file->raw_data + abs_offset;
}
