// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_directory.h"

#include "serialized_file_directory_internal.h"
#include "serialized_metadata_reader_internal.h"

#include "common/common.h"

#include <stdalign.h>

_Static_assert(SIZE_MAX >= UINT32_MAX, "Directory row counts require at least 32-bit size_t");
_Static_assert(SIZE_MAX <= UINT64_MAX, "Mapped byte counts must fit file coordinates");

enum {
    DEPENDENCY_SIZE = 4,
    OBJECT_ALIGNMENT = 4,
    OBJECT_SIZE = 24,
    OBJECT_OFFSET_FIELD = 8,
    OBJECT_SIZE_FIELD = 16,
    OBJECT_TYPE_FIELD = 20
};

typedef struct {
    SerializedFileDirectoryView view;
    SerializedFileDirectoryTypeRow* types;
    SerializedFileDirectoryObjectRow* objects;
} DirectoryStorage;

typedef struct {
    size_t types_offset;
    size_t objects_offset;
    size_t total_bytes;
} DirectoryLayout;

typedef struct {
    const uint8_t* bytes;
    const SerializedFileDirectoryLimits* limits;
    SerializedFileDirectoryResult* result;
    SerializedFileDirectoryView view;
    SerializedMetadataReader reader;
    SerializedFileDirectoryField field;
    size_t row_ordinal;
    SerializedFileDirectoryTypeRow* types;
    SerializedFileDirectoryObjectRow* objects;
    size_t type_capacity;
    size_t object_capacity;
} DirectoryScan;

static SerializedFileDirectoryResult initial_result(void) {
    const SerializedFileDirectoryResult result = {.status = SERIALIZED_FILE_DIRECTORY_OK,
        .row_ordinal = SIZE_MAX,
        .error_offset = SERIALIZED_FILE_PREFIX_NO_OFFSET,
        .prefix_result = {
            SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT, 0, SERIALIZED_FILE_PREFIX_NO_OFFSET}};
    return result;
}

static bool reject(SerializedFileDirectoryResult* result,
    SerializedFileDirectoryStatus status,
    SerializedFileDirectoryLimit limit,
    SerializedFileDirectoryField field,
    size_t row_ordinal,
    uint64_t offset) {
    result->status = status;
    result->limit = limit;
    result->field = field;
    result->row_ordinal = row_ordinal;
    result->error_offset = offset;
    return false;
}

static bool charge_work(SerializedFileDirectoryResult* result,
    const SerializedFileDirectoryLimits* limits,
    uint64_t amount,
    SerializedFileDirectoryField field,
    size_t row_ordinal,
    uint64_t offset) {
    if (amount > limits->max_work - result->work_used) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_WORK,
            field,
            row_ordinal,
            offset);
    }
    result->work_used += amount;
    return true;
}

bool serialized_file_storage_overlaps_internal(
    const void* first, size_t first_size, const void* second, size_t second_size) {
    if (!first_size || !second_size) {
        return false;
    }
    const uintptr_t first_address = (uintptr_t)first;
    const uintptr_t second_address = (uintptr_t)second;
    if (first_address <= second_address) {
        return second_address - first_address < first_size;
    }
    return first_address - second_address < second_size;
}

static bool valid_arguments(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFileDirectoryLimits* limits,
    const SerializedFileDirectory* output) {
    return limits && output && (bytes || !mapped_size) && (uint64_t)mapped_size <= logical_size &&
        !serialized_file_storage_overlaps_internal(bytes, mapped_size, output, sizeof(*output)) &&
        !serialized_file_storage_overlaps_internal(bytes, mapped_size, limits, sizeof(*limits)) &&
        !serialized_file_storage_overlaps_internal(
            limits, sizeof(*limits), output, sizeof(*output));
}

/* The cursor has already admitted this complete span, including a possible
 * one-past-end address for a present empty span. */
static SerializedFilePrefixSpan mapped_span(const uint8_t* bytes, uint64_t offset, size_t size) {
    const SerializedFilePrefixSpan span = {bytes + (size_t)offset, offset, size};
    return span;
}

static SerializedFilePrefixSpan absent_span(void) {
    const SerializedFilePrefixSpan span = {NULL, SERIALIZED_FILE_PREFIX_NO_OFFSET, 0};
    return span;
}

/* The shared reader preserves field admission order; this owner supplies the
 * public table/row diagnostic without changing the physical failure offset. */
static bool reject_reader(DirectoryScan* scan) {
    SerializedFileDirectoryStatus status = SERIALIZED_FILE_DIRECTORY_INVALID_STATE;
    SerializedFileDirectoryLimit limit = SERIALIZED_FILE_DIRECTORY_LIMIT_NONE;
    switch (scan->reader.status) {
    case SERIALIZED_METADATA_READ_METADATA_END:
    case SERIALIZED_METADATA_READ_NEGATIVE_COUNT:
        status = SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA;
        break;
    case SERIALIZED_METADATA_READ_MAPPING_END:
        status = SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING;
        break;
    case SERIALIZED_METADATA_READ_BYTE_LIMIT:
        status = SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_DIRECTORY_LIMIT_METADATA_BYTES;
        break;
    case SERIALIZED_METADATA_READ_WORK_LIMIT:
        status = SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_DIRECTORY_LIMIT_WORK;
        break;
    case SERIALIZED_METADATA_READ_TYPE_SHAPE:
        status = SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TYPE_SHAPE;
        break;
    case SERIALIZED_METADATA_READ_TREE_SHAPE:
        status = SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_SHAPE;
        break;
    case SERIALIZED_METADATA_READ_NODE_LIMIT:
        status = SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_DIRECTORY_LIMIT_NODES;
        break;
    case SERIALIZED_METADATA_READ_STRING_LIMIT:
        status = SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_DIRECTORY_LIMIT_STRING_BYTES;
        break;
    case SERIALIZED_METADATA_READ_OK:
    case SERIALIZED_METADATA_READ_TERMINATED_STRING_LIMIT:
        /* No ordinary-directory failure produces either status. */
        break;
    }
    return reject(
        scan->result, status, limit, scan->field, scan->row_ordinal, scan->reader.error_offset);
}

static bool take_bytes(DirectoryScan* scan, uint64_t width, SerializedFilePrefixSpan* out_span) {
    return serialized_metadata_take(&scan->reader, width, out_span) || reject_reader(scan);
}

static bool take_count(
    DirectoryScan* scan, SerializedFilePrefixSpan* out_source, uint32_t* out_count) {
    return serialized_metadata_read_count(&scan->reader, out_source, out_count) ||
        reject_reader(scan);
}

static bool add_cardinality(DirectoryScan* scan,
    uint64_t* total,
    uint32_t count,
    uint64_t maximum,
    SerializedFileDirectoryLimit limit,
    uint64_t offset) {
    if ((uint64_t)count > maximum - *total) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            limit,
            scan->field,
            scan->row_ordinal,
            offset);
    }
    *total += count;
    return true;
}

static bool scan_tree(DirectoryScan* scan, SerializedFileDirectoryTypeRow* row) {
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_TREE;
    const SerializedMetadataTreeBudget budget = {.max_node_records = scan->limits->max_node_records,
        .max_string_bytes = scan->limits->max_string_bytes,
        .node_records = &scan->view.node_record_count,
        .string_bytes = &scan->view.string_byte_count};
    if (!serialized_metadata_read_tree(&scan->reader, &budget, &row->tree)) {
        return reject_reader(scan);
    }

    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES;
    return take_count(scan, &row->dependency_count_source, &row->dependency_count) &&
        add_cardinality(scan,
            &scan->view.dependency_word_count,
            row->dependency_count,
            scan->limits->max_dependency_words,
            SERIALIZED_FILE_DIRECTORY_LIMIT_DEPENDENCIES,
            row->dependency_count_source.offset) &&
        take_bytes(
            scan, (uint64_t)row->dependency_count * DEPENDENCY_SIZE, &row->dependency_words_source);
}

static bool scan_type(DirectoryScan* scan, size_t ordinal) {
    SerializedFileDirectoryTypeRow row = {.ordinal = ordinal,
        .script_hash_source = absent_span(),
        .tree = {.node_count_source = absent_span(),
            .string_count_source = absent_span(),
            .nodes_source = absent_span(),
            .strings_source = absent_span()},
        .dependency_count_source = absent_span(),
        .dependency_words_source = absent_span()};
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY;
    scan->row_ordinal = ordinal;
    const uint64_t start = scan->reader.position;
    SerializedMetadataTypePrefix prefix;
    if (!serialized_metadata_read_type_prefix(&scan->reader, &prefix)) {
        return reject_reader(scan);
    }
    row.class_id_source = prefix.class_id_source;
    row.stripped_source = prefix.stripped_source;
    row.script_index_source = prefix.script_index_source;
    row.script_hash_source = prefix.script_hash_source;
    row.type_hash_source = prefix.type_hash_source;
    row.class_id_bits = prefix.class_id_bits;
    row.script_index_bits = prefix.script_index_bits;
    row.stripped_raw = prefix.stripped_raw;
    row.has_script_hash = prefix.has_script_hash;
    row.has_tree = scan->view.prefix.type_tree_enabled_raw != 0U;
    if (row.has_tree && !scan_tree(scan, &row)) {
        return false;
    }
    if (!charge_work(scan->result,
            scan->limits,
            1U,
            SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY,
            ordinal,
            start)) {
        return false;
    }
    row.source = mapped_span(scan->bytes, start, (size_t)(scan->reader.position - start));
    if (scan->types) {
        scan->types[ordinal] = row;
    }
    return true;
}

static bool scan_object(DirectoryScan* scan, size_t ordinal) {
    SerializedFileDirectoryObjectRow row = {.ordinal = ordinal};
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY;
    scan->row_ordinal = ordinal;
    if (!take_bytes(scan, OBJECT_SIZE, &row.source)) {
        return false;
    }
    const uint8_t endian_selector = scan->view.prefix.header.endian_selector;
    row.path_id_bits = serialized_metadata_decode_u64(row.source.data, endian_selector);
    row.relative_data_offset =
        serialized_metadata_decode_u64(row.source.data + OBJECT_OFFSET_FIELD, endian_selector);
    row.byte_size =
        serialized_metadata_decode_u32(row.source.data + OBJECT_SIZE_FIELD, endian_selector);
    row.type_ordinal =
        serialized_metadata_decode_u32(row.source.data + OBJECT_TYPE_FIELD, endian_selector);

    const SerializedFileHeaderView* header = &scan->view.prefix.header;
    const uint64_t data_size = header->file_size - header->data_offset;
    if (row.relative_data_offset > data_size) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE,
            ordinal,
            row.source.offset + OBJECT_OFFSET_FIELD);
    }
    if (row.byte_size > data_size - row.relative_data_offset) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE,
            ordinal,
            row.source.offset + OBJECT_SIZE_FIELD);
    }
    if (row.type_ordinal >= scan->view.type_count) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_TYPE,
            ordinal,
            row.source.offset + OBJECT_TYPE_FIELD);
    }
    row.payload =
        (SerializedFilePrefixRange){header->data_offset + row.relative_data_offset, row.byte_size};
    if (!charge_work(scan->result,
            scan->limits,
            1U,
            SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY,
            ordinal,
            row.source.offset)) {
        return false;
    }
    if (scan->objects) {
        scan->objects[ordinal] = row;
    }
    return true;
}

static bool scan_directory(DirectoryScan* scan) {
    uint32_t type_count;
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT;
    scan->row_ordinal = SIZE_MAX;
    if (!take_count(scan, &scan->view.type_count_source, &type_count)) {
        return false;
    }
    if (type_count > scan->limits->max_types) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_TYPES,
            scan->field,
            SIZE_MAX,
            scan->view.type_count_source.offset);
    }
    if (scan->types && type_count != scan->type_capacity) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            scan->field,
            SIZE_MAX,
            scan->view.type_count_source.offset);
    }
    scan->view.type_count = type_count;
    const uint64_t types_start = scan->reader.position;
    for (size_t ordinal = 0U; ordinal < scan->view.type_count; ++ordinal) {
        if (!scan_type(scan, ordinal)) {
            return false;
        }
    }
    scan->view.type_rows_source =
        mapped_span(scan->bytes, types_start, (size_t)(scan->reader.position - types_start));

    uint32_t object_count;
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT;
    scan->row_ordinal = SIZE_MAX;
    if (!take_count(scan, &scan->view.object_count_source, &object_count)) {
        return false;
    }
    if (object_count > scan->limits->max_objects) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_OBJECTS,
            scan->field,
            SIZE_MAX,
            scan->view.object_count_source.offset);
    }
    if (scan->objects && object_count != scan->object_capacity) {
        return reject(scan->result,
            SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            scan->field,
            SIZE_MAX,
            scan->view.object_count_source.offset);
    }
    scan->view.object_count = object_count;

    /* Unity aligns the metadata-relative coordinate, not the backing pointer.
     * An empty object table consumes no alignment bytes. */
    const uint64_t metadata_offset = scan->reader.position - SERIALIZED_FILE_V22_HEADER_SIZE;
    const uint64_t remainder = metadata_offset % OBJECT_ALIGNMENT;
    const uint64_t padding = object_count && remainder ? OBJECT_ALIGNMENT - remainder : 0U;
    scan->field = SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_PADDING;
    if (!take_bytes(scan, padding, &scan->view.object_padding_source)) {
        return false;
    }
    const uint64_t objects_start = scan->reader.position;
    for (size_t ordinal = 0U; ordinal < scan->view.object_count; ++ordinal) {
        if (!scan_object(scan, ordinal)) {
            return false;
        }
    }
    scan->view.object_rows_source =
        mapped_span(scan->bytes, objects_start, (size_t)(scan->reader.position - objects_start));
    scan->view.parsed_metadata_source = mapped_span(scan->bytes,
        SERIALIZED_FILE_V22_HEADER_SIZE,
        (size_t)(scan->reader.position - SERIALIZED_FILE_V22_HEADER_SIZE));
    scan->view.remaining_metadata = (SerializedFilePrefixRange){scan->reader.position,
        scan->view.prefix.header.metadata_to_data_gap.offset - scan->reader.position};
    return true;
}

static bool admit_prefix(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    SerializedFileDirectoryEngineVersion engine_version,
    const SerializedFileDirectoryLimits* limits,
    SerializedFileDirectoryResult* result,
    SerializedFilePrefixView* prefix) {
    const SerializedFilePrefixLimits prefix_limits = {limits->max_version_bytes, limits->max_work};
    result->prefix_attempted = true;
    result->prefix_result =
        serialized_file_prefix_query(bytes, mapped_size, logical_size, &prefix_limits, prefix);
    result->work_used = result->prefix_result.work_used;
    if (result->prefix_result.status != SERIALIZED_FILE_PREFIX_OK) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            result->prefix_result.error_offset);
    }
    static const uint8_t version_35[] = "2021.3.35f1";
    static const uint8_t version_29[] = "2021.3.29f1";
    const uint8_t* expected_version = NULL;
    switch (engine_version) {
    case SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1:
        expected_version = version_35;
        break;
    case SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1:
        expected_version = version_29;
        break;
    default:
        break;
    }
    if (!expected_version) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }

    _Static_assert(sizeof(version_35) == sizeof(version_29), "Selected versions have equal widths");
    const size_t expected_size = sizeof(version_35) - 1U;
    for (size_t index = 0U; index < prefix->version_source.size; ++index) {
        const uint64_t offset = prefix->version_source.offset + index;
        if (!charge_work(result,
                limits,
                1U,
                SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
                SIZE_MAX,
                offset)) {
            return false;
        }
        if (index >= expected_size ||
            prefix->version_source.data[index] != expected_version[index]) {
            return reject(result,
                SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
                SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
                SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
                SIZE_MAX,
                offset);
        }
    }
    if (prefix->version_source.size != expected_size) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
            SIZE_MAX,
            prefix->version_terminator_source.offset);
    }
    if (prefix->type_tree_enabled_raw > 1U) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_FLAG,
            SERIALIZED_FILE_DIRECTORY_LIMIT_NONE,
            SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
            SIZE_MAX,
            prefix->type_tree_source.offset);
    }
    if (prefix->metadata_prefix_source.size > limits->max_metadata_bytes) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_METADATA_BYTES,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            prefix->remaining_metadata.offset);
    }
    return true;
}

bool serialized_file_storage_append_array_internal(
    size_t* total, size_t alignment, size_t count, size_t width, size_t* out_offset) {
    const size_t remainder = *total % alignment;
    const size_t padding = remainder ? alignment - remainder : 0U;
    if (dxbc_size_add_overflows(*total, padding) || dxbc_size_multiply_overflows(count, width)) {
        return false;
    }
    const size_t offset = *total + padding;
    const size_t array_size = count * width;
    if (dxbc_size_add_overflows(offset, array_size)) {
        return false;
    }
    *out_offset = offset;
    *total = offset + array_size;
    return true;
}

static bool plan_storage(const SerializedFileDirectoryView* view,
    const SerializedFileDirectoryLimits* limits,
    SerializedFileDirectoryResult* result,
    DirectoryLayout* layout) {
    if (!charge_work(result,
            limits,
            1U,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET)) {
        return false;
    }
    layout->total_bytes = sizeof(DirectoryStorage);
    if (!serialized_file_storage_append_array_internal(&layout->total_bytes,
            alignof(SerializedFileDirectoryTypeRow),
            view->type_count,
            sizeof(SerializedFileDirectoryTypeRow),
            &layout->types_offset) ||
        !serialized_file_storage_append_array_internal(&layout->total_bytes,
            alignof(SerializedFileDirectoryObjectRow),
            view->object_count,
            sizeof(SerializedFileDirectoryObjectRow),
            &layout->objects_offset)) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_RETAINED_BYTES,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }
    result->required_retained_bytes = layout->total_bytes;
    if (layout->total_bytes > limits->max_retained_bytes) {
        return reject(result,
            SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
            SERIALIZED_FILE_DIRECTORY_LIMIT_RETAINED_BYTES,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }
    return true;
}

void serialized_file_directory_init(SerializedFileDirectory* directory) {
    if (directory) {
        directory->implementation = NULL;
    }
}

void serialized_file_directory_dispose(SerializedFileDirectory* directory) {
    if (directory && directory->implementation) {
        DirectoryStorage* storage = directory->implementation;
        mem_free(storage, storage->view.retained_bytes);
        directory->implementation = NULL;
    }
}

SerializedFileDirectoryResult serialized_file_directory_create(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    SerializedFileDirectoryEngineVersion engine_version,
    const SerializedFileDirectoryLimits* limits,
    SerializedFileDirectory* out_directory) {
    SerializedFileDirectoryResult result = initial_result();
    if (!valid_arguments(mapped_prefix, mapped_size, logical_size, limits, out_directory)) {
        result.status = SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT;
        return result;
    }
    if (out_directory->implementation) {
        result.status = SERIALIZED_FILE_DIRECTORY_INVALID_STATE;
        return result;
    }
    SerializedFilePrefixView prefix;
    if (!admit_prefix(
            mapped_prefix, mapped_size, logical_size, engine_version, limits, &result, &prefix)) {
        return result;
    }

    DirectoryScan scan = {.bytes = mapped_prefix,
        .limits = limits,
        .result = &result,
        .view = {.engine_version = engine_version, .prefix = prefix},
        .reader = {.bytes = mapped_prefix,
            .mapped_size = mapped_size,
            .metadata_end = prefix.header.metadata_to_data_gap.offset,
            .byte_budget_origin = SERIALIZED_FILE_V22_HEADER_SIZE,
            .max_bytes = limits->max_metadata_bytes,
            .max_work = limits->max_work,
            .work_used = &result.work_used,
            .position = prefix.remaining_metadata.offset,
            .endian_selector = prefix.header.endian_selector}};
    if (!scan_directory(&scan)) {
        return result;
    }
    DirectoryLayout layout;
    if (!plan_storage(&scan.view, limits, &result, &layout) ||
        !charge_work(&result,
            limits,
            layout.total_bytes,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET)) {
        return result;
    }
    DirectoryStorage* storage = mem_alloc(layout.total_bytes);
    if (!storage) {
        result.status = SERIALIZED_FILE_DIRECTORY_ALLOCATION_FAILED;
        return result;
    }
    result.peak_retained_bytes = layout.total_bytes;
    memset(storage, 0, layout.total_bytes);
    uint8_t* allocation = (uint8_t*)storage;
    storage->types = (SerializedFileDirectoryTypeRow*)(allocation + layout.types_offset);
    storage->objects = (SerializedFileDirectoryObjectRow*)(allocation + layout.objects_offset);

    /* Replay exactly the same scanner against the immutable backing. Preserve
     * the planned capacities explicitly before resetting aggregate counters. */
    scan.type_capacity = scan.view.type_count;
    scan.object_capacity = scan.view.object_count;
    scan.view = (SerializedFileDirectoryView){.engine_version = engine_version, .prefix = prefix};
    scan.reader.position = prefix.remaining_metadata.offset;
    scan.types = storage->types;
    scan.objects = storage->objects;
    if (!scan_directory(&scan) ||
        !charge_work(&result,
            limits,
            1U,
            SERIALIZED_FILE_DIRECTORY_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET)) {
        mem_free(storage, layout.total_bytes);
        return result;
    }
    storage->view = scan.view;
    storage->view.retained_bytes = layout.total_bytes;
    out_directory->implementation = storage;
    return result;
}

const SerializedFileDirectoryView* serialized_file_directory_view(
    const SerializedFileDirectory* directory) {
    const DirectoryStorage* storage = directory ? directory->implementation : NULL;
    return storage ? &storage->view : NULL;
}

const SerializedFileDirectoryTypeRow* serialized_file_directory_type(
    const SerializedFileDirectory* directory, size_t ordinal) {
    const DirectoryStorage* storage = directory ? directory->implementation : NULL;
    return storage && ordinal < storage->view.type_count ? &storage->types[ordinal] : NULL;
}

const SerializedFileDirectoryObjectRow* serialized_file_directory_object(
    const SerializedFileDirectory* directory, size_t ordinal) {
    const DirectoryStorage* storage = directory ? directory->implementation : NULL;
    return storage && ordinal < storage->view.object_count ? &storage->objects[ordinal] : NULL;
}

bool serialized_file_directory_storage_range_internal(
    const SerializedFileDirectory* directory, const void** out_data, size_t* out_size) {
    const DirectoryStorage* storage = directory ? directory->implementation : NULL;
    if (!storage) {
        return false;
    }
    *out_data = storage;
    *out_size = storage->view.retained_bytes;
    return true;
}
