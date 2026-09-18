// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_MANAGED_VALUES_H
#define SERIALIZED_FILE_MANAGED_VALUES_H

#include "io/serialized_file_reference_index.h"
#include "io/serialized_file_schema_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SerializedFileManagedValues {
    void* implementation;
} SerializedFileManagedValues;

typedef struct SerializedFileManagedValuesStorageRange {
    const void* data;
    size_t size;
} SerializedFileManagedValuesStorageRange;

typedef enum SerializedFileManagedValueKind {
    SERIALIZED_FILE_MANAGED_VALUE_SIGNED_INTEGER = 1,
    SERIALIZED_FILE_MANAGED_VALUE_UNSIGNED_INTEGER,
    SERIALIZED_FILE_MANAGED_VALUE_FLOAT32,
    SERIALIZED_FILE_MANAGED_VALUE_BYTE_STRING
} SerializedFileManagedValueKind;

/* Primitive values follow original wire order. Their complete source spans
 * partition the host payload; no repeated container or RID target is inlined. */
typedef struct SerializedFileManagedValue {
    size_t ordinal;
    SerializedFileManagedValueKind kind;
    SerializedFileSchemaRowKind row_kind;
    size_t type_ordinal;
    size_t registry_row_ordinal; /* SIZE_MAX for ordinary host fields. */
    SerializedFileSchemaNode schema_node;
    SerializedFilePrefixSpan source; /* Scalar/string and its executed padding. */
    uint64_t raw_bits;               /* Integer or float32 bits, zero extended; zero for strings. */
    SerializedFilePrefixSpan scalar_source;
    uint32_t length_bits; /* String byte count, zero for nonstrings. */
    SerializedFilePrefixSpan length_source;
    SerializedFilePrefixSpan bytes_source;
    size_t array_schema_ordinal; /* String children; SIZE_MAX for nonstrings. */
    size_t size_schema_ordinal;
    size_t data_schema_ordinal;
    SerializedFilePrefixSpan array_schema_source;
    SerializedFilePrefixSpan size_schema_source;
    SerializedFilePrefixSpan data_schema_source;
    /* Absent unless alignment executes; an executed zero-byte alignment has
     * a present boundary pointer/offset. Absent is {NULL, UINT64_MAX, 0}. */
    SerializedFilePrefixSpan padding_source;
} SerializedFileManagedValue;

typedef struct SerializedFileManagedSelectedType {
    size_t ordinal; /* First-use order within this host, not original type order. */
    SerializedFileMetadataTailReferenceTypeRow original;
    SerializedFileSchemaView schema;
    SerializedFileSchemaContext context;
} SerializedFileManagedSelectedType;

typedef enum SerializedFileManagedRegistryRowKind {
    SERIALIZED_FILE_MANAGED_REGISTRY_NULL = 1,
    SERIALIZED_FILE_MANAGED_REGISTRY_SELECTED_PAYLOAD
} SerializedFileManagedRegistryRowKind;

typedef struct SerializedFileManagedRegistryRow {
    size_t ordinal; /* Original registry wire order; duplicates remain distinct. */
    SerializedFileManagedRegistryRowKind kind;
    uint64_t rid_bits;
    SerializedFilePrefixSpan source;
    SerializedFilePrefixSpan payload_source; /* Present zero-byte span for null. */
    size_t rid_value_ordinal;
    size_t identity_value_ordinals[3]; /* Class, namespace, assembly strings. */
    size_t first_value;
    size_t value_count;
    size_t first_payload_value; /* first_value + 4, including the null boundary. */
    size_t payload_value_count;
    size_t selected_type_ordinal;            /* SIZE_MAX for null; owner-local otherwise. */
    SerializedFileReferenceMatch type_match; /* Complete internally rerun query. */
} SerializedFileManagedRegistryRow;

typedef enum SerializedFileManagedRidResolution {
    SERIALIZED_FILE_MANAGED_RID_MISSING = 0,
    SERIALIZED_FILE_MANAGED_RID_UNIQUE,
    SERIALIZED_FILE_MANAGED_RID_NULL,
    SERIALIZED_FILE_MANAGED_RID_AMBIGUOUS
} SerializedFileManagedRidResolution;

/* Equality is scoped to this owner's selected original object row/backing,
 * never a file-wide RID or a PathID-only identity. Every row is considered. */
typedef struct SerializedFileManagedRidSlot {
    size_t ordinal;
    size_t value_ordinal;
    size_t registry_row_ordinal; /* SIZE_MAX for a host field/array element. */
    uint64_t rid_bits;
    SerializedFileManagedRidResolution resolution;
    size_t match_count;
    size_t first_registry_row;  /* SIZE_MAX when missing. */
    size_t second_registry_row; /* SIZE_MAX unless ambiguous. */
} SerializedFileManagedRidSlot;

typedef struct SerializedFileManagedValuesView {
    SerializedFileDirectoryObjectRow object;
    SerializedFileSchemaView schema;
    SerializedFileSchemaContext context;
    SerializedFilePrefixSpan payload_source;
    SerializedFilePrefixSpan registry_source;
    SerializedFilePrefixSpan registry_padding_source; /* Absent for this profile. */
    uint32_t registry_version_bits;
    uint32_t registry_count_bits;
    size_t registry_version_value;
    size_t registry_count_value;
    size_t array_count_value;
    size_t array_element_count;
    size_t value_count;
    size_t registry_row_count;
    size_t rid_slot_count;
    size_t selected_type_count;
    size_t missing_rid_slots;
    size_t ambiguous_rid_slots;
    uint64_t consumed_bytes;
    uint64_t string_bytes; /* String content only, excluding length and padding. */
    uint64_t scalar_bytes; /* Integer/float/count/RID cells, excluding string lengths. */
    uint64_t padding_bytes;
    size_t retained_bytes;
} SerializedFileManagedValuesView;

typedef struct SerializedFileManagedValuesLimits {
    uint64_t max_payload_bytes;
    size_t max_values;
    size_t max_registry_rows;
    size_t max_rid_slots;
    size_t max_array_elements;
    size_t max_selected_types;
    uint64_t max_string_bytes;
    uint64_t max_total_string_bytes;
    uint64_t max_scalar_bytes;
    uint64_t max_total_scalar_bytes;
    uint64_t max_padding_bytes;
    size_t max_prerequisite_bytes;   /* P: index/cache/host and selected schemas. */
    size_t max_schema_scratch_bytes; /* S: one schema compiler's temporary request. */
    size_t max_retained_bytes;       /* R: final single allocation. */
    size_t max_heap_bytes;           /* Must cover checked reservation max(P+S, P+R). */
    uint64_t max_work;
    /* Child caps are real limits, not fresh enclosing work/residency budgets.
     * Each call also receives remaining outer work/prerequisite capacity. */
    SerializedFileSchemaLimits schema;
    SerializedFileReferenceIndexLimits index;
    SerializedFileReferenceQueryLimits query;
} SerializedFileManagedValuesLimits;

typedef enum SerializedFileManagedValuesStatus {
    SERIALIZED_FILE_MANAGED_VALUES_OK = 0,
    SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
    SERIALIZED_FILE_MANAGED_VALUES_INVALID_STATE,
    SERIALIZED_FILE_MANAGED_VALUES_SOURCE_MISMATCH,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENGINE,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENDIAN,
    SERIALIZED_FILE_MANAGED_VALUES_NO_EMBEDDED_SCHEMA,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_OBJECT_ALIGNMENT,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_OBJECT_SIZE,
    SERIALIZED_FILE_MANAGED_VALUES_INCOMPLETE_MAPPING,
    SERIALIZED_FILE_MANAGED_VALUES_SCHEMA_REJECTED,
    SERIALIZED_FILE_MANAGED_VALUES_CONTEXT_REJECTED,
    SERIALIZED_FILE_MANAGED_VALUES_INDEX_REJECTED,
    SERIALIZED_FILE_MANAGED_VALUES_QUERY_REJECTED,
    SERIALIZED_FILE_MANAGED_VALUES_EMPTY_REFERENCE_INDEX,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_REGISTRY_VERSION,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_COUNT,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_STRING_LENGTH,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_TERMINATOR,
    SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_NULL_ROW,
    SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_MISSING,
    SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_AMBIGUOUS,
    SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT,
    SERIALIZED_FILE_MANAGED_VALUES_TRAILING_OBJECT_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
    SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_FAILED,
    SERIALIZED_FILE_MANAGED_VALUES_INTERNAL_ERROR
} SerializedFileManagedValuesStatus;

typedef enum SerializedFileManagedValuesLimit {
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE = 0,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PAYLOAD_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_VALUES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_REGISTRY_ROWS,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RID_SLOTS,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_ARRAY_ELEMENTS,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SELECTED_TYPES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_STRING_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_STRING_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCALAR_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_SCALAR_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PADDING_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCHEMA_SCRATCH_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RETAINED_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK
} SerializedFileManagedValuesLimit;

typedef enum SerializedFileManagedValuesField {
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE = 0,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_TYPE,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_TYPE_NAME,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_FIELD_NAME,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_LENGTH,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_STRING_BYTES,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_PADDING,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_REGISTRY_VERSION,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_REGISTRY_COUNT,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_ARRAY_COUNT,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_RID_RESOLUTION,
    SERIALIZED_FILE_MANAGED_VALUES_FIELD_OBJECT_END
} SerializedFileManagedValuesField;

typedef struct SerializedFileManagedValuesResult {
    SerializedFileManagedValuesStatus status;
    SerializedFileManagedValuesLimit limit;
    SerializedFileManagedValuesField field;
    size_t object_ordinal;
    size_t type_ordinal;
    size_t node_ordinal;
    size_t registry_row_ordinal;
    uint64_t error_offset; /* File-slice-relative; UINT64_MAX when absent. */
    uint64_t work_used;
    size_t reserved_heap_bytes; /* Ceiling from partitions, not measured peak. */
    size_t required_retained_bytes;
    size_t peak_retained_bytes;       /* Successful final allocation request, even if freed. */
    size_t peak_prerequisite_bytes;   /* Observed live retained prerequisite requests. */
    size_t peak_schema_scratch_bytes; /* Largest actual nested scratch request. */
    bool schema_attempted;
    SerializedFileSchemaResult schema_result;
    bool context_attempted;
    SerializedFileSchemaContextResult context_result;
    bool index_attempted;
    SerializedFileReferenceIndexResult index_result;
    bool query_attempted;
    size_t query_registry_row_ordinal;
    SerializedFileReferenceQueryResult query_result;
    /* Valid only when query_attempted and the latest query_result.status is OK. */
    SerializedFileReferenceMatch type_match;
} SerializedFileManagedValuesResult;

void serialized_file_managed_values_init(SerializedFileManagedValues* values);
void serialized_file_managed_values_dispose(SerializedFileManagedValues* values);

/* Decode one complete exact35 LE ordinary ClassID 114 host with the evidenced
 * full 46-node managed-reference shape. The complete selected reference shapes
 * have 29 or 26 original nodes; their genuine selected contexts stop at node 8
 * or node 5 before the retained final registry subtree. Qualify every original
 * node's hierarchy/version/index/flags/size/metadata/opaque tail. Authored class
 * and field names, script identities, object names and PathIDs are not allowlists.
 * Unity structural control names retain exact bounded comparisons.
 *
 * Directory/Tail are genuine immutable owners from the same original header,
 * ordinary directory extent and type/object tables. Internally construct the
 * ordinary schema, its context and a nonempty genuine reference index. Select
 * each row by complete class/namespace/assembly byte equality; compile/cache
 * only uniquely selected original reference schemas and rerun their contexts.
 * Never accept caller-edited traversal/match observations as an input plan.
 *
 * Version must be exactly 2. Counts/string lengths must fit nonnegative signed 32-bit;
 * zero array and registry counts consume their single size cells. Reference-type
 * index count 0 is separately unsupported before registry bytes are read. The
 * reserved (Terminus,UnityEngine.DMAT,FAKE_ASM) tuple is refused before lookup.
 * A null row requires raw RID -2, all three empty names, and complete index MISSING.
 * Every UNIQUE identity consumes its selected payload, even with RID -2 or empty
 * names. An all-empty MISSING identity with another RID is UNSUPPORTED_NULL_ROW.
 * Other missing/ambiguous type selection is a typed construction failure: no
 * unknown payload is assumed empty. Other raw RID bits are preserved without
 * a positivity rule or a certificate of runtime-assigned ID validity.
 *
 * Integer and float32 bits are retained without host signed/float conversions.
 * Strings preserve arbitrary bytes and original length/padding. Ordinary RID
 * arrays are one i32 count followed by packed i64 cells. Only evidenced scalar
 * or string alignment executes. The special registry restores its original
 * 0x8001 node at exit, so its padding is absent; RefIds Array 0xc001 is not an
 * ordinary array traversal here. Selected nested registries consume no bytes.
 *
 * Parse every row and exactly exhaust the host before resolving slots by raw 64-bit
 * equality across every original row. Duplicate rows remain in original order;
 * match_count/first/second retain complete ambiguity with no selected winner.
 * A unique accepted null row yields RID_NULL. Missing/ambiguous slots are retained
 * unresolved observations, so successful byte decoding is not a fully resolved
 * runtime graph. Following slots never recursively decodes or duplicates payloads.
 *
 * All arguments are required. Initialize output once; an already live output
 * is rejected unchanged. Failure preserves inputs and output; disposal is
 * NULL-safe/idempotent and never reads borrowed storage. All mutable ranges,
 * handles/limits and parent retained allocations must be mutually disjoint and
 * disjoint from mapped bytes/static common strings. Parents may share their
 * immutable backing. Additional unobserved caller mappings remain a premise.
 * mapped_prefix must be the original header base; mapped_size cannot exceed
 * logical file size and must cover the complete four-aligned object, whose
 * byte count must not exceed INT32_MAX. No source bytes are copied or mutated.
 *
 * Prerequisites are temporary. Both payload passes and the final RID scan use
 * one cumulative work budget; nested calls receive min(child cap, remainder).
 * Before any allocation, check reservation max(P+S,P+R) against max_heap_bytes,
 * including overflow, for prerequisite P/schema scratch S/final retained R.
 * Every child retained request is bounded by remaining P; scratch by remaining
 * schema cap and S. These are conservative reserved ceilings, not exact peaks.
 * Zero is a real limit for every cap. Cleanup is budget-free. One final allocation retains
 * the view, primitive values, selected types, registry rows and RID slots.
 * Byte caps and allocation peaks count requested payload bytes, including layout
 * alignment; allocator bookkeeping, fixed local/stack storage and caller-owned
 * Directory/Tail/input bytes are excluded. max_heap_bytes is a conservative
 * payload-request reservation, not a process-memory or RSS measurement.
 *
 * Work charges: one admission and one owner publication; exact requested bytes
 * before cache/final allocation and initialization; one before each profile
 * node and name-length admission, plus each fixed-name byte compared; width+1
 * before every scalar/length/string/padding span in each payload pass (including
 * empty spans), one per completed primitive/row/slot; one per cache entry tested
 * and each RID row equality test plus one per completed resolution. Reserved
 * terminator checks charge one per reached component length and that component's
 * full byte length when equal, stopping at the first unequal component. Final
 * retained storage planning charges one per array layout. Nested schema, context,
 * index and query work follows their respective contracts and is added exactly
 * once. A failed charge consumes no units. Byte caps count
 * distinct source bytes per pass, not the sum of repeated visits. Source/cap/work
 * diagnostics identify the pending original node/field; absent ordinals/offsets
 * use SIZE_MAX/UINT64_MAX. Nested records describe the latest attempted call;
 * query_registry_row_ordinal identifies its row. Only attempted records are valid.
 * INTERNAL_ERROR refuses a violated replay invariant without publishing output.
 *
 * After success parents and prerequisite schemas/index may be disposed. Getter
 * pointers need this final owner; copied records contain no prerequisite pointers.
 * Their file spans still require immutable original backing, and schema strings
 * retain their tagged file/common-buffer spaces. Accessors are O(1), return NULL
 * for empty owners/out-of-range ordinals, and never extend backing lifetimes.
 * No general TypeTree/PPtr/script/class/native/project recovery claim follows.
 */
SerializedFileManagedValuesResult serialized_file_managed_values_create(
    const SerializedFileDirectory* directory,
    const SerializedFileMetadataTail* tail,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileManagedValuesLimits* limits,
    SerializedFileManagedValues* out_values);

const SerializedFileManagedValuesView* serialized_file_managed_values_view(
    const SerializedFileManagedValues* values);
/* Exact retained allocation for disjoint-range admission only; never interpret
 * its representation or write through it. A NULL/empty owner returns {NULL,0}.
 * A genuine live owner returns its complete allocation without reading borrowed
 * backing, allocating or charging work. The range expires on owner disposal.
 * This accessor does not expose input backing or temporary prerequisite ranges. */
SerializedFileManagedValuesStorageRange serialized_file_managed_values_storage_range(
    const SerializedFileManagedValues* values);
const SerializedFileManagedValue* serialized_file_managed_values_value(
    const SerializedFileManagedValues* values, size_t ordinal);
const SerializedFileManagedSelectedType* serialized_file_managed_values_selected_type(
    const SerializedFileManagedValues* values, size_t ordinal);
const SerializedFileManagedRegistryRow* serialized_file_managed_values_registry_row(
    const SerializedFileManagedValues* values, size_t ordinal);
const SerializedFileManagedRidSlot* serialized_file_managed_values_rid_slot(
    const SerializedFileManagedValues* values, size_t ordinal);

#ifdef __cplusplus
}
#endif
#endif
