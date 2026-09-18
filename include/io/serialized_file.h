// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_H
#define SERIALIZED_FILE_H

#include "common/common.h"
#include "common/stream.h"
#include "io/typetree.h"
#include "io/typetree_schema_registry.h"

typedef struct {
    int32_t file_id;
    int64_t path_id;
} AssetPPtr;

typedef struct {
    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;
    int32_t type_id_or_index;
    
    // Resolved attributes
    int32_t type_id;
    uint16_t script_type_index;
    
    // Legacy fields
    uint16_t old_type_id;
    uint8_t stripped;
} AssetObjectInfo;

typedef struct {
    char* virtual_path;
    uint8_t guid[16];
    int32_t type;
    char* path_name;
} AssetFileExternal;

typedef struct {
    uint64_t metadata_size;
    uint64_t file_size;
    uint32_t version;
    uint64_t data_offset;
    bool big_endian;
    
    char* unity_version;
    uint32_t target_platform;
    bool type_tree_enabled;
    
    int type_count;
    TypeTreeType* types;
    
    int object_count;
    AssetObjectInfo* objects;
    
    int script_count;
    AssetPPtr* scripts;
    
    int external_count;
    AssetFileExternal* externals;
    
    int ref_type_count;
    TypeTreeType* ref_types;
    
    char* user_information;
    
    const uint8_t* raw_data;
    size_t raw_size;
} SerializedFile;

/* Parses a complete mapped v22 file with exact 2021.3.35f1 or 2021.3.29f1
 * bytes. Both use the shared physical directory; exact29 is separate Common
 * compatibility policy, not a complete-reader certificate from the
 * 2021.3.35f1 Editor.
 * Convenience opens cap the ordinary directory at 65,536 types, 1,048,576
 * objects/nodes/dependency words, 64 MiB string bytes, 256 MiB consumed metadata
 * and 128 MiB retained index. Each of at most two explicit version attempts
 * has 1 Gi work and zero heap scratch. These are adapter policy, not Unity limits.
 * Semantic rows temporarily coexist with physical Directory/MetadataTail
 * storage. Both raw owners are disposed before duplicate/overlap validation
 * and schema-registry transactions. Physical tail admission and semantic
 * tail materialization precede those checks.
 * Returns true only after the remaining semantic/schema policies pass. */
bool serialized_file_open(SerializedFile* file, const uint8_t* file_data, size_t file_size);

/*
 * Parses and validates the complete SerializedFile header, metadata, type
 * records, object table, external references, and declared byte ranges, but
 * does not require schemas for a TypeTree-disabled player file.  Such a file
 * is suitable for deterministic inventory only: an object's payload must not
 * be interpreted until its class schema has been resolved explicitly with
 * serialized_file_resolve_class_schema().
 */
bool serialized_file_open_metadata(SerializedFile* file,
                                   const uint8_t* file_data,
                                   size_t file_size);

/*
 * Transactionally resolves every unresolved type/ref-type record for one
 * exact class ID from an imported registry.  A miss leaves the SerializedFile
 * unchanged.  Embedded TypeTrees and already-resolved records are preserved.
 */
TypeTreeSchemaStatus serialized_file_resolve_class_schema(
    SerializedFile* file, int32_t class_id,
    const TypeTreeSchemaRegistry* schema_registry);

/*
 * Registry-aware exact open. Embedded schemas from TypeTree-enabled files are
 * usable for that file but are untrusted and never mutate the registry.
 * TypeTree-disabled files must resolve every serialized type through the
 * exact imported/pinned registry key. Passing NULL retains the ordinary
 * TypeTree-enabled path but rejects TypeTree-disabled files.
 */
bool serialized_file_open_with_schema_registry(
    SerializedFile* file, const uint8_t* file_data, size_t file_size,
    TypeTreeSchemaRegistry* schema_registry);

/*
 * Explicit provenance variant. PINNED_INPUT transactionally learns embedded
 * schemas after complete metadata validation. UNTRUSTED_INPUT is identical to
 * serialized_file_open_with_schema_registry(). IMPORTED_REGISTRY is not a
 * valid input-file provenance.
 */
bool serialized_file_open_with_schema_registry_ex(
    SerializedFile* file, const uint8_t* file_data, size_t file_size,
    TypeTreeSchemaRegistry* schema_registry,
    TypeTreeSchemaProvenance input_provenance);

// Closes and releases all allocated memory within the SerializedFile.
void serialized_file_close(SerializedFile* file);

// Retrieves the object info by PathID. Returns NULL if not found.
const AssetObjectInfo* serialized_file_get_object(SerializedFile* file, int64_t path_id);

// Retrieves the raw byte payload of an object.
// Returns a pointer to the start of the object data in the file, and sets out_size.
// Returns NULL on error or if not found.
const uint8_t* serialized_file_get_object_data(SerializedFile* file, const AssetObjectInfo* obj, size_t* out_size);

#endif // SERIALIZED_FILE_H
