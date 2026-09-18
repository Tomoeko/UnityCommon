// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_metadata_tail.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_metadata_tail_internal.h"
#include "serialized_metadata_reader_internal.h"

#include "common/common.h"

#include <stdalign.h>

_Static_assert(SIZE_MAX >= UINT32_MAX, "Tail row counts require at least 32-bit size_t");
_Static_assert(SIZE_MAX <= UINT64_MAX, "Mapped byte counts must fit file coordinates");

enum {
    SCRIPT_ALIGNMENT = 4,
    SCRIPT_IDENTIFIER_BYTES = 8,
    EXTERNAL_GUID_BYTES = 16
};

typedef struct {
    SerializedFileMetadataTailView view;
    SerializedFileMetadataTailScriptRow* scripts;
    SerializedFileMetadataTailExternalRow* externals;
    SerializedFileMetadataTailReferenceTypeRow* reference_types;
} TailStorage;

typedef struct {
    size_t scripts_offset;
    size_t externals_offset;
    size_t reference_types_offset;
    size_t total_bytes;
} TailLayout;

typedef struct {
    const SerializedFileMetadataTailLimits* limits;
    SerializedFileMetadataTailResult* result;
    SerializedFileMetadataTailView view;
    SerializedMetadataReader reader;
    SerializedFileMetadataTailField field;
    size_t row_ordinal;
    SerializedFileMetadataTailScriptRow* scripts;
    SerializedFileMetadataTailExternalRow* externals;
    SerializedFileMetadataTailReferenceTypeRow* reference_types;
    size_t script_capacity;
    size_t external_capacity;
    size_t reference_type_capacity;
    bool replay;
} TailScan;

static SerializedFileMetadataTailResult initial_result(void) {
    const SerializedFileMetadataTailResult result = {.status = SERIALIZED_FILE_METADATA_TAIL_OK,
        .row_ordinal = SIZE_MAX,
        .error_offset = SERIALIZED_FILE_PREFIX_NO_OFFSET};
    return result;
}

static bool reject(SerializedFileMetadataTailResult* result,
    SerializedFileMetadataTailStatus status,
    SerializedFileMetadataTailLimit limit,
    SerializedFileMetadataTailField field,
    size_t row_ordinal,
    uint64_t offset) {
    result->status = status;
    result->limit = limit;
    result->field = field;
    result->row_ordinal = row_ordinal;
    result->error_offset = offset;
    return false;
}

static bool charge_work(SerializedFileMetadataTailResult* result,
    const SerializedFileMetadataTailLimits* limits,
    uint64_t amount,
    SerializedFileMetadataTailField field,
    size_t row_ordinal,
    uint64_t offset) {
    if (amount > limits->max_work - result->work_used) {
        return reject(result,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK,
            field,
            row_ordinal,
            offset);
    }
    result->work_used += amount;
    return true;
}

static bool charge_owner_work(SerializedFileMetadataTailResult* result,
    const SerializedFileMetadataTailLimits* limits,
    uint64_t amount) {
    return charge_work(result,
        limits,
        amount,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
        SIZE_MAX,
        SERIALIZED_FILE_PREFIX_NO_OFFSET);
}

static bool valid_arguments(const SerializedFileDirectory* directory,
    const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFileMetadataTailLimits* limits,
    const SerializedFileMetadataTail* output) {
    if (!directory || !limits || !output || (!bytes && mapped_size) ||
        (uint64_t)mapped_size > logical_size) {
        return false;
    }
    const void* const ranges[] = {directory, limits, output, bytes};
    const size_t sizes[] = {sizeof(*directory), sizeof(*limits), sizeof(*output), mapped_size};
    for (size_t first = 0U; first < 4U; ++first) {
        for (size_t second = first + 1U; second < 4U; ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool parent_storage_disjoint(const void* storage,
    size_t storage_size,
    const SerializedFileDirectory* directory,
    const uint8_t* bytes,
    size_t mapped_size,
    const SerializedFileMetadataTailLimits* limits,
    const SerializedFileMetadataTail* output) {
    return !serialized_file_storage_overlaps_internal(
               storage, storage_size, directory, sizeof(*directory)) &&
        !serialized_file_storage_overlaps_internal(storage, storage_size, bytes, mapped_size) &&
        !serialized_file_storage_overlaps_internal(
            storage, storage_size, limits, sizeof(*limits)) &&
        !serialized_file_storage_overlaps_internal(storage, storage_size, output, sizeof(*output));
}

/* Every byte in this span was admitted by the cursor. Present empty spans keep
 * their actual boundary, including a possible one-past-end address. */
static SerializedFilePrefixSpan consumed_span(const TailScan* scan, uint64_t start) {
    const SerializedFilePrefixSpan span = {
        scan->reader.bytes + (size_t)start, start, (size_t)(scan->reader.position - start)};
    return span;
}

static SerializedFilePrefixSpan absent_span(void) {
    const SerializedFilePrefixSpan span = {NULL, SERIALIZED_FILE_PREFIX_NO_OFFSET, 0};
    return span;
}

static bool reject_reader(TailScan* scan) {
    SerializedFileMetadataTailStatus status = SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE;
    SerializedFileMetadataTailLimit limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE;
    switch (scan->reader.status) {
    case SERIALIZED_METADATA_READ_METADATA_END:
    case SERIALIZED_METADATA_READ_NEGATIVE_COUNT:
        status = SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA;
        break;
    case SERIALIZED_METADATA_READ_MAPPING_END:
        status = SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING;
        break;
    case SERIALIZED_METADATA_READ_BYTE_LIMIT:
        status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_TAIL_BYTES;
        break;
    case SERIALIZED_METADATA_READ_WORK_LIMIT:
        status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK;
        break;
    case SERIALIZED_METADATA_READ_TYPE_SHAPE:
        status = SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TYPE_SHAPE;
        break;
    case SERIALIZED_METADATA_READ_TREE_SHAPE:
        status = SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TREE_SHAPE;
        break;
    case SERIALIZED_METADATA_READ_NODE_LIMIT:
        status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_NODES;
        break;
    case SERIALIZED_METADATA_READ_STRING_LIMIT:
        status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_TREE_STRING_BYTES;
        break;
    case SERIALIZED_METADATA_READ_TERMINATED_STRING_LIMIT:
        status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
        limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_TERMINATED_STRING_BYTES;
        break;
    case SERIALIZED_METADATA_READ_OK:
        break;
    }
    return reject(
        scan->result, status, limit, scan->field, scan->row_ordinal, scan->reader.error_offset);
}

static bool take_bytes(TailScan* scan, uint64_t width, SerializedFilePrefixSpan* out_source) {
    return serialized_metadata_take(&scan->reader, width, out_source) || reject_reader(scan);
}

static bool take_u32(TailScan* scan, SerializedFilePrefixSpan* out_source, uint32_t* out_value) {
    return serialized_metadata_read_u32(&scan->reader, out_source, out_value) ||
        reject_reader(scan);
}

static bool take_string(
    TailScan* scan, SerializedFileMetadataTailField field, SerializedFilePrefixSpan* out_source) {
    scan->field = field;
    return serialized_metadata_read_terminated_string(&scan->reader,
               scan->limits->max_terminated_string_bytes,
               &scan->view.terminated_string_byte_count,
               out_source) ||
        reject_reader(scan);
}

static bool take_table_count(TailScan* scan,
    SerializedFileMetadataTailField field,
    SerializedFileMetadataTailLimit limit,
    size_t maximum,
    size_t replay_capacity,
    SerializedFilePrefixSpan* out_source,
    size_t* out_count) {
    scan->field = field;
    scan->row_ordinal = SIZE_MAX;
    uint32_t count;
    if (!serialized_metadata_read_count(&scan->reader, out_source, &count)) {
        return reject_reader(scan);
    }
    if (count > maximum) {
        return reject(scan->result,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            limit,
            field,
            SIZE_MAX,
            out_source->offset);
    }
    if (scan->replay && count != replay_capacity) {
        return reject(scan->result,
            SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
            field,
            SIZE_MAX,
            out_source->offset);
    }
    *out_count = count;
    return true;
}

static bool complete_row(TailScan* scan, SerializedFileMetadataTailField field, uint64_t start) {
    return charge_work(scan->result, scan->limits, 1U, field, scan->row_ordinal, start);
}

static bool scan_script(TailScan* scan, size_t ordinal) {
    const uint64_t start = scan->reader.position;
    SerializedFileMetadataTailScriptRow row = {.ordinal = ordinal};
    scan->row_ordinal = ordinal;
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY;
    if (!take_u32(scan, &row.file_index_source, &row.file_index_bits)) {
        return false;
    }
    /* Unity aligns the metadata-relative position after each file-index word.
     * No script row, and therefore no alignment, exists for a zero count. */
    const uint64_t relative = scan->reader.position - SERIALIZED_FILE_V22_HEADER_SIZE;
    const uint64_t remainder = relative % SCRIPT_ALIGNMENT;
    const uint64_t padding = remainder ? SCRIPT_ALIGNMENT - remainder : 0U;
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ALIGNMENT;
    if (!take_bytes(scan, padding, &row.alignment_source)) {
        return false;
    }
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY;
    if (!take_bytes(scan, SCRIPT_IDENTIFIER_BYTES, &row.local_identifier_source)) {
        return false;
    }
    row.local_identifier_bits = serialized_metadata_decode_u64(
        row.local_identifier_source.data, scan->reader.endian_selector);
    if (!complete_row(scan, SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY, start)) {
        return false;
    }
    row.source = consumed_span(scan, start);
    if (scan->scripts) {
        scan->scripts[ordinal] = row;
    }
    return true;
}

static bool scan_external(TailScan* scan, size_t ordinal) {
    const uint64_t start = scan->reader.position;
    SerializedFileMetadataTailExternalRow row = {.ordinal = ordinal};
    scan->row_ordinal = ordinal;
    if (!take_string(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_LEADING_STRING,
            &row.leading_string_source)) {
        return false;
    }
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_GUID;
    if (!take_bytes(scan, EXTERNAL_GUID_BYTES, &row.guid_source)) {
        return false;
    }
    for (size_t word = 0U; word < 4U; ++word) {
        row.guid_words[word] = serialized_metadata_decode_u32(
            row.guid_source.data + word * 4U, scan->reader.endian_selector);
    }
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_TYPE;
    if (!take_u32(scan, &row.type_source, &row.type_bits) ||
        !take_string(scan, SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_PATH, &row.path_source) ||
        !complete_row(scan, SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_ENTRY, start)) {
        return false;
    }
    row.source = consumed_span(scan, start);
    if (scan->externals) {
        scan->externals[ordinal] = row;
    }
    return true;
}

static bool scan_reference_tree(TailScan* scan, SerializedFileMetadataTailReferenceTypeRow* row) {
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE;
    const SerializedMetadataTreeBudget budget = {.max_node_records = scan->limits->max_node_records,
        .max_string_bytes = scan->limits->max_tree_string_bytes,
        .node_records = &scan->view.node_record_count,
        .string_bytes = &scan->view.tree_string_byte_count};
    if (!serialized_metadata_read_tree(&scan->reader, &budget, &row->tree)) {
        return reject_reader(scan);
    }
    return take_string(scan,
               SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_CLASS_NAME,
               &row->class_name_source) &&
        take_string(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_NAMESPACE,
            &row->namespace_source) &&
        take_string(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_ASSEMBLY_NAME,
            &row->assembly_name_source);
}

static bool scan_reference_type(TailScan* scan, size_t ordinal) {
    const uint64_t start = scan->reader.position;
    SerializedFileMetadataTailReferenceTypeRow row = {.ordinal = ordinal,
        .has_tree = scan->view.directory.prefix.type_tree_enabled_raw != 0U,
        .tree = {.node_count_source = absent_span(),
            .string_count_source = absent_span(),
            .nodes_source = absent_span(),
            .strings_source = absent_span()},
        .class_name_source = absent_span(),
        .namespace_source = absent_span(),
        .assembly_name_source = absent_span()};
    scan->row_ordinal = ordinal;
    scan->field = SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY;
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
    if ((row.has_tree && !scan_reference_tree(scan, &row)) ||
        !complete_row(scan, SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY, start)) {
        return false;
    }
    row.source = consumed_span(scan, start);
    if (scan->reference_types) {
        scan->reference_types[ordinal] = row;
    }
    return true;
}

static bool scan_tail(TailScan* scan) {
    const uint64_t start = scan->reader.position;
    if (!take_table_count(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_COUNT,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_SCRIPTS,
            scan->limits->max_scripts,
            scan->script_capacity,
            &scan->view.script_count_source,
            &scan->view.script_count)) {
        return false;
    }
    const uint64_t scripts_start = scan->reader.position;
    for (size_t ordinal = 0U; ordinal < scan->view.script_count; ++ordinal) {
        if (!scan_script(scan, ordinal)) {
            return false;
        }
    }
    scan->view.script_rows_source = consumed_span(scan, scripts_start);
    if (!take_table_count(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_COUNT,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXTERNALS,
            scan->limits->max_externals,
            scan->external_capacity,
            &scan->view.external_count_source,
            &scan->view.external_count)) {
        return false;
    }
    const uint64_t externals_start = scan->reader.position;
    for (size_t ordinal = 0U; ordinal < scan->view.external_count; ++ordinal) {
        if (!scan_external(scan, ordinal)) {
            return false;
        }
    }
    scan->view.external_rows_source = consumed_span(scan, externals_start);
    if (!take_table_count(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_COUNT,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_REFERENCE_TYPES,
            scan->limits->max_reference_types,
            scan->reference_type_capacity,
            &scan->view.reference_type_count_source,
            &scan->view.reference_type_count)) {
        return false;
    }
    const uint64_t references_start = scan->reader.position;
    for (size_t ordinal = 0U; ordinal < scan->view.reference_type_count; ++ordinal) {
        if (!scan_reference_type(scan, ordinal)) {
            return false;
        }
    }
    scan->view.reference_type_rows_source = consumed_span(scan, references_start);
    scan->row_ordinal = SIZE_MAX;
    if (!take_string(scan,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_USER_INFORMATION,
            &scan->view.user_information_source)) {
        return false;
    }
    if (scan->reader.position != scan->reader.metadata_end) {
        return reject(scan->result,
            SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_EXHAUSTION,
            SIZE_MAX,
            scan->reader.position);
    }
    scan->view.source = consumed_span(scan, start);
    return true;
}

static bool plan_storage(const SerializedFileMetadataTailView* view,
    const SerializedFileMetadataTailLimits* limits,
    SerializedFileMetadataTailResult* result,
    TailLayout* layout) {
    if (!charge_owner_work(result, limits, 1U)) {
        return false;
    }
    layout->total_bytes = sizeof(TailStorage);
    if (!serialized_file_storage_append_array_internal(&layout->total_bytes,
            alignof(SerializedFileMetadataTailScriptRow),
            view->script_count,
            sizeof(SerializedFileMetadataTailScriptRow),
            &layout->scripts_offset) ||
        !serialized_file_storage_append_array_internal(&layout->total_bytes,
            alignof(SerializedFileMetadataTailExternalRow),
            view->external_count,
            sizeof(SerializedFileMetadataTailExternalRow),
            &layout->externals_offset) ||
        !serialized_file_storage_append_array_internal(&layout->total_bytes,
            alignof(SerializedFileMetadataTailReferenceTypeRow),
            view->reference_type_count,
            sizeof(SerializedFileMetadataTailReferenceTypeRow),
            &layout->reference_types_offset)) {
        return reject(result,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_RETAINED_BYTES,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }
    result->required_retained_bytes = layout->total_bytes;
    if (layout->total_bytes > limits->max_retained_bytes) {
        return reject(result,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_RETAINED_BYTES,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
            SIZE_MAX,
            SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }
    return true;
}

void serialized_file_metadata_tail_init(SerializedFileMetadataTail* tail) {
    if (tail) {
        tail->implementation = NULL;
    }
}

void serialized_file_metadata_tail_dispose(SerializedFileMetadataTail* tail) {
    if (tail && tail->implementation) {
        TailStorage* storage = tail->implementation;
        mem_free(storage, storage->view.retained_bytes);
        tail->implementation = NULL;
    }
}

SerializedFileMetadataTailResult serialized_file_metadata_tail_create(
    const SerializedFileDirectory* directory,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFileMetadataTailLimits* limits,
    SerializedFileMetadataTail* out_tail) {
    SerializedFileMetadataTailResult result = initial_result();
    if (!valid_arguments(directory, mapped_prefix, mapped_size, logical_size, limits, out_tail)) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT;
        return result;
    }
    const void* parent_storage;
    size_t parent_storage_size;
    if (!serialized_file_directory_storage_range_internal(
            directory, &parent_storage, &parent_storage_size)) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE;
        return result;
    }
    if (!parent_storage_disjoint(parent_storage,
            parent_storage_size,
            directory,
            mapped_prefix,
            mapped_size,
            limits,
            out_tail)) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT;
        return result;
    }
    if (out_tail->implementation) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE;
        return result;
    }
    if (!charge_owner_work(&result, limits, 1U)) {
        return result;
    }
    const SerializedFileDirectoryView* parent = serialized_file_directory_view(directory);
    if (mapped_prefix != parent->prefix.header.header_source.data ||
        logical_size != parent->prefix.header.file_size) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_SOURCE_MISMATCH;
        return result;
    }
    TailScan scan = {.limits = limits,
        .result = &result,
        .view = {.directory = *parent},
        .reader = {.bytes = mapped_prefix,
            .mapped_size = mapped_size,
            .metadata_end = parent->prefix.header.metadata_to_data_gap.offset,
            .byte_budget_origin = parent->remaining_metadata.offset,
            .max_bytes = limits->max_tail_bytes,
            .max_work = limits->max_work,
            .work_used = &result.work_used,
            .position = parent->remaining_metadata.offset,
            .endian_selector = parent->prefix.header.endian_selector}};
    if (!scan_tail(&scan)) {
        return result;
    }
    TailLayout layout;
    if (!plan_storage(&scan.view, limits, &result, &layout) ||
        !charge_owner_work(&result, limits, layout.total_bytes)) {
        return result;
    }
    TailStorage* storage = mem_alloc(layout.total_bytes);
    if (!storage) {
        result.status = SERIALIZED_FILE_METADATA_TAIL_ALLOCATION_FAILED;
        return result;
    }
    result.peak_retained_bytes = layout.total_bytes;
    memset(storage, 0, layout.total_bytes);
    uint8_t* allocation = (uint8_t*)storage;
    storage->scripts = (SerializedFileMetadataTailScriptRow*)(allocation + layout.scripts_offset);
    storage->externals =
        (SerializedFileMetadataTailExternalRow*)(allocation + layout.externals_offset);
    storage->reference_types =
        (SerializedFileMetadataTailReferenceTypeRow*)(allocation + layout.reference_types_offset);

    /* Retain each allocation capacity before resetting the distinct counters.
     * The shared scanner checks replay counts before writing any table row. */
    scan.script_capacity = scan.view.script_count;
    scan.external_capacity = scan.view.external_count;
    scan.reference_type_capacity = scan.view.reference_type_count;
    scan.view = (SerializedFileMetadataTailView){.directory = *parent};
    scan.reader.position = parent->remaining_metadata.offset;
    scan.scripts = storage->scripts;
    scan.externals = storage->externals;
    scan.reference_types = storage->reference_types;
    scan.replay = true;
    if (!scan_tail(&scan) || !charge_owner_work(&result, limits, 1U)) {
        mem_free(storage, layout.total_bytes);
        return result;
    }
    storage->view = scan.view;
    storage->view.retained_bytes = layout.total_bytes;
    out_tail->implementation = storage;
    return result;
}

const SerializedFileMetadataTailView* serialized_file_metadata_tail_view(
    const SerializedFileMetadataTail* tail) {
    const TailStorage* storage = tail ? tail->implementation : NULL;
    return storage ? &storage->view : NULL;
}

const SerializedFileMetadataTailScriptRow* serialized_file_metadata_tail_script(
    const SerializedFileMetadataTail* tail, size_t ordinal) {
    const TailStorage* storage = tail ? tail->implementation : NULL;
    return storage && ordinal < storage->view.script_count ? &storage->scripts[ordinal] : NULL;
}

const SerializedFileMetadataTailExternalRow* serialized_file_metadata_tail_external(
    const SerializedFileMetadataTail* tail, size_t ordinal) {
    const TailStorage* storage = tail ? tail->implementation : NULL;
    return storage && ordinal < storage->view.external_count ? &storage->externals[ordinal] : NULL;
}

const SerializedFileMetadataTailReferenceTypeRow* serialized_file_metadata_tail_reference_type(
    const SerializedFileMetadataTail* tail, size_t ordinal) {
    const TailStorage* storage = tail ? tail->implementation : NULL;
    return storage && ordinal < storage->view.reference_type_count
        ? &storage->reference_types[ordinal]
        : NULL;
}

bool serialized_file_metadata_tail_storage_range_internal(
    const SerializedFileMetadataTail* tail, const void** out_data, size_t* out_size) {
    const TailStorage* storage = tail ? tail->implementation : NULL;
    if (!storage) {
        return false;
    }
    *out_data = storage;
    *out_size = storage->view.retained_bytes;
    return true;
}
