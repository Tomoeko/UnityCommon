// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_METADATA_TAIL_H
#define SERIALIZED_FILE_METADATA_TAIL_H

#include "io/serialized_file_directory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SerializedFileMetadataTailLimits {
    size_t max_scripts;
    size_t max_externals;
    size_t max_reference_types;
    uint64_t max_node_records;
    uint64_t max_tree_string_bytes;
    uint64_t max_terminated_string_bytes; /* Aggregate visited bytes, including every NUL. */
    uint64_t max_tail_bytes;              /* Distinct tail interval; excludes prior directory. */
    size_t max_retained_bytes;
    size_t max_scratch_bytes;
    uint64_t max_work;
} SerializedFileMetadataTailLimits;

typedef struct SerializedFileMetadataTailScriptRow {
    size_t ordinal;
    SerializedFilePrefixSpan source;
    SerializedFilePrefixSpan file_index_source;
    SerializedFilePrefixSpan alignment_source; /* Align4 after the file-index word. */
    SerializedFilePrefixSpan local_identifier_source;
    uint32_t file_index_bits;
    uint64_t local_identifier_bits;
} SerializedFileMetadataTailScriptRow;

typedef struct SerializedFileMetadataTailExternalRow {
    size_t ordinal;
    SerializedFilePrefixSpan source;
    SerializedFilePrefixSpan leading_string_source; /* Includes required NUL. */
    SerializedFilePrefixSpan guid_source;           /* Original 16 bytes. */
    SerializedFilePrefixSpan type_source;
    SerializedFilePrefixSpan path_source; /* Includes required NUL. */
    uint32_t guid_words[4];               /* Selected u32 words; no GUID normalization. */
    uint32_t type_bits;
} SerializedFileMetadataTailExternalRow;

/* A reference type has no dependency count or dependency words. When its
 * inherited tree flag is set, three terminated names follow the tree. */
typedef struct SerializedFileMetadataTailReferenceTypeRow {
    size_t ordinal;
    SerializedFilePrefixSpan source;
    SerializedFilePrefixSpan class_id_source;
    SerializedFilePrefixSpan stripped_source;
    SerializedFilePrefixSpan script_index_source;
    SerializedFilePrefixSpan script_hash_source;
    SerializedFilePrefixSpan type_hash_source;
    uint32_t class_id_bits;
    uint16_t script_index_bits;
    uint8_t stripped_raw;
    bool has_script_hash;
    bool has_tree;
    SerializedFileDirectoryTree tree;
    SerializedFilePrefixSpan class_name_source;    /* NUL included; absent if no tree. */
    SerializedFilePrefixSpan namespace_source;     /* NUL included; absent if no tree. */
    SerializedFilePrefixSpan assembly_name_source; /* NUL included; absent if no tree. */
} SerializedFileMetadataTailReferenceTypeRow;

typedef struct SerializedFileMetadataTailView {
    /* Copied by value. Contains no pointers into the Directory's owned rows.
     * Its spans borrow only the original immutable mapped input. The nested
     * retained_bytes describes the original Directory, not live tail storage. */
    SerializedFileDirectoryView directory;
    SerializedFilePrefixSpan source; /* Complete tail through final NUL. */
    SerializedFilePrefixSpan script_count_source;
    SerializedFilePrefixSpan script_rows_source;
    SerializedFilePrefixSpan external_count_source;
    SerializedFilePrefixSpan external_rows_source;
    SerializedFilePrefixSpan reference_type_count_source;
    SerializedFilePrefixSpan reference_type_rows_source;
    SerializedFilePrefixSpan user_information_source; /* Includes final NUL. */
    size_t script_count;
    size_t external_count;
    size_t reference_type_count;
    uint64_t node_record_count;
    uint64_t tree_string_byte_count;
    uint64_t terminated_string_byte_count;
    size_t retained_bytes;
} SerializedFileMetadataTailView;

typedef struct SerializedFileMetadataTail {
    void* implementation;
} SerializedFileMetadataTail;

typedef enum SerializedFileMetadataTailStatus {
    SERIALIZED_FILE_METADATA_TAIL_OK = 0,
    SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
    SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE,
    SERIALIZED_FILE_METADATA_TAIL_SOURCE_MISMATCH,
    SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TYPE_SHAPE,
    SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TREE_SHAPE,
    SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
    SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
    SERIALIZED_FILE_METADATA_TAIL_ALLOCATION_FAILED
} SerializedFileMetadataTailStatus;

typedef enum SerializedFileMetadataTailLimit {
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE = 0,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_SCRIPTS,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXTERNALS,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_REFERENCE_TYPES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NODES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_TREE_STRING_BYTES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_TERMINATED_STRING_BYTES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_TAIL_BYTES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_RETAINED_BYTES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_SCRATCH_BYTES,
    SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK
} SerializedFileMetadataTailLimit;

typedef enum SerializedFileMetadataTailField {
    SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE = 0,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_COUNT,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ENTRY,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_ALIGNMENT,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_COUNT,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_ENTRY,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_LEADING_STRING,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_GUID,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_TYPE,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_PATH,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_COUNT,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_CLASS_NAME,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_NAMESPACE,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_ASSEMBLY_NAME,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_USER_INFORMATION,
    SERIALIZED_FILE_METADATA_TAIL_FIELD_EXHAUSTION
} SerializedFileMetadataTailField;

typedef struct SerializedFileMetadataTailResult {
    SerializedFileMetadataTailStatus status;
    SerializedFileMetadataTailLimit limit;
    SerializedFileMetadataTailField field;
    size_t row_ordinal;    /* SIZE_MAX outside an original row. */
    uint64_t error_offset; /* Slice-relative; UINT64_MAX if not source-specific. */
    uint64_t work_used;
    size_t required_retained_bytes;
    size_t peak_retained_bytes;
    size_t peak_scratch_bytes;
} SerializedFileMetadataTailResult;

void serialized_file_metadata_tail_init(SerializedFileMetadataTail* tail);
void serialized_file_metadata_tail_dispose(SerializedFileMetadataTail* tail);

/* Requires a live, genuine ordinary Directory and initialized empty output.
 * Inherit its exact35/exact29 selection and admitted flag/prefix; no second
 * caller version override. Require mapped_prefix equal the original header
 * source base and logical_size equal the Directory's admitted file-size
 * premise. A longer readable prefix is allowed at that same immutable backing.
 * Mapping size cannot exceed logical_size; every visited tail byte must fit.
 * No previously inspected byte is reauthenticated or copied.
 *
 * Directory is required only throughout this call. Success copies its View by
 * value and retains independent script/external/reference descriptor arrays.
 * The Directory may then be disposed; input backing must outlive every span.
 * No object payload, external file, tree/string byte copy, or other owner is
 * retained. Disposal is NULL-safe/idempotent and reads neither parent nor input.
 * Owning handles must not be copied, forged, or reinitialized while live.
 *
 * Validate disjoint limits/output/parent-handle/input ranges before admission.
 * Also reject overlap with accessible Directory-owned payload through an
 * internal owner-range helper; do not infer allocator bounds from public spans.
 * Failure leaves output empty and parent/input unchanged; no partial rows are
 * published. Parent storage may overlap its borrowed input only in a forged
 * handle, which is outside the live-owner premise.
 *
 * Preserve row order, duplicate identities, raw bits, GUID bytes, actual
 * alignment, and all terminated string bytes. An empty present string is its
 * one-byte NUL span. Optional absent spans use {NULL, UINT64_MAX, 0}; present
 * empty tree buffers/alignment retain actual boundary addresses and offsets.
 * Script/external/reference counts require nonnegative signed32 values. N/S
 * retain full u32 domains. Reject zero-node trees and class -1/negative-index
 * ambiguity as in the Directory. With tree, reference rows contain names after
 * the tree; without tree, tree and all three names are absent. Enforce exact
 * metadata exhaustion after the final NUL. No semantic identity/index/GUID/path,
 * reference binding, encoding, schema, or full native-reader claim is implied.
 *
 * All caps are real, including zero. Tail/node/tree-string/terminated-string
 * counts are aggregate distinct source quantities, not doubled by replay.
 * Two bounded passes use one aligned retained allocation and zero heap scratch.
 * Work: one unit for parent-premise binding; each pass charges every
 * consumed tail byte plus one per completed row; then one storage-plan unit,
 * the exact allocation request size before initialization, and one publication
 * unit. Thus success is 2*(B+S+E+R)+A+3, where B is tail bytes, S/E/R row counts,
 * and A retained bytes. Failed charges consume no work. Recheck replay counts
 * against preflight capacities before any row writes, without extra work.
 *
 * Span precedence: declared metadata, mapping, distinct tail-byte cap,
 * terminated-byte cap for strings, work, then decode/domain. Counts charge
 * their four bytes before domain/cardinality checks and any payload. Strings
 * inspect/charge one byte at a time, including the required NUL, with no
 * strlen or unbounded scan. Fixed-width spans charge atomically. Source extent
 * errors identify the first unavailable metadata/mapping end; invalid domains
 * identify field starts; limits identify the next field/byte; row-completion
 * work identifies row start; binding/planning/allocation/publication have
 * NO_OFFSET. Trailing metadata identifies the cursor after the final NUL.
 */
SerializedFileMetadataTailResult serialized_file_metadata_tail_create(
    const SerializedFileDirectory* directory,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFileMetadataTailLimits* limits,
    SerializedFileMetadataTail* out_tail);

const SerializedFileMetadataTailView* serialized_file_metadata_tail_view(
    const SerializedFileMetadataTail* tail);
const SerializedFileMetadataTailScriptRow* serialized_file_metadata_tail_script(
    const SerializedFileMetadataTail* tail, size_t ordinal);
const SerializedFileMetadataTailExternalRow* serialized_file_metadata_tail_external(
    const SerializedFileMetadataTail* tail, size_t ordinal);
const SerializedFileMetadataTailReferenceTypeRow* serialized_file_metadata_tail_reference_type(
    const SerializedFileMetadataTail* tail, size_t ordinal);

#ifdef __cplusplus
}
#endif

#endif
