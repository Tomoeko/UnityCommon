// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_SCHEMA_CONTEXT_H
#define SERIALIZED_FILE_SCHEMA_CONTEXT_H

#include "io/serialized_file_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SerializedFileSchemaContextKind {
    SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT = 1,
    SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD
} SerializedFileSchemaContextKind;

typedef struct SerializedFileSchemaContext {
    SerializedFileSchemaContextKind kind;
    SerializedFileDirectoryEngineVersion engine_version;
    SerializedFileSchemaRowKind row_kind;
    size_t type_ordinal;
    SerializedFilePrefixSpan header_source;
    uint64_t file_size;
    uint8_t endian_selector;
    SerializedFilePrefixSpan type_entry_source;
    SerializedFileDirectoryTree tree;
    size_t root;          /* Original root ordinal, always zero. */
    size_t first_child;   /* Original root child; SIZE_MAX when absent. */
    size_t traversal_end; /* Exclusive preorder boundary, never a byte count. */
    size_t registry;      /* Original mask0x04 node; SIZE_MAX when absent. */
    size_t registry_end;  /* Exclusive original subtree end; SIZE_MAX when absent. */
    bool registry_omitted;
} SerializedFileSchemaContext;

typedef enum SerializedFileSchemaContextStatus {
    SERIALIZED_FILE_SCHEMA_CONTEXT_OK = 0,
    SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT,
    SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_STATE,
    SERIALIZED_FILE_SCHEMA_CONTEXT_WRONG_ROW_KIND,
    SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_FLAGS,
    SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION,
    SERIALIZED_FILE_SCHEMA_CONTEXT_WORK_LIMIT
} SerializedFileSchemaContextStatus;

typedef enum SerializedFileSchemaContextField {
    SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE = 0,
    SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_TYPE_FLAGS,
    SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_LEVEL
} SerializedFileSchemaContextField;

typedef struct SerializedFileSchemaContextResult {
    SerializedFileSchemaContextStatus status;
    SerializedFileSchemaContextField field;
    size_t type_ordinal;   /* Original row; SIZE_MAX before a live schema is admitted. */
    size_t node_ordinal;   /* Original node; SIZE_MAX outside a node diagnostic. */
    uint64_t error_offset; /* File-slice-relative original field, UINT64_MAX if absent. */
    uint64_t work_used;
} SerializedFileSchemaContextResult;

/* Qualify traversal boundaries of one genuine live exact35 schema. All pointers
 * are required; kind must name one of the two explicit modes. Ordinary mode
 * requires an original ordinary row and includes its complete root subtree.
 * Selected-reference mode requires an original reference row and describes
 * the root's direct-child walk after a separately proven reference selection.
 * This query has no host or identity-index input and cannot prove that selection.
 *
 * A registry is identified here only by type_flags mask0x04, not by its name or
 * internal value shape. No registry is required: without one, both boundaries
 * are node_count. With one final direct child whose complete flags equal0x04,
 * ordinary mode includes it; selected-reference mode stops before its original
 * ordinal and marks its complete retained subtree omitted. first_child always
 * remains the original root child; equality with traversal_end means an empty
 * selected child prefix. No node, source descriptor or relationship is changed.
 *
 * Root/nested/nonfinal/multiple registry nodes and combined registry flags are
 * explicitly unsupported. These conservative policies do not claim Unity's
 * rejection behavior. Other flags and raw node properties remain unclassified;
 * success does not admit arrays, RID slots, registry fields, zero-size leaves,
 * scalar widths, alignment, payload versions/bytes, null rules or object values.
 * This query performs no string lookup, raw-byte decoding or allocation.
 *
 * Validate arguments and handle/output disjointness, genuine owner storage,
 * the readable file prefix through the selected original row, the static
 * common table and output ranges, then original row kind, before charging work.
 * All accessible input/owner ranges are mutually disjoint. Additional caller
 * backing beyond that known prefix must also remain disjoint from output; this
 * query does not discover a complete-file mapping. Do not forge/copy live
 * handles or mutate the schema/backing. An empty schema is INVALID_STATE.
 *
 * Charge one admission unit, one before each original node's registry-flag and
 * relationship inspection, and one before fixed-output publication. Success
 * costs exactly node_count+2. Failed charges consume no units; zero is a real
 * limit. Within a node, combined flags fail before placement. Diagnostics use
 * that node's original flag/level byte; a pending node work charge names its
 * flag byte. Owner/admission/publication failures have FIELD_NONE, absent node
 * and offset. A live schema's original type ordinal remains in later results.
 *
 * out_context needs no initialization or disposal and is unchanged on every
 * failure. Success contains only values and borrowed raw source descriptors;
 * no pointer refers into schema storage. Parent owners are unnecessary. Node
 * access needs the live schema; copied spans require unchanged original backing.
 * After backing disposal only by-value counts/coordinates/profile remain usable.
 *
 * This caller-editable record is an observation, never an accepted executable
 * plan. A future value consumer must rerun this query from a genuine schema or
 * keep a qualified plan privately in a genuine owner. Matching source spans or
 * ordinals alone does not authenticate caller-provided traversal fields.
 */
SerializedFileSchemaContextResult serialized_file_schema_context_query(
    const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind kind,
    uint64_t max_work,
    SerializedFileSchemaContext* out_context);

#ifdef __cplusplus
}
#endif

#endif
