// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_VALUES_H
#define SERIALIZED_FILE_VALUES_H

#include "io/serialized_file_schema_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SerializedFileValues {
    void* implementation;
} SerializedFileValues;

typedef enum SerializedFileValueKind {
    SERIALIZED_FILE_VALUE_CONTAINER = 1,
    SERIALIZED_FILE_VALUE_BYTE_STRING,
    SERIALIZED_FILE_VALUE_SIGNED_INTEGER,
    SERIALIZED_FILE_VALUE_UNSIGNED_INTEGER
} SerializedFileValueKind;

typedef struct SerializedFileValue {
    size_t ordinal; /* Logical preorder; distinct from schema-node ordinal. */
    SerializedFileValueKind kind;
    SerializedFileSchemaNode schema_node; /* Copied; no pointer into schema storage. */
    size_t parent;
    size_t first_child;
    size_t next_sibling;
    size_t child_count;
    size_t subtree_end;
    SerializedFilePrefixSpan source; /* Complete consumed interval, including padding. */
    /* String-only fields. Other kinds use SIZE_MAX ordinals and absent spans. */
    size_t array_schema_ordinal;
    size_t size_schema_ordinal;
    size_t data_schema_ordinal;
    SerializedFilePrefixSpan array_schema_source;
    SerializedFilePrefixSpan size_schema_source;
    SerializedFilePrefixSpan data_schema_source;
    uint32_t length_bits;
    SerializedFilePrefixSpan length_source;
    SerializedFilePrefixSpan bytes_source;
    /* Executed string/scalar alignment, including present zero-byte padding.
     * Absent when no admitted alignment control applies. */
    SerializedFilePrefixSpan padding_source;
    /* Integer-only raw bits, zero extended to 64 bits. integer_source.size is
     * the original width (1, 4 or 8); nonintegers have zero bits/absent span.
     * SIGNED_INTEGER records preserve two's-complement bits without a host
     * unsigned-to-signed conversion. source includes any executed padding. */
    uint64_t integer_bits;
    SerializedFilePrefixSpan integer_source;
} SerializedFileValue;

typedef struct SerializedFileValuesView {
    SerializedFileDirectoryObjectRow object;
    SerializedFileSchemaView schema;
    SerializedFileSchemaContext context;
    size_t value_count;   /* TextAsset3; admitted ClassID114 profiles10 or12. */
    size_t maximum_depth; /* Root depth0; TextAsset1 or ClassID114 profile2. */
    uint64_t consumed_bytes;
    uint64_t string_bytes;
    uint64_t padding_bytes;
    size_t retained_bytes;
    uint64_t integer_bytes; /* Distinct scalar bytes, excluding padding/lengths. */
} SerializedFileValuesView;

typedef struct SerializedFileValuesLimits {
    uint64_t max_payload_bytes;
    size_t max_values;
    size_t max_depth;
    uint64_t max_string_bytes; /* Per string. */
    uint64_t max_total_string_bytes;
    uint64_t max_padding_bytes;
    size_t max_retained_bytes;
    uint64_t max_work;
    uint64_t max_integer_bytes; /* Per scalar; zero admits no integer bytes. */
    uint64_t max_total_integer_bytes;
} SerializedFileValuesLimits;

typedef enum SerializedFileValuesStatus {
    SERIALIZED_FILE_VALUES_OK = 0,
    SERIALIZED_FILE_VALUES_INVALID_ARGUMENT,
    SERIALIZED_FILE_VALUES_INVALID_STATE,
    SERIALIZED_FILE_VALUES_SOURCE_MISMATCH,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_ENGINE,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_ENDIAN,
    SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA,
    SERIALIZED_FILE_VALUES_CONTEXT_REJECTED,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_ALIGNMENT,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_SIZE,
    SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING,
    SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT,
    SERIALIZED_FILE_VALUES_UNSUPPORTED_STRING_LENGTH,
    SERIALIZED_FILE_VALUES_TRAILING_OBJECT_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
    SERIALIZED_FILE_VALUES_ALLOCATION_FAILED
} SerializedFileValuesStatus;

typedef enum SerializedFileValuesLimit {
    SERIALIZED_FILE_VALUES_LIMIT_NONE = 0,
    SERIALIZED_FILE_VALUES_LIMIT_PAYLOAD_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_VALUES,
    SERIALIZED_FILE_VALUES_LIMIT_DEPTH,
    SERIALIZED_FILE_VALUES_LIMIT_STRING_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_TOTAL_STRING_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_PADDING_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_RETAINED_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_WORK,
    SERIALIZED_FILE_VALUES_LIMIT_INTEGER_BYTES,
    SERIALIZED_FILE_VALUES_LIMIT_TOTAL_INTEGER_BYTES
} SerializedFileValuesLimit;

typedef enum SerializedFileValuesField {
    SERIALIZED_FILE_VALUES_FIELD_NONE = 0,
    SERIALIZED_FILE_VALUES_FIELD_OBJECT,
    SERIALIZED_FILE_VALUES_FIELD_TYPE,
    SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
    SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME,
    SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME,
    SERIALIZED_FILE_VALUES_FIELD_CONTEXT,
    SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH,
    SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES,
    SERIALIZED_FILE_VALUES_FIELD_PADDING,
    SERIALIZED_FILE_VALUES_FIELD_OBJECT_END,
    SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES
} SerializedFileValuesField;

typedef struct SerializedFileValuesResult {
    SerializedFileValuesStatus status;
    SerializedFileValuesLimit limit;
    SerializedFileValuesField field;
    size_t object_ordinal; /* SIZE_MAX until an original object is selected. */
    size_t type_ordinal;
    size_t node_ordinal;
    uint64_t error_offset; /* File-slice-relative; UINT64_MAX when absent. */
    uint64_t work_used;
    size_t required_retained_bytes;
    size_t peak_retained_bytes;
    bool context_attempted;
    SerializedFileSchemaContextResult context_result;
} SerializedFileValuesResult;

void serialized_file_values_init(SerializedFileValues* values);
void serialized_file_values_dispose(SerializedFileValues* values);

/* Decode exactly one ordinary TextAsset in exact35 little endian. This initial
 * subset requires class49, unstripped/no script hash/script index -1, and the
 * complete observed nine-node TextAsset/Base, string/m_Name, Array/Array,
 * int/size, char/data, string/m_Script, Array/Array, int/size, char/data tree.
 * Require observed versions1, original indices0..8, levels, child relationships,
 * flags, sizes, meta flags and zero opaque tails. Names compare bounded bytes;
 * their original local/common encodings remain intact. Unknown alternatives
 * are unsupported policies, not claims about Unity's rejection behavior.
 *
 * All pointers are required. directory/schema must be genuine immutable owners;
 * schema must be the selected object's original ordinary row in the same file
 * backing, with identical complete type/tree sources. A tree-disabled row returns
 * NO_EMBEDDED_SCHEMA before requiring a live schema. Initialize output once;
 * never copy/reinitialize live owners. Every failure preserves all inputs and
 * output (including an already live output). Dispose is NULL-safe, idempotent
 * and reads no borrowed bytes. Accessors are O(1) and return NULL for empty
 * owners or invalid value ordinals.
 *
 * mapped_prefix must equal the original directory header backing. mapped_size
 * is an actually readable prefix, no larger than logical file size, covering
 * the complete selected object. Immutable backing must outlive every borrowed
 * span. Directory and schema may be disposed after success; no returned pointer
 * refers into either parent's storage. Copied schema.retained_bytes describes
 * that source owner, not this allocation. Handles, limits, output, both owner
 * allocations, mapped input, and static common bytes must be mutually disjoint,
 * except that both parents intentionally borrow the same file/common backing.
 * Additional file bytes known through the parents must also be disjoint even
 * when mapped_size is shorter. Arbitrary forged handles are invalid.
 *
 * Rerun the genuine schema's ordinary context query internally. Require an
 * absent registry, a four-byte-aligned object start and a complete payload no
 * larger than INT32_MAX. This conservative size domain keeps cumulative
 * object-relative cursors within the addressed converter's signed32 range;
 * no larger-object behavior is inferred. Read each selected u32 length,
 * require its high bit clear, retain exactly that many raw bytes and
 * then the 0..3 bytes needed for absolute four-byte alignment. No UTF-8, NUL,
 * padding-content or terminator constraint is imposed. Empty present spans
 * retain their exact boundary pointer/offset; root-only absent spans are
 * {NULL, UINT64_MAX, 0}. Lengths/bytes/padding partition each string source;
 * both string sources partition the complete object. Trailing bytes fail.
 * No array expansion, scalar/PPtr interpretation, registry/RID resolution,
 * selected reference payload or general TypeTree execution is admitted.
 *
 * Validate pointer/range disjointness and output/parent states before work.
 * Charge one admission unit; select engine, endian, original object/type,
 * tree presence, mapped identity/logical upper bound, supported object size,
 * mapped object coverage, schema lineage, then context query. An object size
 * above INT32_MAX returns UNSUPPORTED_OBJECT_SIZE with no limit, FIELD_OBJECT
 * and the original object's byte-size word offset. This precedes incomplete
 * mapping, live-schema/lineage checks and caller payload caps; no payload byte
 * is read. Earlier argument/state, work, engine/endian, row, tree-presence and
 * mapped-identity failures retain precedence.
 * Charge the nested query against remaining work and preserve its complete
 * result; a nested work failure maps to LIMIT_WORK. Qualify the type/profile
 * before payload parsing: one per node, one per name length, and one per
 * compared name byte. A schema-name failure identifies its original offset
 * word rather than mixing common-buffer coordinates with file coordinates.
 *
 * Enforce payload/value/depth caps before two payload passes. Each pass charges
 * width+1 before each of six spans (length/bytes/padding for both strings),
 * including empty spans, and one per completed logical value. Check object
 * extent, then work before reading each span. Count domain and string caps
 * follow length decoding; padding cap precedes its span. Caps count distinct
 * source bytes, not repeated pass visits. All caps include zero. The fixed
 * logical depth is1 and value count3. Fixed stack locals need no heap scratch.
 *
 * After complete preflight, charge one planning unit, report/enforce the exact
 * single retained allocation size A, then charge A before allocation and zero
 * initialization. Replay into staged storage, and charge one final publication
 * unit. Successful work is A+Q+2*(B+6+3)+3, where B is consumed object bytes
 * and Q is context work plus profile qualification (127 for this profile).
 * Failed charges consume no units. Retained accounting counts requested heap
 * payload including alignment, excluding allocator bookkeeping and fixed stack
 * locals. Cleanup is budget-free; peaks include allocations freed on failure.
 * Extent failures identify object/mapping end; field/domain/cap/work failures
 * identify the pending field, except owner/planning/publication use no source.
 */
SerializedFileValuesResult serialized_file_values_create_text_asset(
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* out_values);

/* Decode one ordinary object using only the complete TextAsset profile above
 * or the thirteen-/fifteen-node ClassID114 profiles below. The named
 * create_text_asset constructor keeps its original type admission, failure
 * ordering and Q=127; it does not admit ClassID114. Both constructors share
 * the same genuine-owner/backing, disjointness, lifetime, transactional output,
 * little-endian, four-aligned origin and INT32_MAX payload premises above.
 * Type selection happens once after the same source/context admission, with
 * one cumulative budget; no failed constructor is retried with fresh limits.
 *
 * ClassID114 requires unstripped, script-bearing original type metadata with
 * a nonnegative signed16 script index and genuine sixteen-byte script/type hash
 * sources. These hashes/index are retained identities, not resolved scripts or
 * allowlists. Require the complete original ordered hierarchy below, version1,
 * indices0..12, zero opaque tails, and no ordinary registry boundary. Every
 * listed control is exact; unlisted variants remain unsupported policies.
 *
 * node level type/field                         size type_flags meta_flags
 *   0   0   MonoBehaviour/Base                    -1      0     0x8000
 *   1   1   PPtr<GameObject>/m_GameObject          12      0     0x41
 *   2   2   int/m_FileID                           4      0     0x41
 *   3   2   SInt64/m_PathID                        8      0     0x41
 *   4   1   UInt8/m_Enabled                        1      0     0x4101
 *   5   1   PPtr<MonoScript>/m_Script              12      0     0
 *   6   2   int/m_FileID                           4      0     0x800001
 *   7   2   SInt64/m_PathID                        8      0     0x800001
 *   8   1   string/m_Name                         -1      0     0x88001
 *   9   2   Array/Array                           -1      1     0x84001
 *  10   3   int/size                              4      0     0x80001
 *  11   3   char/data                             1      0     0x80001
 *  12   1   int/<original authored field name>     4      0     0
 *
 * The last field name is arbitrary genuine schema bytes: no lexical check,
 * normalization or copy is needed. Preserve its original name descriptor.
 * All other names/types compare exact bounded bytes. No object name, PathID,
 * script index/hash, final field spelling or fixture digest is an allowlist.
 * The shape does not certify a CLR type or ScriptableObject runtime identity.
 *
 * Ten logical values retain root0, PPtr1 with integer children2/3, UInt8
 * value4, PPtr5 with integer children6/7, string8, and final integer9 (schema
 * node12). Both PPtrs are ordinary containers with packed 4+8-byte children; no
 * implicit eight-byte alignment or target resolution is performed. int/SInt64
 * are SIGNED_INTEGER; UInt8 is UNSIGNED_INTEGER. Preserve all integer bits.
 * Only UInt8's 0x4000 ordinary-exit control executes scalar align4; string's
 * 0xc000 control executes its existing align4. No padding-content rule is
 * imposed. Integer bytes plus executed padding partition each scalar source;
 * each container's children partition its complete source. Exact exhaustion is
 * required. No PPtr graph, generic arrays, registry/RID or project claim
 * follows.
 *
 * The new integer caps precede the pending scalar span's extent/work/read:
 * check per-scalar width, then cumulative integer bytes. Failure identifies
 * FIELD_INTEGER_BYTES, its original schema node and pending file offset. They
 * count the six scalar fields (29 bytes), not string length words, padding or
 * repeated passes. TextAsset uses zero integer bytes and passes zero caps.
 * Existing payload/value/depth/string/padding/storage/work caps retain
 * real-zero behavior; logical count/depth is10/2 for this profile. Padding caps
 * precede padding span admission just as in the TextAsset profile.
 *
 * Qualification Q=227: context15, thirteen node checks, 26 name admissions and
 * 173 fixed-name byte comparisons. The arbitrary final field costs one of
 * those name admissions and no byte inspection. Each payload pass admits six
 * scalar spans, one scalar-padding span and three string spans (K=10), then
 * charges one per completed logical value (V=10). Complete PPtr containers
 * charge after their children; the root charges after exact exhaustion.
 * Successful work is A+Q+2*(B+K+V)+3 with the same
 * admission/planning/publication units as TextAsset. A is the one exact
 * allocation for the view plus3,10 or12 extended records; no heap scratch or
 * general recursive walker is used.
 *
 * The second ClassID114 profile retains exactly the same nodes0..11 and their
 * controls, with the root's complete subtree ending at15 instead of13. Its
 * final authored scalar is replaced by this complete three-node PPtr tail:
 *
 * node level type/field                         size type_flags meta_flags
 *  12   1   PPtr<$TextAsset>/<authored name>       12      0     0
 *  13   2   int/m_FileID                           4      0     0x800001
 *  14   2   SInt64/m_PathID                        8      0     0x800001
 *
 * The dollar is part of the exact observed type spelling; PPtr<TextAsset>
 * without it remains unsupported. Require the same version1, original node
 * indices, zero tails, type metadata and absent registry premises. Node12's
 * field name is arbitrary genuine bytes under the same no-scan policy as the
 * thirteen-node authored field. Profile selection uses the complete node
 * count once, then checks every control and fixed name with the same budget;
 * failed qualification never falls back to another profile.
 *
 * Twelve logical values retain the same values0..8, root subtree end12, and
 * final container9 (schema12) with signed integer children10/11 (schema13/14).
 * Its packed4+8-byte pair uses the same ordinary PPtr reader and performs no
 * extra alignment. Seven scalar fields total37 integer bytes; value count12,
 * maximum depth2 and per-scalar maximum8 retain all existing real-zero caps.
 * No meaning is assigned to null/local/external pairs here. The copied fields
 * do not resolve a target, bind a managed type, or recover original identity.
 *
 * This profile has Q=273: context17, fifteen node checks, thirty name
 * admissions and211 fixed-name byte comparisons. Each pass admits seven scalar
 * spans, one scalar-padding span and three string spans (K=11), and completes
 * twelve values (V=12). Success is A+273+2*(B+23)+3. A grows by exactly two
 * value records relative to the thirteen-node profile; the shared ownership,
 * source admission, transaction and failure-order contracts stay unchanged.
 */
SerializedFileValuesResult serialized_file_values_create_ordinary(
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* out_values);

const SerializedFileValuesView* serialized_file_values_view(const SerializedFileValues* values);
const SerializedFileValue* serialized_file_values_value(
    const SerializedFileValues* values, size_t value_ordinal);

#ifdef __cplusplus
}
#endif

#endif
