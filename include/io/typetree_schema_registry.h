// SPDX-License-Identifier: GPL-3.0-only

#ifndef TYPETREE_SCHEMA_REGISTRY_H
#define TYPETREE_SCHEMA_REGISTRY_H

#include "common/common.h"
#include "io/typetree.h"

/*
 * Portable, fail-closed cache of TypeTree schemas observed in authoritative
 * SerializedFiles.  A schema is never synthesized from an object payload.
 * Every lookup component is compared byte-for-byte (or by exact integer
 * value), including the Unity version and both Unity hashes.
 */

#define TYPETREE_SCHEMA_REGISTRY_FORMAT_VERSION 1U
#define TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE 1024U

typedef enum {
    TYPETREE_SCHEMA_OK = 0,
    TYPETREE_SCHEMA_NOT_FOUND,
    TYPETREE_SCHEMA_INVALID_ARGUMENT,
    TYPETREE_SCHEMA_INVALID_SCHEMA,
    TYPETREE_SCHEMA_KEY_CONFLICT,
    TYPETREE_SCHEMA_ALLOCATION_FAILED,
    TYPETREE_SCHEMA_SIZE_OVERFLOW,
    TYPETREE_SCHEMA_IO_ERROR,
    TYPETREE_SCHEMA_INVALID_FORMAT,
    TYPETREE_SCHEMA_DIGEST_MISMATCH,
    TYPETREE_SCHEMA_LIMIT_EXCEEDED
} TypeTreeSchemaStatus;

/*
 * Provenance is an explicit trust boundary, not a property inferred from the
 * presence of an embedded TypeTree. PINNED_INPUT means the caller validated
 * the complete source against an external identity (for example a corpus
 * SHA-256 pin). IMPORTED_REGISTRY is reserved for the canonical registry
 * decoder after its format and digest have validated.
 */
typedef enum {
    TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT = 0,
    TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT,
    TYPETREE_SCHEMA_PROVENANCE_IMPORTED_REGISTRY
} TypeTreeSchemaProvenance;

/*
 * unity_version is a borrowed byte string and need not be NUL terminated.
 * Embedded NUL bytes are invalid.  serialized_type_id identifies the
 * TypeTreeType record; class_id is the resolved class ID used by the object.
 * They are deliberately separate because newer SerializedFiles can refer to
 * the type table by index.  The stripped/ref flags are also key material;
 * neither state is silently substituted during lookup.
 */
typedef struct {
    uint32_t serialized_file_version;
    const char* unity_version;
    size_t unity_version_size;
    int32_t class_id;
    int32_t serialized_type_id;
    uint16_t script_type_index;
    bool has_script_id_hash;
    bool is_stripped;
    bool is_ref_type;
    uint8_t script_id_hash[16];
    uint8_t type_hash[16];
} TypeTreeSchemaKey;

typedef struct TypeTreeSchemaRegistryEntry TypeTreeSchemaRegistryEntry;

typedef struct {
    TypeTreeSchemaRegistryEntry* entries;
    size_t count;
    size_t capacity;
} TypeTreeSchemaRegistry;

void typetree_schema_registry_init(TypeTreeSchemaRegistry* registry);
void typetree_schema_registry_dispose(TypeTreeSchemaRegistry* registry);
size_t typetree_schema_registry_count(const TypeTreeSchemaRegistry* registry);

/*
 * Returns a borrowed exact key and schema in the same canonical key order as
 * registry serialization. The views remain valid only while registry is
 * unchanged and alive. This is an inspection boundary; it performs no
 * fallback lookup and never exposes the private entry representation.
 */
TypeTreeSchemaStatus typetree_schema_registry_entry_view(
    const TypeTreeSchemaRegistry* registry, size_t canonical_index,
    TypeTreeSchemaKey* out_key, const TypeTreeType** out_schema);

/*
 * Constructs a key using the same script-hash presence rule as Unity's
 * SerializedFile type record.  unity_version must be NUL terminated within
 * TYPETREE_SCHEMA_REGISTRY_MAX_UNITY_VERSION_SIZE bytes.
 */
TypeTreeSchemaStatus typetree_schema_key_from_type(
    TypeTreeSchemaKey* out_key,
    uint32_t serialized_file_version,
    const char* unity_version,
    int32_t class_id,
    const TypeTreeType* type);

/*
 * Learns a deep copy of schema only across an explicit trusted provenance
 * boundary. Merely finding an embedded TypeTree is not authority. Re-learning
 * an identical key/schema pair is idempotent; the same key with different
 * schema bytes is a hard conflict.
 */
TypeTreeSchemaStatus typetree_schema_registry_learn(
    TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* key,
    const TypeTreeType* schema,
    TypeTreeSchemaProvenance provenance);

/*
 * Returns a newly allocated deep copy.  The caller owns it and must call
 * typetree_free_type().  A miss never returns a partial schema.
 */
TypeTreeSchemaStatus typetree_schema_registry_lookup(
    const TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* key,
    TypeTreeType* out_schema);

/*
 * Creates an explicit exact-version binding from an already-authoritative
 * schema whose complete key is identical except for unity_version.  This is
 * permitted only for a destination version/class/hash combination with a
 * code-reviewed semantic-shape profile, and only across PINNED_INPUT
 * provenance.  It is never an automatic fallback during lookup.
 */
TypeTreeSchemaStatus typetree_schema_registry_bind_exact_version(
    TypeTreeSchemaRegistry* registry,
    const TypeTreeSchemaKey* destination_key,
    TypeTreeSchemaProvenance evidence_provenance);

/*
 * Canonical little-endian encoding.  Entries are sorted by their complete
 * keys, so equal registries serialize identically regardless of insertion
 * order.  The returned buffer uses mem_alloc() and must be released with
 * mem_free(buffer, size).
 */
TypeTreeSchemaStatus typetree_schema_registry_serialize(
    const TypeTreeSchemaRegistry* registry,
    uint8_t** out_data,
    size_t* out_size);

/*
 * Strict, transactional replacement: the input digest, all reserved fields,
 * every record bound, canonical key ordering, and full input consumption are
 * checked before registry is changed.
 */
TypeTreeSchemaStatus typetree_schema_registry_deserialize_replace(
    TypeTreeSchemaRegistry* registry,
    const uint8_t* data,
    size_t size);

TypeTreeSchemaStatus typetree_schema_registry_export_file(
    const TypeTreeSchemaRegistry* registry,
    const char* path);

TypeTreeSchemaStatus typetree_schema_registry_import_file_replace(
    TypeTreeSchemaRegistry* registry,
    const char* path);

const char* typetree_schema_status_name(TypeTreeSchemaStatus status);

#endif /* TYPETREE_SCHEMA_REGISTRY_H */
