// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_managed_values.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_managed_profile_internal.h"
#include "serialized_file_metadata_tail_internal.h"
#include "serialized_metadata_reader_internal.h"
#include "typetree_common_strings_internal.h"

#include "common/common.h"

#include <string.h>

enum {
    MANAGED_ALIGNMENT = 4U,
    MANAGED_I32_BYTES = 4U,
    MANAGED_OBJECT_BYTE_COUNT_OFFSET = 16U,
    MANAGED_ALIGNMENT_FLAG = 0x4000U,
    MANAGED_REGISTRY_VERSION = 2U
};

typedef struct ManagedSchema {
    SerializedFileSchema owner;
    SerializedFileManagedSelectedType selected;
    const ManagedProfile* profile;
    size_t retained_bytes;
} ManagedSchema;

typedef struct ManagedStorage {
    SerializedFileManagedValuesView view;
    size_t values_offset;
    size_t types_offset;
    size_t rows_offset;
    size_t slots_offset;
} ManagedStorage;

typedef struct ManagedBuild {
    const SerializedFileDirectory* directory;
    const SerializedFileMetadataTail* tail;
    const SerializedFileManagedValuesLimits* limits;
    const uint8_t* bytes;
    SerializedFileManagedValuesResult result;
    SerializedFileManagedValuesView view;
    ManagedSchema host;
    SerializedFileReferenceIndex index;
    ManagedSchema* types;
    size_t type_capacity;
    size_t type_count;
    size_t cache_bytes;
    size_t prerequisite_bytes;
    ManagedStorage* storage;
} ManagedBuild;

typedef struct ManagedCursor {
    uint64_t position;
    uint64_t end;
    uint64_t string_bytes;
    uint64_t scalar_bytes;
    uint64_t padding_bytes;
    size_t values;
    size_t rows;
    size_t slots;
    size_t registry_row;
    const ManagedSchema* schema;
} ManagedCursor;

static SerializedFilePrefixSpan absent_span(void) {
    const SerializedFilePrefixSpan span = {NULL, UINT64_MAX, 0U};
    return span;
}

static bool reject(ManagedBuild* build,
    SerializedFileManagedValuesStatus status,
    SerializedFileManagedValuesLimit limit,
    SerializedFileManagedValuesField field,
    size_t node,
    uint64_t offset) {
    build->result.status = status;
    build->result.limit = limit;
    build->result.field = field;
    build->result.node_ordinal = node;
    build->result.error_offset = offset;
    return false;
}

static bool reject_owner(ManagedBuild* build,
    SerializedFileManagedValuesStatus status,
    SerializedFileManagedValuesLimit limit) {
    return reject(
        build, status, limit, SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
}

static bool charge(ManagedBuild* build,
    uint64_t amount,
    SerializedFileManagedValuesField field,
    size_t node,
    uint64_t offset) {
    if (amount > build->limits->max_work - build->result.work_used) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK,
            field,
            node,
            offset);
    }
    build->result.work_used += amount;
    return true;
}

static uint64_t remaining_work(const ManagedBuild* build, uint64_t child_cap) {
    const uint64_t remaining = build->limits->max_work - build->result.work_used;
    return child_cap < remaining ? child_cap : remaining;
}

static bool cap_size(ManagedBuild* build,
    size_t value,
    size_t maximum,
    SerializedFileManagedValuesLimit limit,
    SerializedFileManagedValuesField field,
    size_t node,
    uint64_t offset) {
    return value <= maximum ||
        reject(build, SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED, limit, field, node, offset);
}

static bool same_span(SerializedFilePrefixSpan first, SerializedFilePrefixSpan second) {
    return first.data == second.data && first.offset == second.offset && first.size == second.size;
}

static bool disjoint_ranges(const void* const* ranges, const size_t* sizes, size_t count) {
    for (size_t first = 0U; first < count; ++first) {
        if (sizes[first] != 0U &&
            (!ranges[first] || sizes[first] > UINTPTR_MAX - (uintptr_t)ranges[first])) {
            return false;
        }
        for (size_t second = first + 1U; second < count; ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool admit_arguments(
    ManagedBuild* build, size_t mapped_size, const SerializedFileManagedValues* output) {
    if (!build->directory || !build->tail || !build->limits || !build->bytes || !output) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    const void* const handles[] = {build->directory, build->tail, build->limits, output};
    const size_t handle_sizes[] = {
        sizeof(*build->directory), sizeof(*build->tail), sizeof(*build->limits), sizeof(*output)};
    if (!disjoint_ranges(handles, handle_sizes, sizeof(handles) / sizeof(handles[0]))) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    const void* directory_storage;
    const void* tail_storage;
    size_t directory_size;
    size_t tail_size;
    if (!serialized_file_directory_storage_range_internal(
            build->directory, &directory_storage, &directory_size) ||
        !serialized_file_metadata_tail_storage_range_internal(
            build->tail, &tail_storage, &tail_size)) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_STATE,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    const SerializedFileDirectoryView* directory = serialized_file_directory_view(build->directory);
    const SerializedFileMetadataTailView* tail = serialized_file_metadata_tail_view(build->tail);
    size_t common_size;
    const uint8_t* common = typetree_common_string_table(&common_size);
    size_t directory_known =
        (size_t)(directory->parsed_metadata_source.offset + directory->parsed_metadata_source.size);
    const size_t tail_known = (size_t)(tail->source.offset + tail->source.size);
    const uint8_t* other = tail->directory.prefix.header.header_source.data;
    size_t other_size = tail_known;
    if (other == directory->prefix.header.header_source.data) {
        if (tail_known > directory_known) {
            directory_known = tail_known;
        }
        other = NULL;
        other_size = 0U;
    }
    const void* const ranges[] = {build->directory,
        build->tail,
        build->limits,
        output,
        directory_storage,
        tail_storage,
        directory->prefix.header.header_source.data,
        other,
        common};
    const size_t sizes[] = {sizeof(*build->directory),
        sizeof(*build->tail),
        sizeof(*build->limits),
        sizeof(*output),
        directory_size,
        tail_size,
        directory_known,
        other_size,
        common_size};
    if (!disjoint_ranges(ranges, sizes, sizeof(ranges) / sizeof(ranges[0]))) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    if (mapped_size > UINTPTR_MAX - (uintptr_t)build->bytes) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    for (size_t index = 0U; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
        if (index != 6U && index != 7U &&
            serialized_file_storage_overlaps_internal(
                build->bytes, mapped_size, ranges[index], sizes[index])) {
            return reject_owner(build,
                SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
        }
    }
    if (output->implementation) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_STATE,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    return true;
}

static bool admit_source(ManagedBuild* build, size_t object_ordinal, size_t mapped_size) {
    const SerializedFileDirectoryView* directory = serialized_file_directory_view(build->directory);
    const SerializedFileMetadataTailView* tail = serialized_file_metadata_tail_view(build->tail);
    if (directory->engine_version != SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENGINE,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    if (directory->prefix.header.endian_selector != 0U) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENDIAN,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    if (!same_span(
            directory->prefix.header.header_source, tail->directory.prefix.header.header_source) ||
        directory->prefix.header.file_size != tail->directory.prefix.header.file_size ||
        directory->prefix.header.endian_selector != tail->directory.prefix.header.endian_selector ||
        directory->engine_version != tail->directory.engine_version ||
        !same_span(directory->parsed_metadata_source, tail->directory.parsed_metadata_source) ||
        build->bytes != directory->prefix.header.header_source.data ||
        mapped_size > directory->prefix.header.file_size) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_SOURCE_MISMATCH,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    const SerializedFileDirectoryObjectRow* object =
        serialized_file_directory_object(build->directory, object_ordinal);
    if (!object) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    build->result.object_ordinal = object_ordinal;
    build->result.type_ordinal = object->type_ordinal;
    const SerializedFileDirectoryTypeRow* type =
        serialized_file_directory_type(build->directory, object->type_ordinal);
    if (!type->has_tree) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_NO_EMBEDDED_SCHEMA,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_TYPE,
            SIZE_MAX,
            type->source.offset);
    }
    if (object->payload.size > INT32_MAX) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_OBJECT_SIZE,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            object->source.offset + MANAGED_OBJECT_BYTE_COUNT_OFFSET);
    }
    if (object->payload.offset + object->payload.size > mapped_size) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_INCOMPLETE_MAPPING,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            mapped_size);
    }
    if (object->payload.offset % MANAGED_ALIGNMENT != 0U) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_OBJECT_ALIGNMENT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            object->payload.offset);
    }
    if (object->payload.size > build->limits->max_payload_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PAYLOAD_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            object->payload.offset);
    }
    build->view.object = *object;
    build->view.payload_source =
        (SerializedFilePrefixSpan){build->bytes + (size_t)object->payload.offset,
            object->payload.offset,
            (size_t)object->payload.size};
    build->view.registry_padding_source = absent_span();
    return true;
}

static bool reserve_memory(ManagedBuild* build) {
    const size_t prerequisite = build->limits->max_prerequisite_bytes;
    const size_t other = build->limits->max_schema_scratch_bytes > build->limits->max_retained_bytes
        ? build->limits->max_schema_scratch_bytes
        : build->limits->max_retained_bytes;
    if (other > SIZE_MAX - prerequisite) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES);
    }
    build->result.reserved_heap_bytes = prerequisite + other;
    return cap_size(build,
        prerequisite + other,
        build->limits->max_heap_bytes,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX);
}

static void note_prerequisite_peak(ManagedBuild* build, size_t incoming) {
    const size_t total = build->prerequisite_bytes + incoming;
    if (total > build->result.peak_prerequisite_bytes) {
        build->result.peak_prerequisite_bytes = total;
    }
}

static bool qualify_schema(
    ManagedBuild* build, ManagedSchema* schema, SerializedFileSchemaContextKind kind) {
    build->result.context_attempted = true;
    build->result.context_result = serialized_file_schema_context_query(
        &schema->owner, kind, remaining_work(build, UINT64_MAX), &schema->selected.context);
    build->result.work_used += build->result.context_result.work_used;
    if (build->result.context_result.status != SERIALIZED_FILE_SCHEMA_CONTEXT_OK) {
        const bool work =
            build->result.context_result.status == SERIALIZED_FILE_SCHEMA_CONTEXT_WORK_LIMIT;
        return reject(build,
            work ? SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED
                 : SERIALIZED_FILE_MANAGED_VALUES_CONTEXT_REJECTED,
            work ? SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK
                 : SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            build->result.context_result.node_ordinal,
            build->result.context_result.error_offset);
    }
    const ManagedProfileResult profile = serialized_file_managed_profile_qualify(
        &schema->owner, kind, remaining_work(build, UINT64_MAX), &schema->profile);
    build->result.work_used += profile.work_used;
    if (profile.status != SERIALIZED_FILE_MANAGED_VALUES_OK) {
        return reject(build,
            profile.status,
            profile.status == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED
                ? SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK
                : SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            profile.field,
            profile.node_ordinal,
            profile.error_offset);
    }
    return true;
}

static bool create_schema(
    ManagedBuild* build, size_t ordinal, bool reference, ManagedSchema* schema) {
    build->result.type_ordinal = ordinal;
    SerializedFileSchemaLimits limits = build->limits->schema;
    const size_t remaining = build->limits->max_prerequisite_bytes - build->prerequisite_bytes;
    const bool outer_retained = remaining <= limits.max_retained_bytes;
    const bool outer_scratch = build->limits->max_schema_scratch_bytes <= limits.max_scratch_bytes;
    const bool outer_work = build->limits->max_work - build->result.work_used <= limits.max_work;
    if (limits.max_retained_bytes > remaining) {
        limits.max_retained_bytes = remaining;
    }
    if (limits.max_scratch_bytes > build->limits->max_schema_scratch_bytes) {
        limits.max_scratch_bytes = build->limits->max_schema_scratch_bytes;
    }
    limits.max_work = remaining_work(build, limits.max_work);
    build->result.schema_attempted = true;
    build->result.schema_result = reference
        ? serialized_file_schema_create_reference(build->tail, ordinal, &limits, &schema->owner)
        : serialized_file_schema_create_ordinary(
              build->directory, ordinal, &limits, &schema->owner);
    const SerializedFileSchemaResult* nested = &build->result.schema_result;
    build->result.work_used += nested->work_used;
    note_prerequisite_peak(build, nested->peak_retained_bytes);
    if (nested->peak_scratch_bytes > build->result.peak_schema_scratch_bytes) {
        build->result.peak_schema_scratch_bytes = nested->peak_scratch_bytes;
    }
    if (nested->status != SERIALIZED_FILE_SCHEMA_OK) {
        SerializedFileManagedValuesLimit limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE;
        if (nested->status == SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED) {
            if (nested->limit == SERIALIZED_FILE_SCHEMA_LIMIT_RETAINED && outer_retained) {
                limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES;
            } else if (nested->limit == SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH && outer_scratch) {
                limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCHEMA_SCRATCH_BYTES;
            } else if (nested->limit == SERIALIZED_FILE_SCHEMA_LIMIT_WORK && outer_work) {
                limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK;
            }
        }
        /* A stricter child cap remains a nested schema refusal. Equal caps
         * identify the enclosing bound while retaining every child diagnostic. */
        const SerializedFileManagedValuesStatus status =
            limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE
            ? SERIALIZED_FILE_MANAGED_VALUES_SCHEMA_REJECTED
            : SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED;
        return reject(build,
            status,
            limit,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            nested->node_ordinal,
            nested->error_space == SERIALIZED_FILE_SCHEMA_ERROR_FILE ? nested->error_offset
                                                                     : UINT64_MAX);
    }
    schema->selected.schema = *serialized_file_schema_view(&schema->owner);
    schema->retained_bytes = schema->selected.schema.retained_bytes;
    build->prerequisite_bytes += schema->retained_bytes;
    const SerializedFileSchemaContextKind kind = reference
        ? SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD
        : SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT;
    return qualify_schema(build, schema, kind);
}

static bool create_index_and_cache(ManagedBuild* build) {
    SerializedFileReferenceIndexLimits limits = build->limits->index;
    const size_t remaining = build->limits->max_prerequisite_bytes - build->prerequisite_bytes;
    const bool outer_retained = remaining <= limits.max_retained_bytes;
    const bool outer_work = build->limits->max_work - build->result.work_used <= limits.max_work;
    if (limits.max_retained_bytes > remaining) {
        limits.max_retained_bytes = remaining;
    }
    limits.max_work = remaining_work(build, limits.max_work);
    build->result.index_attempted = true;
    build->result.index_result =
        serialized_file_reference_index_create(build->tail, &limits, &build->index);
    build->result.work_used += build->result.index_result.work_used;
    note_prerequisite_peak(build, build->result.index_result.peak_retained_bytes);
    if (build->result.index_result.status != SERIALIZED_FILE_REFERENCE_INDEX_OK) {
        SerializedFileManagedValuesLimit limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE;
        if (build->result.index_result.status == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED) {
            if (build->result.index_result.limit ==
                    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_RETAINED_BYTES &&
                outer_retained) {
                limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES;
            } else if (build->result.index_result.limit ==
                    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK &&
                outer_work) {
                limit = SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK;
            }
        }
        const SerializedFileManagedValuesStatus status =
            limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE
            ? SERIALIZED_FILE_MANAGED_VALUES_INDEX_REJECTED
            : SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED;
        return reject_owner(build, status, limit);
    }
    const SerializedFileReferenceIndexView* index =
        serialized_file_reference_index_view(&build->index);
    build->prerequisite_bytes += index->retained_bytes;
    if (index->reference_type_count == 0U) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_EMPTY_REFERENCE_INDEX,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    build->type_capacity = index->reference_type_count < build->limits->max_selected_types
        ? index->reference_type_count
        : build->limits->max_selected_types;
    if (build->type_capacity > SIZE_MAX / sizeof(*build->types)) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES);
    }
    build->cache_bytes = build->type_capacity * sizeof(*build->types);
    if (!cap_size(build,
            build->cache_bytes,
            build->limits->max_prerequisite_bytes - build->prerequisite_bytes,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE,
            SIZE_MAX,
            UINT64_MAX) ||
        !charge(build,
            build->cache_bytes,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE,
            SIZE_MAX,
            UINT64_MAX)) {
        return false;
    }
    if (build->cache_bytes != 0U) {
        build->types = mem_alloc(build->cache_bytes);
        if (!build->types) {
            return reject_owner(build,
                SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_FAILED,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
        }
        note_prerequisite_peak(build, build->cache_bytes);
        build->prerequisite_bytes += build->cache_bytes;
        memset(build->types, 0, build->cache_bytes);
    }
    return true;
}

static bool select_schema(ManagedBuild* build, size_t original_ordinal, size_t* out_ordinal) {
    for (size_t index = 0U; index < build->type_count; ++index) {
        if (!charge(build,
                1U,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                SIZE_MAX,
                UINT64_MAX)) {
            return false;
        }
        if (build->types[index].selected.original.ordinal == original_ordinal) {
            *out_ordinal = index;
            return true;
        }
    }
    if (build->storage) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    if (build->type_count == build->type_capacity) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SELECTED_TYPES);
    }
    ManagedSchema* schema = &build->types[build->type_count];
    schema->selected.ordinal = build->type_count;
    schema->selected.original =
        *serialized_file_reference_index_row(&build->index, original_ordinal);
    /* Include an in-progress owner in the single cleanup path even if its
     * schema compiles but its context/profile is subsequently refused. */
    ++build->type_count;
    if (!create_schema(build, original_ordinal, true, schema)) {
        return false;
    }
    *out_ordinal = schema->selected.ordinal;
    return true;
}

static SerializedFileManagedValue* values_array(ManagedStorage* storage) {
    return (SerializedFileManagedValue*)((uint8_t*)storage + storage->values_offset);
}

static SerializedFileManagedRegistryRow* rows_array(ManagedStorage* storage) {
    return (SerializedFileManagedRegistryRow*)((uint8_t*)storage + storage->rows_offset);
}

static SerializedFileManagedRidSlot* slots_array(ManagedStorage* storage) {
    return (SerializedFileManagedRidSlot*)((uint8_t*)storage + storage->slots_offset);
}

static SerializedFileManagedSelectedType* types_array(ManagedStorage* storage) {
    return (SerializedFileManagedSelectedType*)((uint8_t*)storage + storage->types_offset);
}

static bool take_span(ManagedBuild* build,
    ManagedCursor* cursor,
    size_t width,
    SerializedFileManagedValuesField field,
    size_t node,
    SerializedFilePrefixSpan* out_span) {
    if (width > cursor->end - cursor->position) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            field,
            node,
            cursor->end);
    }
    if (!charge(build, (uint64_t)width + 1U, field, node, cursor->position)) {
        return false;
    }
    *out_span = (SerializedFilePrefixSpan){
        build->bytes + (size_t)cursor->position, cursor->position, width};
    cursor->position += width;
    return true;
}

static bool begin_value(ManagedBuild* build,
    const ManagedCursor* cursor,
    size_t node,
    SerializedFileManagedValueKind kind,
    SerializedFileManagedValue* value) {
    if (cursor->values == build->limits->max_values) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_VALUES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            node,
            cursor->position);
    }
    const SerializedFileManagedValue initial = {.ordinal = cursor->values,
        .kind = kind,
        .row_kind = cursor->schema->selected.schema.row_kind,
        .type_ordinal = cursor->schema->selected.schema.type_ordinal,
        .registry_row_ordinal = cursor->registry_row,
        .schema_node = *serialized_file_schema_node(&cursor->schema->owner, node),
        .source = {NULL, UINT64_MAX, 0U},
        .scalar_source = {NULL, UINT64_MAX, 0U},
        .length_source = {NULL, UINT64_MAX, 0U},
        .bytes_source = {NULL, UINT64_MAX, 0U},
        .padding_source = {NULL, UINT64_MAX, 0U},
        .array_schema_ordinal = SIZE_MAX,
        .size_schema_ordinal = SIZE_MAX,
        .data_schema_ordinal = SIZE_MAX,
        .array_schema_source = {NULL, UINT64_MAX, 0U},
        .size_schema_source = {NULL, UINT64_MAX, 0U},
        .data_schema_source = {NULL, UINT64_MAX, 0U}};
    *value = initial;
    build->result.type_ordinal = initial.type_ordinal;
    return true;
}

static bool read_padding(
    ManagedBuild* build, ManagedCursor* cursor, size_t node, SerializedFilePrefixSpan* span) {
    const size_t width =
        (size_t)((MANAGED_ALIGNMENT - cursor->position % MANAGED_ALIGNMENT) % MANAGED_ALIGNMENT);
    if (width > build->limits->max_padding_bytes - cursor->padding_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PADDING_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_PADDING,
            node,
            cursor->position);
    }
    cursor->padding_bytes += width;
    return take_span(
        build, cursor, width, SERIALIZED_FILE_MANAGED_VALUES_FIELD_PADDING, node, span);
}

static bool complete_value(
    ManagedBuild* build, ManagedCursor* cursor, uint64_t start, SerializedFileManagedValue* value) {
    value->source = (SerializedFilePrefixSpan){
        build->bytes + (size_t)start, start, (size_t)(cursor->position - start)};
    if (!charge(build,
            1U,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            value->schema_node.ordinal,
            start)) {
        return false;
    }
    if (build->storage) {
        if (cursor->values >= build->storage->view.value_count) {
            return reject(build,
                SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
                value->schema_node.ordinal,
                start);
        }
        values_array(build->storage)[cursor->values] = *value;
    }
    ++cursor->values;
    return true;
}

static bool read_scalar(ManagedBuild* build,
    ManagedCursor* cursor,
    size_t node,
    SerializedFileManagedValueKind kind,
    SerializedFileManagedValue* out_value) {
    SerializedFileManagedValue value;
    if (!begin_value(build, cursor, node, kind, &value)) {
        return false;
    }
    const size_t width = value.schema_node.byte_size_bits;
    const uint64_t start = cursor->position;
    if (width > build->limits->max_scalar_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCALAR_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
            node,
            start);
    }
    if (width > build->limits->max_total_scalar_bytes - cursor->scalar_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_SCALAR_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
            node,
            start);
    }
    cursor->scalar_bytes += width;
    if (!take_span(build,
            cursor,
            width,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
            node,
            &value.scalar_source)) {
        return false;
    }
    /* Complete profile admission restricts scalar widths to 1, 4 and 8.
     * Float32 uses the same four raw bytes, without any floating conversion. */
    if (width == 1U) {
        value.raw_bits = value.scalar_source.data[0];
    } else if (width == MANAGED_I32_BYTES) {
        value.raw_bits = serialized_metadata_decode_u32(value.scalar_source.data, 0U);
    } else {
        value.raw_bits = serialized_metadata_decode_u64(value.scalar_source.data, 0U);
    }
    if ((value.schema_node.meta_flags & MANAGED_ALIGNMENT_FLAG) != 0U &&
        !read_padding(build, cursor, node, &value.padding_source)) {
        return false;
    }
    if (!complete_value(build, cursor, start, &value)) {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool read_string(ManagedBuild* build,
    ManagedCursor* cursor,
    size_t node,
    SerializedFileManagedValue* out_value) {
    SerializedFileManagedValue value;
    if (!begin_value(build, cursor, node, SERIALIZED_FILE_MANAGED_VALUE_BYTE_STRING, &value)) {
        return false;
    }
    value.array_schema_ordinal = node + 1U;
    value.size_schema_ordinal = node + 2U;
    value.data_schema_ordinal = node + 3U;
    value.array_schema_source =
        serialized_file_schema_node(&cursor->schema->owner, node + 1U)->source;
    value.size_schema_source =
        serialized_file_schema_node(&cursor->schema->owner, node + 2U)->source;
    value.data_schema_source =
        serialized_file_schema_node(&cursor->schema->owner, node + 3U)->source;
    const uint64_t start = cursor->position;
    if (!take_span(build,
            cursor,
            MANAGED_I32_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_LENGTH,
            node + 2U,
            &value.length_source)) {
        return false;
    }
    value.length_bits = serialized_metadata_decode_u32(value.length_source.data, 0U);
    if (value.length_bits > INT32_MAX) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_STRING_LENGTH,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_LENGTH,
            node + 2U,
            start);
    }
    if (value.length_bits > build->limits->max_string_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_STRING_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_LENGTH,
            node + 2U,
            start);
    }
    if (value.length_bits > build->limits->max_total_string_bytes - cursor->string_bytes) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_STRING_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_LENGTH,
            node + 2U,
            start);
    }
    cursor->string_bytes += value.length_bits;
    if (!take_span(build,
            cursor,
            value.length_bits,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_BYTES,
            node + 3U,
            &value.bytes_source) ||
        !read_padding(build, cursor, node, &value.padding_source) ||
        !complete_value(build, cursor, start, &value)) {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool read_rid_slot(ManagedBuild* build, ManagedCursor* cursor, size_t node) {
    if (cursor->slots == build->limits->max_rid_slots) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RID_SLOTS,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
            node,
            cursor->position);
    }
    SerializedFileManagedValue value;
    if (!read_scalar(build, cursor, node, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, &value) ||
        !charge(
            build, 1U, SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR, node, value.source.offset)) {
        return false;
    }
    const SerializedFileManagedRidSlot slot = {.ordinal = cursor->slots,
        .value_ordinal = value.ordinal,
        .registry_row_ordinal = cursor->registry_row,
        .rid_bits = value.raw_bits,
        .resolution = SERIALIZED_FILE_MANAGED_RID_MISSING,
        .first_registry_row = SIZE_MAX,
        .second_registry_row = SIZE_MAX};
    if (build->storage) {
        if (cursor->slots >= build->storage->view.rid_slot_count) {
            return reject(build,
                SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
                node,
                value.source.offset);
        }
        slots_array(build->storage)[cursor->slots] = slot;
    }
    ++cursor->slots;
    return true;
}

static bool read_count(ManagedBuild* build,
    ManagedCursor* cursor,
    size_t node,
    size_t maximum,
    SerializedFileManagedValuesLimit limit,
    SerializedFileManagedValuesField field,
    uint32_t* count,
    size_t* value_ordinal) {
    SerializedFileManagedValue value;
    if (!read_scalar(build, cursor, node, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, &value)) {
        return false;
    }
    if (value.raw_bits > INT32_MAX) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_COUNT,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            field,
            node,
            value.source.offset);
    }
    if (!cap_size(
            build, (size_t)value.raw_bits, maximum, limit, field, node, value.source.offset)) {
        return false;
    }
    *count = (uint32_t)value.raw_bits;
    *value_ordinal = value.ordinal;
    return true;
}

static bool read_host_fields(ManagedBuild* build, ManagedCursor* cursor) {
    if (!read_scalar(build, cursor, 2U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, NULL) ||
        !read_scalar(build, cursor, 3U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, NULL) ||
        !read_scalar(build, cursor, 4U, SERIALIZED_FILE_MANAGED_VALUE_UNSIGNED_INTEGER, NULL) ||
        !read_scalar(build, cursor, 6U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, NULL) ||
        !read_scalar(build, cursor, 7U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, NULL) ||
        !read_string(build, cursor, 8U, NULL) || !read_string(build, cursor, 12U, NULL) ||
        !read_rid_slot(build, cursor, 17U) || !read_rid_slot(build, cursor, 19U)) {
        return false;
    }
    uint32_t count;
    if (!read_count(build,
            cursor,
            22U,
            build->limits->max_array_elements,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_ARRAY_ELEMENTS,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_ARRAY_COUNT,
            &count,
            &build->view.array_count_value)) {
        return false;
    }
    build->view.array_element_count = count;
    for (uint32_t index = 0U; index < count; ++index) {
        if (!read_rid_slot(build, cursor, 24U)) {
            return false;
        }
    }
    return true;
}

static bool read_selected_payload(
    ManagedBuild* build, ManagedCursor* cursor, size_t selected_ordinal) {
    cursor->schema = &build->types[selected_ordinal];
    if (!read_scalar(build, cursor, 1U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, NULL)) {
        return false;
    }
    if (cursor->schema->profile->kind == MANAGED_PROFILE_TEXT) {
        if (!read_string(build, cursor, 2U, NULL) || !read_rid_slot(build, cursor, 7U)) {
            return false;
        }
    } else if (!read_scalar(build, cursor, 2U, SERIALIZED_FILE_MANAGED_VALUE_FLOAT32, NULL) ||
        !read_rid_slot(build, cursor, 4U)) {
        return false;
    }
    cursor->schema = &build->host;
    build->result.type_ordinal = build->host.selected.schema.type_ordinal;
    return true;
}

static bool reject_terminator(ManagedBuild* build, const SerializedFileManagedValue identity[3]) {
    static const char* const names[3] = {"Terminus", "UnityEngine.DMAT", "FAKE_ASM"};
    static const size_t lengths[3] = {8U, 16U, 8U};
    for (size_t component = 0U; component < 3U; ++component) {
        if (!charge(build,
                1U,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                identity[component].schema_node.ordinal,
                identity[component].length_source.offset)) {
            return false;
        }
        if (identity[component].length_bits != lengths[component]) {
            return true;
        }
        if (!charge(build,
                lengths[component],
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                identity[component].schema_node.ordinal,
                identity[component].bytes_source.offset)) {
            return false;
        }
        if (memcmp(identity[component].bytes_source.data, names[component], lengths[component]) !=
            0) {
            return true;
        }
    }
    return reject(build,
        SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_TERMINATOR,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
        SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
        identity[0].schema_node.ordinal,
        identity[0].length_source.offset);
}

static bool query_row_type(ManagedBuild* build,
    const SerializedFileManagedValue identity[3],
    SerializedFileReferenceMatch* match) {
    const SerializedFileReferenceKey key = {
        {identity[0].bytes_source.data, identity[0].bytes_source.size},
        {identity[1].bytes_source.data, identity[1].bytes_source.size},
        {identity[2].bytes_source.data, identity[2].bytes_source.size}};
    SerializedFileReferenceQueryLimits limits = build->limits->query;
    const bool outer_work = build->limits->max_work - build->result.work_used <= limits.max_work;
    limits.max_work = remaining_work(build, limits.max_work);
    build->result.query_attempted = true;
    build->result.query_registry_row_ordinal = build->result.registry_row_ordinal;
    build->result.query_result =
        serialized_file_reference_index_query(&build->index, &key, &limits, match);
    build->result.work_used += build->result.query_result.work_used;
    if (build->result.query_result.status != SERIALIZED_FILE_REFERENCE_INDEX_OK) {
        const bool work = outer_work &&
            build->result.query_result.limit == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK;
        size_t component = 0U;
        if (build->result.query_result.component == SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE) {
            component = 1U;
        } else if (build->result.query_result.component ==
            SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY) {
            component = 2U;
        }
        return reject(build,
            work ? SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED
                 : SERIALIZED_FILE_MANAGED_VALUES_QUERY_REJECTED,
            work ? SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK
                 : SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
            identity[component].schema_node.ordinal,
            identity[component].length_source.offset);
    }
    build->result.type_match = *match;
    return true;
}

static bool read_registry_row(ManagedBuild* build, ManagedCursor* cursor) {
    cursor->registry_row = cursor->rows;
    build->result.registry_row_ordinal = cursor->rows;
    const uint64_t start = cursor->position;
    SerializedFileManagedRegistryRow row = {.ordinal = cursor->rows,
        .first_value = cursor->values,
        .selected_type_ordinal = SIZE_MAX,
        .type_match = {.first_ordinal = SIZE_MAX, .second_ordinal = SIZE_MAX}};
    SerializedFileManagedValue rid;
    SerializedFileManagedValue identity[3];
    if (!read_scalar(build, cursor, 31U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, &rid) ||
        !read_string(build, cursor, 33U, &identity[0]) ||
        !read_string(build, cursor, 37U, &identity[1]) ||
        !read_string(build, cursor, 41U, &identity[2])) {
        return false;
    }
    row.rid_bits = rid.raw_bits;
    row.rid_value_ordinal = rid.ordinal;
    for (size_t component = 0U; component < 3U; ++component) {
        row.identity_value_ordinals[component] = identity[component].ordinal;
    }
    row.first_payload_value = cursor->values;
    const uint64_t payload_start = cursor->position;
    if (!reject_terminator(build, identity) || !query_row_type(build, identity, &row.type_match)) {
        return false;
    }
    const bool empty_identity = identity[0].length_bits == 0U && identity[1].length_bits == 0U &&
        identity[2].length_bits == 0U;
    if (row.rid_bits == UINT64_MAX - 1U && empty_identity &&
        row.type_match.kind == SERIALIZED_FILE_REFERENCE_MISSING) {
        row.kind = SERIALIZED_FILE_MANAGED_REGISTRY_NULL;
    } else {
        if (empty_identity && row.type_match.kind == SERIALIZED_FILE_REFERENCE_MISSING) {
            return reject(build,
                SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_NULL_ROW,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                31U,
                rid.source.offset);
        }
        if (row.type_match.kind != SERIALIZED_FILE_REFERENCE_UNIQUE) {
            return reject(build,
                row.type_match.kind == SERIALIZED_FILE_REFERENCE_MISSING
                    ? SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_MISSING
                    : SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_AMBIGUOUS,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                33U,
                identity[0].length_source.offset);
        }
        row.kind = SERIALIZED_FILE_MANAGED_REGISTRY_SELECTED_PAYLOAD;
        if (!select_schema(build, row.type_match.first_ordinal, &row.selected_type_ordinal) ||
            !read_selected_payload(build, cursor, row.selected_type_ordinal)) {
            return false;
        }
    }
    row.payload_source = (SerializedFilePrefixSpan){build->bytes + (size_t)payload_start,
        payload_start,
        (size_t)(cursor->position - payload_start)};
    row.source = (SerializedFilePrefixSpan){
        build->bytes + (size_t)start, start, (size_t)(cursor->position - start)};
    row.value_count = cursor->values - row.first_value;
    row.payload_value_count = cursor->values - row.first_payload_value;
    if (!charge(build, 1U, SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY, 30U, start)) {
        return false;
    }
    if (build->storage) {
        if (cursor->rows >= build->storage->view.registry_row_count) {
            return reject(build,
                SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR,
                SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
                30U,
                start);
        }
        rows_array(build->storage)[cursor->rows] = row;
    }
    ++cursor->rows;
    return true;
}

static bool replay_matches_plan(const ManagedBuild* build) {
    const SerializedFileManagedValuesView* planned = &build->storage->view;
    const SerializedFileManagedValuesView* replayed = &build->view;
    return same_span(planned->registry_source, replayed->registry_source) &&
        planned->registry_version_bits == replayed->registry_version_bits &&
        planned->registry_count_bits == replayed->registry_count_bits &&
        planned->registry_version_value == replayed->registry_version_value &&
        planned->registry_count_value == replayed->registry_count_value &&
        planned->array_count_value == replayed->array_count_value &&
        planned->array_element_count == replayed->array_element_count &&
        planned->value_count == replayed->value_count &&
        planned->registry_row_count == replayed->registry_row_count &&
        planned->rid_slot_count == replayed->rid_slot_count &&
        planned->selected_type_count == replayed->selected_type_count &&
        planned->consumed_bytes == replayed->consumed_bytes &&
        planned->string_bytes == replayed->string_bytes &&
        planned->scalar_bytes == replayed->scalar_bytes &&
        planned->padding_bytes == replayed->padding_bytes;
}

static bool parse_payload(ManagedBuild* build) {
    ManagedCursor cursor = {.position = build->view.payload_source.offset,
        .end = build->view.payload_source.offset + build->view.payload_source.size,
        .registry_row = SIZE_MAX,
        .schema = &build->host};
    build->result.registry_row_ordinal = SIZE_MAX;
    if (!read_host_fields(build, &cursor)) {
        return false;
    }
    const uint64_t registry_start = cursor.position;
    SerializedFileManagedValue version;
    if (!read_scalar(build, &cursor, 26U, SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER, &version)) {
        return false;
    }
    if (version.raw_bits != MANAGED_REGISTRY_VERSION) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_REGISTRY_VERSION,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_REGISTRY_VERSION,
            26U,
            version.source.offset);
    }
    build->view.registry_version_bits = (uint32_t)version.raw_bits;
    build->view.registry_version_value = version.ordinal;
    uint32_t count;
    if (!read_count(build,
            &cursor,
            29U,
            build->limits->max_registry_rows,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_REGISTRY_ROWS,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_REGISTRY_COUNT,
            &count,
            &build->view.registry_count_value)) {
        return false;
    }
    build->view.registry_count_bits = count;
    for (uint32_t row = 0U; row < count; ++row) {
        if (!read_registry_row(build, &cursor)) {
            return false;
        }
    }
    /* The special registry route returns to its original 0x8001 node.
     * Neither its RefIds Array's 0xc001 nor an omitted reference registry
     * contributes an additional size cell, traversal or alignment. */
    if (cursor.position != cursor.end) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_TRAILING_OBJECT_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT_END,
            SIZE_MAX,
            cursor.position);
    }
    build->view.registry_source = (SerializedFilePrefixSpan){build->bytes + (size_t)registry_start,
        registry_start,
        (size_t)(cursor.position - registry_start)};
    build->view.value_count = cursor.values;
    build->view.registry_row_count = cursor.rows;
    build->view.rid_slot_count = cursor.slots;
    build->view.selected_type_count = build->type_count;
    build->view.consumed_bytes = cursor.position - build->view.payload_source.offset;
    build->view.string_bytes = cursor.string_bytes;
    build->view.scalar_bytes = cursor.scalar_bytes;
    build->view.padding_bytes = cursor.padding_bytes;
    if (build->storage && !replay_matches_plan(build)) {
        return reject(build,
            SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT_END,
            SIZE_MAX,
            cursor.position);
    }
    return true;
}

static bool plan_array(ManagedBuild* build,
    size_t* size,
    size_t alignment,
    size_t count,
    size_t width,
    size_t* offset) {
    if (!charge(build, 1U, SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return false;
    }
    if (!serialized_file_storage_append_array_internal(size, alignment, count, width, offset)) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RETAINED_BYTES);
    }
    return true;
}

static bool allocate_values(ManagedBuild* build) {
    ManagedStorage layout = {.view = build->view};
    size_t size = sizeof(layout);
    if (!plan_array(build,
            &size,
            _Alignof(SerializedFileManagedValue),
            build->view.value_count,
            sizeof(SerializedFileManagedValue),
            &layout.values_offset) ||
        !plan_array(build,
            &size,
            _Alignof(SerializedFileManagedSelectedType),
            build->view.selected_type_count,
            sizeof(SerializedFileManagedSelectedType),
            &layout.types_offset) ||
        !plan_array(build,
            &size,
            _Alignof(SerializedFileManagedRegistryRow),
            build->view.registry_row_count,
            sizeof(SerializedFileManagedRegistryRow),
            &layout.rows_offset) ||
        !plan_array(build,
            &size,
            _Alignof(SerializedFileManagedRidSlot),
            build->view.rid_slot_count,
            sizeof(SerializedFileManagedRidSlot),
            &layout.slots_offset)) {
        return false;
    }
    build->result.required_retained_bytes = size;
    if (!cap_size(build,
            size,
            build->limits->max_retained_bytes,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RETAINED_BYTES,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE,
            SIZE_MAX,
            UINT64_MAX) ||
        !charge(build, size, SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return false;
    }
    build->storage = mem_alloc(size);
    if (!build->storage) {
        return reject_owner(build,
            SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_FAILED,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    }
    build->result.peak_retained_bytes = size;
    memset(build->storage, 0, size);
    layout.view.retained_bytes = size;
    *build->storage = layout;
    for (size_t index = 0U; index < build->type_count; ++index) {
        types_array(build->storage)[index] = build->types[index].selected;
    }
    return true;
}

static bool resolve_slots(ManagedBuild* build) {
    SerializedFileManagedRegistryRow* rows = rows_array(build->storage);
    SerializedFileManagedRidSlot* slots = slots_array(build->storage);
    for (size_t index = 0U; index < build->view.rid_slot_count; ++index) {
        SerializedFileManagedRidSlot* slot = &slots[index];
        const SerializedFileManagedValue* value =
            &values_array(build->storage)[slot->value_ordinal];
        build->result.registry_row_ordinal = slot->registry_row_ordinal;
        build->result.type_ordinal = value->type_ordinal;
        for (size_t row = 0U; row < build->view.registry_row_count; ++row) {
            if (!charge(build,
                    1U,
                    SERIALIZED_FILE_MANAGED_VALUES_FIELD_RID_RESOLUTION,
                    value->schema_node.ordinal,
                    value->scalar_source.offset)) {
                return false;
            }
            if (rows[row].rid_bits != slot->rid_bits) {
                continue;
            }
            if (slot->match_count == 0U) {
                slot->first_registry_row = row;
            } else if (slot->match_count == 1U) {
                slot->second_registry_row = row;
            }
            ++slot->match_count;
        }
        if (!charge(build,
                1U,
                SERIALIZED_FILE_MANAGED_VALUES_FIELD_RID_RESOLUTION,
                value->schema_node.ordinal,
                value->scalar_source.offset)) {
            return false;
        }
        if (slot->match_count == 0U) {
            ++build->view.missing_rid_slots;
        } else if (slot->match_count > 1U) {
            slot->resolution = SERIALIZED_FILE_MANAGED_RID_AMBIGUOUS;
            ++build->view.ambiguous_rid_slots;
        } else {
            slot->resolution =
                rows[slot->first_registry_row].kind == SERIALIZED_FILE_MANAGED_REGISTRY_NULL
                ? SERIALIZED_FILE_MANAGED_RID_NULL
                : SERIALIZED_FILE_MANAGED_RID_UNIQUE;
        }
    }
    return true;
}

static void dispose_prerequisites(ManagedBuild* build) {
    for (size_t index = 0U; index < build->type_count; ++index) {
        serialized_file_schema_dispose(&build->types[index].owner);
    }
    if (build->types) {
        mem_free(build->types, build->cache_bytes);
    }
    serialized_file_reference_index_dispose(&build->index);
    serialized_file_schema_dispose(&build->host.owner);
}

SerializedFileManagedValuesResult serialized_file_managed_values_create(
    const SerializedFileDirectory* directory,
    const SerializedFileMetadataTail* tail,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileManagedValuesLimits* limits,
    SerializedFileManagedValues* out_values) {
    ManagedBuild build = {.directory = directory,
        .tail = tail,
        .limits = limits,
        .bytes = mapped_prefix,
        .result = {.status = SERIALIZED_FILE_MANAGED_VALUES_OK,
            .object_ordinal = SIZE_MAX,
            .type_ordinal = SIZE_MAX,
            .node_ordinal = SIZE_MAX,
            .registry_row_ordinal = SIZE_MAX,
            .error_offset = UINT64_MAX,
            .query_registry_row_ordinal = SIZE_MAX,
            .type_match = {.first_ordinal = SIZE_MAX, .second_ordinal = SIZE_MAX}}};
    if (!admit_arguments(&build, mapped_size, out_values) ||
        !charge(&build, 1U, SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX) ||
        !admit_source(&build, object_ordinal, mapped_size) || !reserve_memory(&build) ||
        !create_schema(&build, build.view.object.type_ordinal, false, &build.host)) {
        goto cleanup_managed_values;
    }
    build.view.schema = build.host.selected.schema;
    build.view.context = build.host.selected.context;
    if (!create_index_and_cache(&build) || !parse_payload(&build) || !allocate_values(&build) ||
        !parse_payload(&build) || !resolve_slots(&build) ||
        !charge(&build, 1U, SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        goto cleanup_managed_values;
    }
    build.view.retained_bytes = build.result.required_retained_bytes;
    build.storage->view = build.view;

cleanup_managed_values:
    dispose_prerequisites(&build);
    if (build.result.status == SERIALIZED_FILE_MANAGED_VALUES_OK) {
        out_values->implementation = build.storage;
    } else if (build.storage) {
        mem_free(build.storage, build.result.required_retained_bytes);
    }
    return build.result;
}

void serialized_file_managed_values_init(SerializedFileManagedValues* values) {
    if (values) {
        values->implementation = NULL;
    }
}

void serialized_file_managed_values_dispose(SerializedFileManagedValues* values) {
    if (!values || !values->implementation) {
        return;
    }
    ManagedStorage* storage = values->implementation;
    mem_free(storage, storage->view.retained_bytes);
    values->implementation = NULL;
}

const SerializedFileManagedValuesView* serialized_file_managed_values_view(
    const SerializedFileManagedValues* values) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    return storage ? &storage->view : NULL;
}

SerializedFileManagedValuesStorageRange serialized_file_managed_values_storage_range(
    const SerializedFileManagedValues* values) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    const SerializedFileManagedValuesStorageRange range = {
        storage, storage ? storage->view.retained_bytes : 0U};
    return range;
}

const SerializedFileManagedValue* serialized_file_managed_values_value(
    const SerializedFileManagedValues* values, size_t ordinal) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    if (!storage || ordinal >= storage->view.value_count) {
        return NULL;
    }
    const SerializedFileManagedValue* records =
        (const SerializedFileManagedValue*)((const uint8_t*)storage + storage->values_offset);
    return &records[ordinal];
}

const SerializedFileManagedSelectedType* serialized_file_managed_values_selected_type(
    const SerializedFileManagedValues* values, size_t ordinal) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    if (!storage || ordinal >= storage->view.selected_type_count) {
        return NULL;
    }
    const SerializedFileManagedSelectedType* records =
        (const SerializedFileManagedSelectedType*)((const uint8_t*)storage + storage->types_offset);
    return &records[ordinal];
}

const SerializedFileManagedRegistryRow* serialized_file_managed_values_registry_row(
    const SerializedFileManagedValues* values, size_t ordinal) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    if (!storage || ordinal >= storage->view.registry_row_count) {
        return NULL;
    }
    const SerializedFileManagedRegistryRow* records =
        (const SerializedFileManagedRegistryRow*)((const uint8_t*)storage + storage->rows_offset);
    return &records[ordinal];
}

const SerializedFileManagedRidSlot* serialized_file_managed_values_rid_slot(
    const SerializedFileManagedValues* values, size_t ordinal) {
    const ManagedStorage* storage = values ? values->implementation : NULL;
    if (!storage || ordinal >= storage->view.rid_slot_count) {
        return NULL;
    }
    const SerializedFileManagedRidSlot* records =
        (const SerializedFileManagedRidSlot*)((const uint8_t*)storage + storage->slots_offset);
    return &records[ordinal];
}
