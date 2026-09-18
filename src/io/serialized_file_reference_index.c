// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_reference_index.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_metadata_tail_internal.h"

#include "common/common.h"

#include <stdalign.h>

_Static_assert(SIZE_MAX <= UINT64_MAX, "Reference index sizes fit source/work coordinates");

enum {
    IDENTITY_COMPONENTS = 3
};

static const SerializedFileReferenceIdentityComponent k_components[IDENTITY_COMPONENTS] = {
    SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS,
    SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE,
    SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY};

typedef struct ReferenceIndexStorage {
    SerializedFileReferenceIndexView view;
    SerializedFileMetadataTailReferenceTypeRow* rows;
    size_t known_size;
} ReferenceIndexStorage;

typedef struct ReferenceIndexBuild {
    const SerializedFileReferenceIndexLimits* limits;
    const SerializedFileMetadataTailView* parent;
    SerializedFileReferenceIndexView view;
    SerializedFileReferenceIndexResult result;
    const uint8_t* bytes;
    size_t known_size;
} ReferenceIndexBuild;

static SerializedFileReferenceIndexResult initial_result(void) {
    const SerializedFileReferenceIndexResult result = {.status = SERIALIZED_FILE_REFERENCE_INDEX_OK,
        .row_ordinal = SIZE_MAX,
        .error_offset = UINT64_MAX};
    return result;
}

static bool range_is_valid(const void* data, size_t size) {
    return !size || (data && size <= UINTPTR_MAX - (uintptr_t)data);
}

static bool ranges_are_disjoint(const void* const* ranges, const size_t* sizes, size_t count) {
    for (size_t first = 0U; first < count; ++first) {
        if (!range_is_valid(ranges[first], sizes[first])) {
            return false;
        }
        for (size_t second = 0U; second < first; ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool reject_build(ReferenceIndexBuild* build,
    SerializedFileReferenceIndexStatus status,
    SerializedFileReferenceIndexLimit limit,
    SerializedFileReferenceIdentityComponent component,
    size_t ordinal,
    uint64_t offset) {
    build->result.status = status;
    build->result.limit = limit;
    build->result.component = component;
    build->result.row_ordinal = ordinal;
    build->result.error_offset = offset;
    return false;
}

static bool charge_build(
    ReferenceIndexBuild* build, uint64_t amount, size_t ordinal, uint64_t offset) {
    if (amount > build->limits->max_work - build->result.work_used) {
        return reject_build(build,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK,
            SERIALIZED_FILE_REFERENCE_IDENTITY_NONE,
            ordinal,
            offset);
    }
    build->result.work_used += amount;
    return true;
}

static bool reject_owner(ReferenceIndexBuild* build,
    SerializedFileReferenceIndexStatus status,
    SerializedFileReferenceIndexLimit limit) {
    return reject_build(
        build, status, limit, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE, SIZE_MAX, UINT64_MAX);
}

static bool source_is_backed(const ReferenceIndexBuild* build, SerializedFilePrefixSpan source) {
    return source.offset <= build->known_size && source.size <= build->known_size - source.offset &&
        source.data == build->bytes + (size_t)source.offset;
}

static bool prepare_parent(const SerializedFileMetadataTail* tail,
    SerializedFileReferenceIndex* output,
    ReferenceIndexBuild* build) {
    const void* const arguments[] = {tail, build->limits, output};
    const size_t argument_sizes[] = {sizeof(*tail), sizeof(*build->limits), sizeof(*output)};
    if (!tail || !build->limits || !output || !ranges_are_disjoint(arguments, argument_sizes, 3U)) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    const void* parent_storage;
    size_t parent_storage_size;
    if (!serialized_file_metadata_tail_storage_range_internal(
            tail, &parent_storage, &parent_storage_size)) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    build->parent = serialized_file_metadata_tail_view(tail);
    const SerializedFilePrefixSpan source = build->parent->source;
    if (source.offset > SIZE_MAX || source.size > SIZE_MAX - (size_t)source.offset) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    build->known_size = (size_t)source.offset + source.size;
    build->bytes = build->parent->directory.prefix.header.header_source.data;
    const void* const ranges[] = {tail, build->limits, output, parent_storage, build->bytes};
    const size_t sizes[] = {sizeof(*tail),
        sizeof(*build->limits),
        sizeof(*output),
        parent_storage_size,
        build->known_size};
    if (!ranges_are_disjoint(ranges, sizes, 5U)) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    if (output->implementation) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    return true;
}

static bool select_domain(ReferenceIndexBuild* build) {
    if (build->parent->directory.engine_version != SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_UNSUPPORTED_ENGINE,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    const size_t count = build->parent->reference_type_count;
    build->result.reference_type_count = count;
    if (count > build->limits->max_reference_types) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_REFERENCE_TYPES);
    }
    if (count && !build->parent->directory.prefix.type_tree_enabled_raw) {
        return reject_owner(build,
            SERIALIZED_FILE_REFERENCE_INDEX_IDENTITIES_UNAVAILABLE,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    }
    build->view.engine_version = build->parent->directory.engine_version;
    build->view.prefix = build->parent->directory.prefix;
    build->view.tail_source = build->parent->source;
    build->view.reference_type_count_source = build->parent->reference_type_count_source;
    build->view.reference_type_rows_source = build->parent->reference_type_rows_source;
    build->view.reference_type_count = count;
    return true;
}

static bool preflight_row(ReferenceIndexBuild* build,
    const SerializedFileMetadataTailReferenceTypeRow* row,
    size_t ordinal) {
    if (!row || row->ordinal != ordinal || !row->has_tree ||
        !source_is_backed(build, row->source) ||
        !source_is_backed(build, row->tree.strings_source)) {
        return reject_build(build,
            SERIALIZED_FILE_REFERENCE_INDEX_SOURCE_MISMATCH,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE,
            SERIALIZED_FILE_REFERENCE_IDENTITY_NONE,
            ordinal,
            row ? row->source.offset : UINT64_MAX);
    }
    const SerializedFilePrefixSpan rows = build->view.reference_type_rows_source;
    if (!source_is_backed(build, rows) || row->source.offset < rows.offset ||
        row->source.offset - rows.offset > rows.size ||
        row->source.size > rows.size - (row->source.offset - rows.offset)) {
        return reject_build(build,
            SERIALIZED_FILE_REFERENCE_INDEX_SOURCE_MISMATCH,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE,
            SERIALIZED_FILE_REFERENCE_IDENTITY_NONE,
            ordinal,
            row->source.offset);
    }
    const SerializedFilePrefixSpan names[IDENTITY_COMPONENTS] = {
        row->class_name_source, row->namespace_source, row->assembly_name_source};
    uint64_t next = row->tree.strings_source.offset + row->tree.strings_source.size;
    const uint64_t end = row->source.offset + row->source.size;
    for (size_t component = 0U; component < IDENTITY_COMPONENTS; ++component) {
        const SerializedFilePrefixSpan name = names[component];
        if (!name.size || !source_is_backed(build, name) || name.offset != next ||
            name.offset < row->source.offset || name.offset > end ||
            name.size > end - name.offset) {
            return reject_build(build,
                SERIALIZED_FILE_REFERENCE_INDEX_SOURCE_MISMATCH,
                SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE,
                k_components[component],
                ordinal,
                name.offset);
        }
        if (name.size >
            build->limits->max_identity_source_bytes - build->view.identity_source_bytes) {
            return reject_build(build,
                SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
                SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_IDENTITY_SOURCE_BYTES,
                k_components[component],
                ordinal,
                name.offset);
        }
        build->view.identity_source_bytes += name.size;
        next = name.offset + name.size;
    }
    if (next != end) {
        return reject_build(build,
            SERIALIZED_FILE_REFERENCE_INDEX_SOURCE_MISMATCH,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE,
            SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY,
            ordinal,
            next);
    }
    return true;
}

SerializedFileReferenceIndexResult serialized_file_reference_index_create(
    const SerializedFileMetadataTail* tail,
    const SerializedFileReferenceIndexLimits* limits,
    SerializedFileReferenceIndex* out_index) {
    ReferenceIndexBuild build = {.limits = limits, .result = initial_result()};
    if (!prepare_parent(tail, out_index, &build) ||
        !charge_build(&build, 1U, SIZE_MAX, UINT64_MAX) || !select_domain(&build)) {
        return build.result;
    }
    const size_t count = build.view.reference_type_count;
    for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
        const SerializedFileMetadataTailReferenceTypeRow* row =
            serialized_file_metadata_tail_reference_type(tail, ordinal);
        if (!charge_build(&build, 1U, ordinal, row ? row->source.offset : UINT64_MAX) ||
            !preflight_row(&build, row, ordinal)) {
            return build.result;
        }
    }
    size_t retained_bytes = sizeof(ReferenceIndexStorage);
    size_t rows_offset;
    if (!charge_build(&build, 1U, SIZE_MAX, UINT64_MAX)) {
        return build.result;
    }
    if (!serialized_file_storage_append_array_internal(&retained_bytes,
            alignof(SerializedFileMetadataTailReferenceTypeRow),
            count,
            sizeof(SerializedFileMetadataTailReferenceTypeRow),
            &rows_offset)) {
        (void)reject_owner(&build,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_RETAINED_BYTES);
        return build.result;
    }
    build.result.required_retained_bytes = retained_bytes;
    if (retained_bytes > limits->max_retained_bytes) {
        (void)reject_owner(&build,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_RETAINED_BYTES);
        return build.result;
    }
    if (!charge_build(&build, retained_bytes, SIZE_MAX, UINT64_MAX)) {
        return build.result;
    }
    ReferenceIndexStorage* storage = mem_alloc(retained_bytes);
    if (!storage) {
        (void)reject_owner(&build,
            SERIALIZED_FILE_REFERENCE_INDEX_ALLOCATION_FAILED,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
        return build.result;
    }
    build.result.peak_retained_bytes = retained_bytes;
    memset(storage, 0, retained_bytes);
    storage->rows = (SerializedFileMetadataTailReferenceTypeRow*)((uint8_t*)storage + rows_offset);
    for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
        const SerializedFileMetadataTailReferenceTypeRow* row =
            serialized_file_metadata_tail_reference_type(tail, ordinal);
        if (!charge_build(&build, 1U, ordinal, row->source.offset)) {
            goto cleanup_storage;
        }
        storage->rows[ordinal] = *row;
    }
    if (!charge_build(&build, 1U, SIZE_MAX, UINT64_MAX)) {
        goto cleanup_storage;
    }
    build.view.retained_bytes = retained_bytes;
    storage->view = build.view;
    storage->known_size = build.known_size;
    out_index->implementation = storage;
    return build.result;

cleanup_storage:
    mem_free(storage, retained_bytes);
    return build.result;
}

void serialized_file_reference_index_init(SerializedFileReferenceIndex* index) {
    if (index) {
        index->implementation = NULL;
    }
}

void serialized_file_reference_index_dispose(SerializedFileReferenceIndex* index) {
    if (index && index->implementation) {
        ReferenceIndexStorage* storage = index->implementation;
        mem_free(storage, storage->view.retained_bytes);
        index->implementation = NULL;
    }
}

const SerializedFileReferenceIndexView* serialized_file_reference_index_view(
    const SerializedFileReferenceIndex* index) {
    const ReferenceIndexStorage* storage = index ? index->implementation : NULL;
    return storage ? &storage->view : NULL;
}

const SerializedFileMetadataTailReferenceTypeRow* serialized_file_reference_index_row(
    const SerializedFileReferenceIndex* index, size_t original_ordinal) {
    const ReferenceIndexStorage* storage = index ? index->implementation : NULL;
    return storage && original_ordinal < storage->view.reference_type_count
        ? &storage->rows[original_ordinal]
        : NULL;
}

static bool query_arguments(const SerializedFileReferenceIndex* index,
    const SerializedFileReferenceKey* key,
    const SerializedFileReferenceQueryLimits* limits,
    const SerializedFileReferenceMatch* output,
    SerializedFileReferenceQueryResult* result) {
    const void* const arguments[] = {index, key, limits, output};
    const size_t argument_sizes[] = {
        sizeof(*index), sizeof(*key), sizeof(*limits), sizeof(*output)};
    if (!index || !key || !limits || !output ||
        !ranges_are_disjoint(arguments, argument_sizes, 4U)) {
        result->status = SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT;
        return false;
    }
    const ReferenceIndexStorage* storage = index->implementation;
    if (!storage) {
        result->status = SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE;
        return false;
    }
    const void* const ranges[] = {
        index, key, limits, output, storage, storage->view.prefix.header.header_source.data};
    const size_t sizes[] = {sizeof(*index),
        sizeof(*key),
        sizeof(*limits),
        sizeof(*output),
        storage->view.retained_bytes,
        storage->known_size};
    if (!ranges_are_disjoint(ranges, sizes, 6U)) {
        result->status = SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT;
        return false;
    }
    const SerializedFileReferenceKeyPart parts[IDENTITY_COMPONENTS] = {
        key->class_name, key->namespace_name, key->assembly_name};
    for (size_t component = 0U; component < IDENTITY_COMPONENTS; ++component) {
        const SerializedFileReferenceKeyPart part = parts[component];
        if (!range_is_valid(part.bytes, part.size) ||
            serialized_file_storage_overlaps_internal(
                part.bytes, part.size, output, sizeof(*output))) {
            result->status = SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT;
            result->component = k_components[component];
            return false;
        }
    }
    return true;
}

static bool charge_query(SerializedFileReferenceQueryResult* result,
    const SerializedFileReferenceQueryLimits* limits,
    uint64_t amount,
    size_t ordinal,
    SerializedFileReferenceIdentityComponent component) {
    if (amount > limits->max_work - result->work_used) {
        result->status = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED;
        result->limit = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK;
        result->row_ordinal = ordinal;
        result->component = component;
        return false;
    }
    result->work_used += amount;
    return true;
}

static bool query_row(const SerializedFileMetadataTailReferenceTypeRow* row,
    const SerializedFileReferenceKeyPart* parts,
    const SerializedFileReferenceQueryLimits* limits,
    SerializedFileReferenceQueryResult* result,
    bool* out_equal) {
    const SerializedFilePrefixSpan names[IDENTITY_COMPONENTS] = {
        row->class_name_source, row->namespace_source, row->assembly_name_source};
    *out_equal = false;
    for (size_t component = 0U; component < IDENTITY_COMPONENTS; ++component) {
        if (!charge_query(result, limits, 1U, row->ordinal, k_components[component])) {
            return false;
        }
        const size_t length = names[component].size - 1U;
        if (length != parts[component].size) {
            return true;
        }
        if (!length) {
            continue;
        }
        if (!charge_query(result, limits, length, row->ordinal, k_components[component])) {
            return false;
        }
        if (memcmp(names[component].data, parts[component].bytes, length) != 0) {
            return true;
        }
    }
    *out_equal = true;
    return true;
}

SerializedFileReferenceQueryResult serialized_file_reference_index_query(
    const SerializedFileReferenceIndex* index,
    const SerializedFileReferenceKey* key,
    const SerializedFileReferenceQueryLimits* limits,
    SerializedFileReferenceMatch* out_match) {
    SerializedFileReferenceQueryResult result = {
        .status = SERIALIZED_FILE_REFERENCE_INDEX_OK, .row_ordinal = SIZE_MAX};
    if (!query_arguments(index, key, limits, out_match, &result) ||
        !charge_query(&result, limits, 1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE)) {
        return result;
    }
    const ReferenceIndexStorage* storage = index->implementation;
    const SerializedFileReferenceKeyPart parts[IDENTITY_COMPONENTS] = {
        key->class_name, key->namespace_name, key->assembly_name};
    size_t key_size = 0U;
    for (size_t component = 0U; component < IDENTITY_COMPONENTS; ++component) {
        if (parts[component].size > limits->max_component_bytes) {
            result.status = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED;
            result.limit = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_COMPONENT_BYTES;
            result.component = k_components[component];
            return result;
        }
        if (parts[component].size > limits->max_key_bytes - key_size) {
            result.status = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED;
            result.limit = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_KEY_BYTES;
            result.component = k_components[component];
            return result;
        }
        key_size += parts[component].size;
    }
    SerializedFileReferenceMatch match = {.kind = SERIALIZED_FILE_REFERENCE_MISSING,
        .first_ordinal = SIZE_MAX,
        .second_ordinal = SIZE_MAX};
    for (size_t ordinal = 0U; ordinal < storage->view.reference_type_count; ++ordinal) {
        if (!charge_query(&result, limits, 1U, ordinal, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE)) {
            return result;
        }
        bool equal;
        if (!query_row(&storage->rows[ordinal], parts, limits, &result, &equal)) {
            return result;
        }
        if (!equal) {
            continue;
        }
        if (!match.match_count) {
            match.first_ordinal = ordinal;
            match.kind = SERIALIZED_FILE_REFERENCE_UNIQUE;
        } else if (match.match_count == 1U) {
            match.second_ordinal = ordinal;
            match.kind = SERIALIZED_FILE_REFERENCE_AMBIGUOUS;
        }
        ++match.match_count;
    }
    if (!charge_query(&result, limits, 1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE)) {
        return result;
    }
    *out_match = match;
    return result;
}
