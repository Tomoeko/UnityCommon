// SPDX-License-Identifier: GPL-3.0-only

#ifndef TYPETREE_H
#define TYPETREE_H

#include "common/common.h"
#include "common/stream.h"

typedef struct {
    uint16_t version;
    uint8_t level;
    uint8_t type_flags;
    uint32_t type_str_offset;
    uint32_t name_str_offset;
    int32_t byte_size;
    uint32_t index;
    uint32_t meta_flags;
    /* Opaque final eight wire bytes in little-endian bit order, independent
     * of metadata byte order. Equality/registry storage is not a numeric hash
     * interpretation of this unresolved field. */
    uint64_t ref_type_hash;
    
    /*
     * Borrowed resolved names.  For file/registry-backed schemas these point
     * into TypeTreeType.string_buffer or Unity's immutable common table and
     * remain valid until typetree_free_type().  Synthetic schemas may use
     * string literals with the same lifetime requirement.
     */
    const char* type_str;
    const char* name_str;
} TypeTreeNode;

typedef struct {
    int32_t type_id;
    bool is_stripped;
    uint16_t script_type_index;
    uint8_t script_id_hash[16];
    uint8_t type_hash[16];
    
    int node_count;
    TypeTreeNode* nodes;
    
    uint8_t* string_buffer;
    uint32_t string_buffer_size;
    
    int dependency_count;
    int32_t* dependencies;
    
    bool is_ref_type;
    
    char* ref_class_name;
    char* ref_namespace;
    char* ref_asm_name;
} TypeTreeType;

// Reads a single TypeTreeType structure from a serialized file stream.
bool typetree_read_type(TypeTreeType* type, ByteStream* stream, uint32_t version, bool has_type_tree, bool is_ref_type);

// Releases all allocated resources in the TypeTreeType structure.
void typetree_free_type(TypeTreeType* type);

// Resolves an exact NUL-terminated string start from the local/common table.
// Returns NULL for invalid offsets, mid-string offsets, or unterminated data.
// The high-bit common selector uses the exact 2021.3.35f1 buffer, including its
// final empty string. Local names are independent of that fixed common table.
const char* typetree_resolve_string(const uint8_t* local_table, uint32_t local_size, uint32_t offset);

/*
 * Resolves every node string from its stored offset.  The operation is
 * transactional: no node pointer is changed unless all offsets are valid.
 */
bool typetree_bind_node_strings(TypeTreeType* type);

/* Validates the preorder grammar before object bytes are interpreted. */
bool typetree_validate_schema(const TypeTreeType* type);

typedef enum {
    VAL_TYPE_INT,
    VAL_TYPE_FLOAT,
    VAL_TYPE_STRING,
    VAL_TYPE_ARRAY,
    VAL_TYPE_STRUCT,
    VAL_TYPE_NONE
} ValueType;

typedef enum {
    /* array_val.elements owns count TypeTreeValue objects. */
    TYPETREE_ARRAY_VALUES = 0,
    /* packed_bytes stores count contiguous byte-exact uint8_t values. */
    TYPETREE_ARRAY_PACKED_BYTES = 1
} TypeTreeArrayStorage;

typedef enum {
    TYPETREE_PARSE_DEFAULT = 0,
    /*
     * Store every scalar UInt8 array contiguously instead of creating one
     * TypeTreeValue per byte. Access through typetree_array_get_int() or the
     * byte-span/copy APIs so representation never depends on a field name.
     */
    TYPETREE_PARSE_PACK_BYTE_ARRAYS = 1u << 0,
    TYPETREE_PARSE_PACK_COMPRESSED_BLOB = TYPETREE_PARSE_PACK_BYTE_ARRAYS,
    /*
     * Borrow packed byte arrays directly from ByteStream instead of copying.
     * The stream backing bytes must then outlive the parsed TypeTreeValue.
     * This flag is mutually exclusive with PACK_BYTE_ARRAYS.
     */
    TYPETREE_PARSE_BORROW_BYTE_ARRAYS = 1u << 1
} TypeTreeParseFlags;

struct TypeTreeValue;
typedef struct TypeTreeValue TypeTreeValue;

struct TypeTreeValue {
    /* Borrowed from the schema; the TypeTreeType must outlive this value. */
    const char* name;
    const char* type_str;
    ValueType type;
    bool integer_is_unsigned;
    size_t string_length;
    union {
        int64_t int_val;
        uint64_t uint_val;
        double float_val;
        char* string_val;
        struct {
            TypeTreeValue* elements;
            int count;
            TypeTreeArrayStorage storage;
            const uint8_t* packed_bytes;
            bool packed_bytes_owned;
        } array_val;
        struct {
            TypeTreeValue* members;
            int count;
        } struct_val;
    };
};

bool typetree_parse_value(const TypeTreeType* type, int* node_idx, ByteStream* stream, TypeTreeValue* out_val);
bool typetree_parse_value_ex(const TypeTreeType* type, int* node_idx,
                             ByteStream* stream, TypeTreeValue* out_val,
                             uint32_t parse_flags);
void typetree_free_value(TypeTreeValue* val);
const TypeTreeValue* typetree_find_child(const TypeTreeValue* val, const char* name);
const TypeTreeValue* typetree_find_path(const TypeTreeValue* val, const char* path);
const TypeTreeValue* typetree_get_array(const TypeTreeValue* val);

/* Reads either legacy TypeTreeValue elements or packed byte elements. */
bool typetree_array_get_int(const TypeTreeValue* val, int index,
                            int64_t* out_value);
bool typetree_array_get_uint(const TypeTreeValue* val, int index,
                             uint64_t* out_value);
bool typetree_value_get_int(const TypeTreeValue* val, int64_t* out_value);
bool typetree_value_get_uint(const TypeTreeValue* val, uint64_t* out_value);

/*
 * Returns a borrowed, contiguous view only when the array uses packed byte
 * storage.  The span remains valid until typetree_free_value() is called.
 */
bool typetree_get_byte_span(const TypeTreeValue* val, const uint8_t** out_data,
                            size_t* out_size);

/* Copies a byte range from either packed or legacy array storage. */
bool typetree_array_copy_bytes(const TypeTreeValue* val, size_t offset,
                               uint8_t* destination, size_t count);

#endif // TYPETREE_H
