// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_REFERENCE_INDEX_H
#define SERIALIZED_FILE_REFERENCE_INDEX_H

#include "io/serialized_file_metadata_tail.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SerializedFileReferenceIndex {
    void* implementation;
} SerializedFileReferenceIndex;

typedef struct SerializedFileReferenceIndexView {
    SerializedFileDirectoryEngineVersion engine_version;
    SerializedFilePrefixView prefix;
    SerializedFilePrefixSpan tail_source;
    SerializedFilePrefixSpan reference_type_count_source;
    SerializedFilePrefixSpan reference_type_rows_source;
    size_t reference_type_count;
    uint64_t identity_source_bytes; /* Aggregate source bytes including three NULs per row. */
    size_t retained_bytes;
} SerializedFileReferenceIndexView;

typedef struct SerializedFileReferenceIndexLimits {
    size_t max_reference_types;
    uint64_t max_identity_source_bytes;
    size_t max_retained_bytes;
    uint64_t max_work;
} SerializedFileReferenceIndexLimits;

typedef enum SerializedFileReferenceIndexStatus {
    SERIALIZED_FILE_REFERENCE_INDEX_OK = 0,
    SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT,
    SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE,
    SERIALIZED_FILE_REFERENCE_INDEX_UNSUPPORTED_ENGINE,
    SERIALIZED_FILE_REFERENCE_INDEX_IDENTITIES_UNAVAILABLE,
    SERIALIZED_FILE_REFERENCE_INDEX_SOURCE_MISMATCH,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED,
    SERIALIZED_FILE_REFERENCE_INDEX_ALLOCATION_FAILED
} SerializedFileReferenceIndexStatus;

typedef enum SerializedFileReferenceIndexLimit {
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE = 0,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_REFERENCE_TYPES,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_IDENTITY_SOURCE_BYTES,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_RETAINED_BYTES,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_COMPONENT_BYTES,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_KEY_BYTES,
    SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK
} SerializedFileReferenceIndexLimit;

typedef enum SerializedFileReferenceIdentityComponent {
    SERIALIZED_FILE_REFERENCE_IDENTITY_NONE = 0,
    SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS,
    SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE,
    SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY
} SerializedFileReferenceIdentityComponent;

typedef struct SerializedFileReferenceIndexResult {
    SerializedFileReferenceIndexStatus status;
    SerializedFileReferenceIndexLimit limit;
    SerializedFileReferenceIdentityComponent component;
    size_t reference_type_count; /* Original count once known, including unavailable identities. */
    size_t row_ordinal;          /* SIZE_MAX outside a selected original row. */
    uint64_t error_offset;       /* File-slice-relative; UINT64_MAX without a source field. */
    uint64_t work_used;
    size_t required_retained_bytes;
    size_t peak_retained_bytes;
} SerializedFileReferenceIndexResult;

typedef struct SerializedFileReferenceKeyPart {
    const uint8_t* bytes;
    size_t size; /* Exact bytes, no required terminator; zero permits NULL. */
} SerializedFileReferenceKeyPart;

typedef struct SerializedFileReferenceKey {
    SerializedFileReferenceKeyPart class_name;
    SerializedFileReferenceKeyPart namespace_name;
    SerializedFileReferenceKeyPart assembly_name;
} SerializedFileReferenceKey;

typedef struct SerializedFileReferenceQueryLimits {
    size_t max_component_bytes;
    size_t max_key_bytes;
    uint64_t max_work;
} SerializedFileReferenceQueryLimits;

typedef enum SerializedFileReferenceMatchKind {
    SERIALIZED_FILE_REFERENCE_MISSING = 0,
    SERIALIZED_FILE_REFERENCE_UNIQUE,
    SERIALIZED_FILE_REFERENCE_AMBIGUOUS
} SerializedFileReferenceMatchKind;

typedef struct SerializedFileReferenceMatch {
    SerializedFileReferenceMatchKind kind;
    size_t match_count;
    size_t first_ordinal;  /* SIZE_MAX if missing. */
    size_t second_ordinal; /* SIZE_MAX unless ambiguous. */
} SerializedFileReferenceMatch;

typedef struct SerializedFileReferenceQueryResult {
    SerializedFileReferenceIndexStatus status;
    SerializedFileReferenceIndexLimit limit;
    SerializedFileReferenceIdentityComponent component;
    size_t row_ordinal; /* Next original row on row/component work exhaustion; MAX otherwise. */
    uint64_t work_used;
} SerializedFileReferenceQueryResult;

/* Build one exact35 original-occurrence catalogue from a genuine live Tail.
 * This index uses a complete linear query, not a sorted or hashed lookup.
 * All pointers are required. Initialize output once; live output is rejected.
 * Failure leaves output and all inputs unchanged. Success owns one aligned
 * descriptor allocation, no heap scratch, and no copied file/schema bytes.
 * The Tail is needed only during construction. Index rows/views require this
 * owner; copied descriptors require only the original immutable file backing.
 * Disposal accepts NULL/empty, resets the handle and never reads parent/backing.
 * Do not copy, forge or reinitialize a live owning handle.
 *
 * Before work, reject aliases among handles/limits/output, the genuine Tail's
 * retained allocation and its known readable metadata prefix. Disjointness
 * from additional caller mapping remains a caller premise. Borrowed storage
 * must remain immutable/readable throughout the call and lifetime of its spans.
 * No previously parsed byte is reread or authenticated by this constructor.
 *
 * After one admission unit: inherit exact35 (exact29 is unsupported), retain
 * original count, enforce row cap, then distinguish absent identities. A
 * nonempty tree-free Tail is IDENTITIES_UNAVAILABLE, never an empty successful
 * index or rows of empty identity strings. A genuine zero-row Tail, including
 * tree-free, constructs a successful empty index. Every original occurrence,
 * duplicate identity and raw descriptor is preserved in original order.
 *
 * Each charged preflight row checks complete original/name source bounds and
 * physical ordering, including three NUL-inclusive names ending at row end.
 * Lengths come from the genuine parent's terminated spans; there is no strlen,
 * terminator reread, Unicode policy or schema/value acceptance. Sum all name
 * source bytes toward max_identity_source_bytes, including every NUL. Check
 * this aggregate in class/namespace/assembly order. Plan one checked aligned
 * header+rows allocation, enforce retained cap, allocate/initialize, copy each
 * original descriptor and publish only after complete replay. All caps include
 * zero. Peaks count actual successful heap payload requests; allocator metadata
 * and fixed locals are excluded. Cleanup requires no further work budget.
 *
 * Work is one admission, one per preflight row, one planning, the complete
 * allocation request before initialization, one per copied row, one publication:
 * exact success A+2*N+3 for retained bytes A and original rows N. Failed charges
 * consume no work. Row charges report that ordinal and original row start;
 * identity source refusals report their component/offset. Owner/planning/
 * allocation/publication diagnostics use component NONE, row SIZE_MAX and
 * offset UINT64_MAX. Counts are known only after exact35 selection succeeds.
 * No query observation is an authenticated plan for a later value constructor.
 */
void serialized_file_reference_index_init(SerializedFileReferenceIndex* index);
void serialized_file_reference_index_dispose(SerializedFileReferenceIndex* index);
SerializedFileReferenceIndexResult serialized_file_reference_index_create(
    const SerializedFileMetadataTail* tail,
    const SerializedFileReferenceIndexLimits* limits,
    SerializedFileReferenceIndex* out_index);

const SerializedFileReferenceIndexView* serialized_file_reference_index_view(
    const SerializedFileReferenceIndex* index);
const SerializedFileMetadataTailReferenceTypeRow* serialized_file_reference_index_row(
    const SerializedFileReferenceIndex* index, size_t original_ordinal);

/* Query a genuine live index with three exact byte+length components, compared
 * in class/namespace/assembly order. Key parts need no terminator; zero permits
 * NULL. Embedded NUL, empty, non-UTF-8 and case-different bytes are not rewritten.
 * The all-empty tuple is an ordinary key, never a missing-identity/RID marker.
 * All pointer arguments are required. Nonempty keys must remain immutable and
 * readable during this call. Read-only key bytes may alias each other or the
 * index's borrowed file bytes. Typed handles/key/limits and mutable output must
 * be disjoint from one another and accessible retained/backing ranges. Output
 * also must not overlap any nonempty key byte range. Reject wrapping ranges;
 * unobserved caller mapping remains a disjointness premise.
 *
 * After one admission unit, check each component cap and checked aggregate key
 * cap in class/namespace/assembly order before reading any key byte. Then scan
 * every row. One unit admits a row; one unit admits each reached length test.
 * Unequal lengths read neither component's bytes. Equal nonzero lengths admit
 * that full byte count before memcmp; an earlier byte difference is not a work
 * refund. Empty matches never pass NULL to memcmp. Stop a row at its first
 * unequal component, but scan all rows even after a second identity match.
 * One unit precedes publication. Success costs exactly 2+N+C+B, where C counts
 * reached component-length tests and B admitted equal-length byte comparisons.
 * All caps include zero; failed charges consume no work. Exhaustion reports the
 * next original row/component, never a partial successful match observation.
 *
 * Missing/unique/ambiguous are complete outcomes, all with result.status OK.
 * match_count includes every original occurrence. first_ordinal is SIZE_MAX
 * only when missing; second_ordinal is SIZE_MAX unless ambiguous. Additional
 * equal rows are counted even when every schema/hash/descriptor byte agrees.
 * All query failures preserve out_match. Its mutable contents are observations,
 * never input proof to a value/schema constructor. Such a consumer must rerun
 * lookup from genuine owners/raw context, require UNIQUE, and verify the
 * selected genuine schema's complete original-row/backing lineage internally.
 * No value shape, RID/null semantics, payload skipping or capture identity is
 * implied by a match or by MISSING.
 */
SerializedFileReferenceQueryResult serialized_file_reference_index_query(
    const SerializedFileReferenceIndex* index,
    const SerializedFileReferenceKey* key,
    const SerializedFileReferenceQueryLimits* limits,
    SerializedFileReferenceMatch* out_match);

#ifdef __cplusplus
}
#endif
#endif
