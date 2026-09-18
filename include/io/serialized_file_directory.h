// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_DIRECTORY_H
#define SERIALIZED_FILE_DIRECTORY_H

#include "io/serialized_file_prefix.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SerializedFileDirectoryEngineVersion {
    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1 = 1,
    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1
} SerializedFileDirectoryEngineVersion;

typedef struct SerializedFileDirectoryLimits {
    size_t max_version_bytes; /* Includes NUL, as in the physical prefix query. */
    size_t max_types;
    size_t max_objects;
    uint64_t max_node_records;
    uint64_t max_string_bytes;
    uint64_t max_dependency_words;
    uint64_t max_metadata_bytes;
    size_t max_retained_bytes;
    size_t max_scratch_bytes;
    uint64_t max_work;
} SerializedFileDirectoryLimits;

typedef struct SerializedFileDirectoryTree {
    SerializedFilePrefixSpan node_count_source;
    SerializedFilePrefixSpan string_count_source;
    SerializedFilePrefixSpan nodes_source;   /* Opaque 32-byte records. */
    SerializedFilePrefixSpan strings_source; /* Opaque bytes, not resolved strings. */
    uint32_t node_count;                     /* Raw u32; this owner requires a positive count. */
    uint32_t string_byte_count;              /* Raw u32, including its high bit. */
} SerializedFileDirectoryTree;

/* Absent optional sources are {NULL, UINT64_MAX, 0}. A present empty span
 * retains its actual mapped boundary address and offset; do not dereference it. */
typedef struct SerializedFileDirectoryTypeRow {
    size_t ordinal;
    SerializedFilePrefixSpan source; /* Complete original ordinary type entry. */
    SerializedFilePrefixSpan class_id_source;
    SerializedFilePrefixSpan stripped_source;
    SerializedFilePrefixSpan script_index_source;
    SerializedFilePrefixSpan script_hash_source;
    SerializedFilePrefixSpan type_hash_source;
    uint32_t class_id_bits;
    uint16_t script_index_bits; /* Bit15 is the sign bit for the hash predicate. */
    uint8_t stripped_raw;
    bool has_script_hash;
    bool has_tree;
    SerializedFileDirectoryTree tree;
    SerializedFilePrefixSpan dependency_count_source;
    SerializedFilePrefixSpan dependency_words_source;
    uint32_t dependency_count; /* Nonnegative count; words have no index claim. */
} SerializedFileDirectoryTypeRow;

typedef struct SerializedFileDirectoryObjectRow {
    size_t ordinal;
    SerializedFilePrefixSpan source; /* Exactly 24 bytes; fields at 0, 8, 16, 20. */
    uint64_t path_id_bits;
    uint64_t relative_data_offset;
    uint32_t byte_size;
    uint32_t type_ordinal;             /* Validated against the ordinary type count. */
    SerializedFilePrefixRange payload; /* Slice-relative coordinates, never a pointer. */
} SerializedFileDirectoryObjectRow;

typedef struct SerializedFileDirectoryView {
    SerializedFileDirectoryEngineVersion engine_version;
    SerializedFilePrefixView prefix;
    SerializedFilePrefixSpan parsed_metadata_source;
    SerializedFilePrefixSpan type_count_source;
    SerializedFilePrefixSpan type_rows_source;
    SerializedFilePrefixSpan object_count_source;
    SerializedFilePrefixSpan object_padding_source;
    SerializedFilePrefixSpan object_rows_source;
    SerializedFilePrefixRange remaining_metadata;
    size_t type_count;
    size_t object_count;
    uint64_t node_record_count;
    uint64_t string_byte_count;
    uint64_t dependency_word_count;
    size_t retained_bytes;
} SerializedFileDirectoryView;

typedef struct SerializedFileDirectory {
    void* implementation;
} SerializedFileDirectory;

typedef enum SerializedFileDirectoryStatus {
    SERIALIZED_FILE_DIRECTORY_OK = 0,
    SERIALIZED_FILE_DIRECTORY_INVALID_ARGUMENT,
    SERIALIZED_FILE_DIRECTORY_INVALID_STATE,
    SERIALIZED_FILE_DIRECTORY_PREFIX_REJECTED,
    SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_ENGINE_VERSION,
    SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_FLAG,
    SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TYPE_SHAPE,
    SERIALIZED_FILE_DIRECTORY_UNSUPPORTED_TREE_SHAPE,
    SERIALIZED_FILE_DIRECTORY_MALFORMED_METADATA,
    SERIALIZED_FILE_DIRECTORY_INCOMPLETE_MAPPING,
    SERIALIZED_FILE_DIRECTORY_LIMIT_EXCEEDED,
    SERIALIZED_FILE_DIRECTORY_ALLOCATION_FAILED
} SerializedFileDirectoryStatus;

typedef enum SerializedFileDirectoryLimit {
    SERIALIZED_FILE_DIRECTORY_LIMIT_NONE = 0,
    SERIALIZED_FILE_DIRECTORY_LIMIT_TYPES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_OBJECTS,
    SERIALIZED_FILE_DIRECTORY_LIMIT_NODES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_STRING_BYTES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_DEPENDENCIES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_METADATA_BYTES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_RETAINED_BYTES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_SCRATCH_BYTES,
    SERIALIZED_FILE_DIRECTORY_LIMIT_WORK
} SerializedFileDirectoryLimit;

typedef enum SerializedFileDirectoryField {
    SERIALIZED_FILE_DIRECTORY_FIELD_NONE = 0,
    SERIALIZED_FILE_DIRECTORY_FIELD_ENGINE_VERSION,
    SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_COUNT,
    SERIALIZED_FILE_DIRECTORY_FIELD_TYPE_ENTRY,
    SERIALIZED_FILE_DIRECTORY_FIELD_TREE,
    SERIALIZED_FILE_DIRECTORY_FIELD_DEPENDENCIES,
    SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_COUNT,
    SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_PADDING,
    SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_ENTRY,
    SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_RANGE,
    SERIALIZED_FILE_DIRECTORY_FIELD_OBJECT_TYPE
} SerializedFileDirectoryField;

typedef struct SerializedFileDirectoryResult {
    SerializedFileDirectoryStatus status;
    SerializedFileDirectoryLimit limit;
    SerializedFileDirectoryField field;
    size_t row_ordinal;    /* SIZE_MAX when no original type/object row is selected. */
    uint64_t error_offset; /* Slice-relative; UINT64_MAX when not source-specific. */
    uint64_t work_used;
    /* Required bytes are known only after successful preflight. Peaks include
     * allocations released on failure; they do not imply published output. */
    size_t required_retained_bytes;
    size_t peak_retained_bytes;
    size_t peak_scratch_bytes;
    bool prefix_attempted;
    SerializedFilePrefixResult prefix_result;
} SerializedFileDirectoryResult;

/* Initialize once; never copy an owning handle or reinitialize live storage.
 * Disposal is NULL-safe, idempotent, and never reads the borrowed file bytes. */
void serialized_file_directory_init(SerializedFileDirectory* directory);
void serialized_file_directory_dispose(SerializedFileDirectory* directory);

/* Constructs the ordinary-type/object index in original wire order. Requires
 * an initialized empty output; failure leaves it empty and all inputs unchanged.
 * Each explicit engine selection requires its exact version bytes; the
 * selection is not a trusted override. The separate 2021.3.29f1 selection
 * establishes this physical directory grammar only, not complete acceptance
 * by the 2021.3.35f1 reader or a 2021.3.29f1 recovery profile.
 *
 * mapped_prefix/logical_size retain the physical-prefix ownership and length
 * premise. Input backing must remain immutable throughout construction and
 * outlive every borrowed span. No complete-file, metadata, node, string or
 * dependency-byte copy is retained. Objects need not be mapped.
 * limits/output must be disjoint from each other and the mapped input; overlap
 * is rejected before prefix admission. Arbitrary forged live handles are invalid.
 *
 * Scope ends after object rows. Preserve duplicates, overlap, raw fields and
 * padding; validate each row's type ordinal and individual payload coordinates.
 * No uniqueness, nonoverlap, schema, tail-table or payload-value certificate is
 * implied. Reject zero-node trees and class ID -1/negative-script-index ambiguity.
 * Type, object and dependency counts must be nonnegative signed32 values.
 * Node and string-byte counts retain their full unsigned32 domain under the
 * cardinality/extent limits. Raw stripped bytes, dependency words and PathID
 * bits have no additional domain restriction in this physical owner.
 *
 * All caps are real, including zero; there is no implicit unlimited budget.
 * Retained/scratch caps cover requested heap payload and alignment padding,
 * excluding allocator bookkeeping, caller handles and fixed stack locals.
 * Construction uses two bounded passes and one retained allocation; heap
 * scratch is zero. Charge the physical prefix query, then each non-NUL version
 * byte before comparison. Each pass charges every consumed metadata byte by
 * field/span width and one unit per completed row. Charge one unit for storage
 * planning, the exact retained allocation size before initialization, and one
 * unit before publication. Failed charges consume no work.
 *
 * Field checks occur in this order: declared metadata extent, mapped extent,
 * metadata-byte ceiling, work, then decode/domain validation. Aggregate source
 * cardinality limits are checked after their count field and before its payload.
 * max_metadata_bytes counts the distinct metadata interval through object rows,
 * including prefix and padding, but excludes all uninspected tail/payload bytes.
 * Successful work is P+V+2*(D+T+O)+R+2, with prefix work P, version bytes V,
 * directory bytes D after the prefix, type/object counts T/O and allocation R.
 * Version/prefix failures precede directory scanning. Required retained bytes
 * become available after complete preflight and checked layout planning.
 * A mismatch charges version bytes through the first differing non-NUL byte;
 * a short matching version ends at its uncharged NUL. Unknown selections reject
 * after prefix admission without comparing version bytes. Source extent errors
 * identify the metadata/mapping end; count/domain errors identify their field.
 * Metadata/work limits identify the next field start, except an already consumed
 * prefix exceeding the metadata cap identifies the prefix end. Row-completion
 * work identifies the row start; planning/allocation/publication use NO_OFFSET.
 */
SerializedFileDirectoryResult serialized_file_directory_create(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    SerializedFileDirectoryEngineVersion engine_version,
    const SerializedFileDirectoryLimits* limits,
    SerializedFileDirectory* out_directory);

/* O(1), allocation-free accessors; return NULL for an empty handle or invalid
 * ordinal. Returned rows/view are immutable and live until directory disposal.
 * Their spans additionally require the original input backing to remain live. */
const SerializedFileDirectoryView* serialized_file_directory_view(
    const SerializedFileDirectory* directory);
const SerializedFileDirectoryTypeRow* serialized_file_directory_type(
    const SerializedFileDirectory* directory, size_t ordinal);
const SerializedFileDirectoryObjectRow* serialized_file_directory_object(
    const SerializedFileDirectory* directory, size_t ordinal);

#ifdef __cplusplus
}
#endif

#endif
