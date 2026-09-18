// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_SCHEMA_H
#define SERIALIZED_FILE_SCHEMA_H

#include "io/serialized_file_metadata_tail.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SerializedFileSchema {
    void* implementation;
} SerializedFileSchema;

typedef enum SerializedFileSchemaRowKind {
    SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE = 1,
    SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE
} SerializedFileSchemaRowKind;

typedef enum SerializedFileSchemaStringSpace {
    SERIALIZED_FILE_SCHEMA_STRING_LOCAL = 1,
    SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35
} SerializedFileSchemaStringSpace;

typedef struct SerializedFileSchemaString {
    SerializedFileSchemaStringSpace space;
    uint32_t encoded_offset; /* Original node word, including its common bit. */
    const uint8_t* bytes;
    size_t byte_count; /* Excludes the terminating NUL; may be zero. */
    /* Exactly byte_count+1 bytes. Offset is file-slice-relative for LOCAL and
     * common-buffer-relative for COMMON_EXACT35. space must accompany it. */
    uint64_t source_offset;
} SerializedFileSchemaString;

typedef struct SerializedFileSchemaNode {
    size_t ordinal;
    SerializedFilePrefixSpan source; /* Original complete 32-byte record. */
    uint16_t version;
    uint8_t level;
    uint8_t type_flags;
    uint32_t type_offset_bits;
    uint32_t name_offset_bits;
    uint32_t byte_size_bits;
    uint32_t index_bits;
    uint32_t meta_flags;
    uint8_t opaque_tail[8]; /* Original byte sequence; never endian-swapped. */
    SerializedFileSchemaString type_name;
    SerializedFileSchemaString field_name;
    size_t parent;       /* SIZE_MAX only at the root. */
    size_t first_child;  /* SIZE_MAX when absent. */
    size_t next_sibling; /* SIZE_MAX when absent. */
    size_t child_count;
    size_t subtree_end; /* Exclusive original ordinal, <= node_count. */
} SerializedFileSchemaNode;

typedef struct SerializedFileSchemaView {
    SerializedFileDirectoryEngineVersion engine_version;
    SerializedFilePrefixView prefix; /* Borrowed file bytes; copied descriptor. */
    SerializedFileSchemaRowKind row_kind;
    size_t type_ordinal;
    SerializedFilePrefixSpan type_entry_source;
    SerializedFileDirectoryTree tree;
    uint32_t class_id_bits;
    uint16_t script_index_bits;
    uint8_t stripped_raw;
    bool has_script_hash;
    SerializedFilePrefixSpan script_hash_source;
    SerializedFilePrefixSpan type_hash_source;
    /* NUL-inclusive metadata-tail identity spans for a reference type;
     * canonical absent spans for an ordinary type. */
    SerializedFilePrefixSpan class_name_source;
    SerializedFilePrefixSpan namespace_source;
    SerializedFilePrefixSpan assembly_name_source;
    SerializedFilePrefixSpan dependency_count_source; /* Absent for reference rows. */
    SerializedFilePrefixSpan dependency_words_source; /* Absent for reference rows. */
    uint32_t dependency_count;
    size_t node_count;
    size_t maximum_depth; /* Root depth0; maximum hierarchy level. */
    size_t retained_bytes;
} SerializedFileSchemaView;

typedef struct SerializedFileSchemaLimits {
    uint64_t max_nodes;
    uint64_t max_local_string_bytes;
    size_t max_depth; /* Root depth0. Zero permits only the root. */
    size_t max_retained_bytes;
    size_t max_scratch_bytes;
    uint64_t max_work;
} SerializedFileSchemaLimits;

typedef enum SerializedFileSchemaStatus {
    SERIALIZED_FILE_SCHEMA_OK = 0,
    SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT,
    SERIALIZED_FILE_SCHEMA_INVALID_STATE,
    SERIALIZED_FILE_SCHEMA_UNSUPPORTED_ENGINE,
    SERIALIZED_FILE_SCHEMA_NO_EMBEDDED_TREE,
    SERIALIZED_FILE_SCHEMA_SOURCE_MISMATCH,
    SERIALIZED_FILE_SCHEMA_MALFORMED_HIERARCHY,
    SERIALIZED_FILE_SCHEMA_INVALID_STRING_OFFSET,
    SERIALIZED_FILE_SCHEMA_UNTERMINATED_STRING,
    SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED,
    SERIALIZED_FILE_SCHEMA_ALLOCATION_FAILED
} SerializedFileSchemaStatus;

typedef enum SerializedFileSchemaLimit {
    SERIALIZED_FILE_SCHEMA_LIMIT_NONE = 0,
    SERIALIZED_FILE_SCHEMA_LIMIT_NODES,
    SERIALIZED_FILE_SCHEMA_LIMIT_LOCAL_STRINGS,
    SERIALIZED_FILE_SCHEMA_LIMIT_DEPTH,
    SERIALIZED_FILE_SCHEMA_LIMIT_RETAINED,
    SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH,
    SERIALIZED_FILE_SCHEMA_LIMIT_WORK
} SerializedFileSchemaLimit;

typedef enum SerializedFileSchemaErrorSpace {
    SERIALIZED_FILE_SCHEMA_ERROR_NO_SOURCE = 0,
    SERIALIZED_FILE_SCHEMA_ERROR_FILE,
    SERIALIZED_FILE_SCHEMA_ERROR_COMMON_EXACT35
} SerializedFileSchemaErrorSpace;

typedef enum SerializedFileSchemaField {
    SERIALIZED_FILE_SCHEMA_FIELD_NONE = 0,
    SERIALIZED_FILE_SCHEMA_FIELD_NODE_RECORD,
    SERIALIZED_FILE_SCHEMA_FIELD_LEVEL,
    SERIALIZED_FILE_SCHEMA_FIELD_TYPE_OFFSET,
    SERIALIZED_FILE_SCHEMA_FIELD_NAME_OFFSET,
    SERIALIZED_FILE_SCHEMA_FIELD_STRING_TERMINATOR
} SerializedFileSchemaField;

typedef struct SerializedFileSchemaResult {
    SerializedFileSchemaStatus status;
    SerializedFileSchemaLimit limit;
    SerializedFileSchemaField field;
    SerializedFileSchemaErrorSpace error_space;
    size_t type_ordinal;   /* SIZE_MAX outside a selected original row. */
    size_t node_ordinal;   /* SIZE_MAX outside a selected original node. */
    uint64_t error_offset; /* Coordinates selected by error_space; MAX if absent. */
    uint64_t work_used;
    size_t required_retained_bytes;
    size_t required_scratch_bytes;
    size_t peak_retained_bytes;
    size_t peak_scratch_bytes;
} SerializedFileSchemaResult;

void serialized_file_schema_init(SerializedFileSchema* schema);
void serialized_file_schema_dispose(SerializedFileSchema* schema);

/* Compile one genuine parent's original ordinary/reference tree in exact35.
 * All pointers are required. Profile, metadata byte order, row kind and every
 * source descriptor are inherited, never caller overrides. The separate
 * exact29 physical-parent profile is UNSUPPORTED_ENGINE. An absent tree is
 * NO_EMBEDDED_TREE, never a synthesized schema or empty successful tree.
 *
 * Initialize output once; a live output is INVALID_STATE and remains live.
 * Every failure leaves an initially empty output empty and all inputs unchanged.
 * Success owns one aligned node allocation. Names and sources borrow unchanged
 * file bytes or the fixed exact35 common buffer; no backing buffer is copied.
 * The parent is needed only during construction. Row pointers require this
 * owner; copied View and String descriptors contain no pointers into it.
 * The original backing must outlive borrowed file spans. Dispose is NULL-safe,
 * idempotent and does not inspect either parents or borrowed source bytes.
 * Do not copy/reinitialize a live handle or forge a parent owning handle.
 *
 * Validate pointer/handle/limit disjointness first, then genuine parent storage,
 * its known readable metadata prefix, common-buffer ranges, and output state.
 * Reject aliases among all these accessible ranges before charging work.
 * The parent retains only its parsed metadata extent: disjointness from any
 * additional caller mapping remains a caller precondition, not a discovered
 * complete-file mapping claim. Source descriptors are trusted under the genuine
 * immutable-parent premise; no previously parsed byte is reauthenticated.
 *
 * After one admission unit, check engine, original row, tree presence, node cap,
 * local-byte cap and complete node/string extents in that order. Plan retained
 * then scratch storage; report each required size when known. Enforce both caps
 * before allocating. Every cap includes zero; no implicit unlimited budget.
 * Peaks report actual successful heap requests even if freed on failure;
 * retained/scratch sizes count requested heap payload bytes, including layout
 * alignment. They exclude allocator bookkeeping, caller storage and fixed
 * local arrays (two 256-entry size_t hierarchy arrays and one 256-entry size_t
 * radix array), as well as local descriptors.
 *
 * Preserve all scalar bits, empty names, unknown flags/versions/byte sizes and
 * the opaque final eight node bytes. Selected byte order affects only scalar
 * words in the first 24 bytes. Require one level-zero root; later level zero or
 * a level jump greater than one is MALFORMED_HIERARCHY. Check that before the
 * caller's depth cap. Root depth is zero, so max_depth=0 permits a sole root.
 * Derive original parent/child/sibling links and exclusive subtree boundaries.
 * SIZE_MAX denotes absent relationships; subtree_end can equal node_count.
 * No value type, managed-reference context, node flag meaning or object layout
 * is admitted by these structural relationships.
 *
 * A name's high offset bit selects the exact35 common table; remaining bits
 * select its exact NUL-terminated string start. Local and common source spaces
 * remain distinct even for equal bytes. Referenced mid-string/outside offsets
 * are INVALID_STRING_OFFSET; referenced unterminated tails are UNTERMINATED_STRING.
 * Unreferenced trailing bytes and empty strings remain lossless observations.
 * Both whole string buffers are scanned, including the common table's final
 * extra NUL. This neither validates UTF-8 nor accepts empty names for values.
 *
 * Compilation is O(N+S+C) for nodes N, local bytes S and fixed common bytes C.
 * Two bounded request arrays in one scratch allocation hold 2*N names; four
 * stable byte-radix passes establish offset order. A fixed 256-entry ancestor
 * array and last-child array process hierarchy; a separate fixed 256-bucket
 * array sorts requests. No recursion, repeated subtree scan or per-name suffix
 * search runs. All node/string/relationship ordinals remain original.
 *
 * Work charges one admission and one planning unit; each exact allocation size
 * before allocation/initialization; 33 units before each complete node decode
 * and link preparation; one before each subtree closes; 512 per radix pass
 * before bucket initialization/prefix calculation; one per request count and
 * scatter; one per inspected string byte; one per resolved/invalid name request;
 * and one before publication. Success is R+T+52*N+S+C+2051, with retained bytes
 * R and scratch bytes T. Failed charges consume no work. Failure diagnostics
 * identify the next charged field/byte; byte-buffer scans use node=SIZE_MAX.
 * Local invalid-offset requests point to their original node offset word;
 * common ones retain common-buffer offsets. Missing NULs identify the selected
 * buffer's exclusive end. Owner/planning/allocation/publication failures have
 * FIELD_NONE, no source, node=SIZE_MAX and offset=UINT64_MAX. type_ordinal is
 * retained after a valid original row is selected, including on success.
 *
 * No object bytes, RID relations, reference identity lookup, external schema
 * substitution, registry omission, alignment or value-consumption rule is
 * implemented. Those require separate authenticated context/value owners.
 */
SerializedFileSchemaResult serialized_file_schema_create_ordinary(
    const SerializedFileDirectory* directory,
    size_t ordinary_type_ordinal,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* out_schema);

SerializedFileSchemaResult serialized_file_schema_create_reference(
    const SerializedFileMetadataTail* tail,
    size_t reference_type_ordinal,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* out_schema);

const SerializedFileSchemaView* serialized_file_schema_view(const SerializedFileSchema* schema);
const SerializedFileSchemaNode* serialized_file_schema_node(
    const SerializedFileSchema* schema, size_t node_ordinal);

#ifdef __cplusplus
}
#endif

#endif
