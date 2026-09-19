// SPDX-License-Identifier: GPL-3.0-only
#include "io/typetree_value_digest.h"

#include <string.h>

#define VALUE_DIGEST_MAX_NODES 1048576U
#define VALUE_DIGEST_MAX_DEPTH 256U
#define VALUE_DIGEST_MAX_STRING 16777216U

_Static_assert(sizeof(double) == sizeof(uint64_t), "decoded float identity requires 64-bit double");

typedef struct {
    CommonSha256Context hash;
    size_t remaining_nodes;
} ValueDigest;

static void hash_word(ValueDigest *digest, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (i * 8U));
    common_sha256_update(&digest->hash, bytes, sizeof(bytes));
}

static bool hash_string(ValueDigest *digest, const char *text, size_t *length) {
    if (!text)
        return false;
    size_t size = 0;
    while (size <= VALUE_DIGEST_MAX_STRING && text[size])
        ++size;
    if (size > VALUE_DIGEST_MAX_STRING)
        return false;
    hash_word(digest, size);
    common_sha256_update(&digest->hash, text, size);
    if (length)
        *length = size;
    return true;
}

static bool expanded_bytes(const TypeTreeValue *array) {
    for (int i = 0; i < array->array_val.count; ++i) {
        const TypeTreeValue *byte = &array->array_val.elements[i];
        if (byte->type != VAL_TYPE_INT || !byte->integer_is_unsigned || byte->uint_val > 255U ||
            !byte->name || !byte->type_str || strcmp(byte->type_str, "UInt8") != 0)
            return false;
    }
    return true;
}

static bool hash_value(ValueDigest *digest, const TypeTreeValue *value, size_t depth) {
    if (!value || depth >= VALUE_DIGEST_MAX_DEPTH || !digest->remaining_nodes ||
        value->type < VAL_TYPE_INT || value->type > VAL_TYPE_NONE)
        return false;
    --digest->remaining_nodes;
    hash_word(digest, (uint32_t)value->type);
    if (!hash_string(digest, value->name, NULL) || !hash_string(digest, value->type_str, NULL))
        return false;
    switch (value->type) {
    case VAL_TYPE_INT:
        hash_word(digest, value->integer_is_unsigned);
        hash_word(digest, value->integer_is_unsigned ? value->uint_val : (uint64_t)value->int_val);
        return true;
    case VAL_TYPE_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &value->float_val, sizeof(bits));
        hash_word(digest, bits);
        return true;
    }
    case VAL_TYPE_STRING: {
        size_t length;
        return hash_string(digest, value->string_val, &length) && length == value->string_length;
    }
    case VAL_TYPE_STRUCT:
        if (value->struct_val.count < 0 ||
            (size_t)value->struct_val.count > digest->remaining_nodes ||
            (value->struct_val.count && !value->struct_val.members))
            return false;
        hash_word(digest, (uint32_t)value->struct_val.count);
        for (int i = 0; i < value->struct_val.count; ++i)
            if (!hash_value(digest, &value->struct_val.members[i], depth + 1))
                return false;
        return true;
    case VAL_TYPE_ARRAY: {
        const int count = value->array_val.count;
        if (count < 0 || (size_t)count > digest->remaining_nodes)
            return false;
        const bool packed = value->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES;
        if (packed) {
            if (count && !value->array_val.packed_bytes)
                return false;
        } else if (value->array_val.storage != TYPETREE_ARRAY_VALUES ||
                   (count && !value->array_val.elements))
            return false;
        const bool bytes = packed || expanded_bytes(value);
        hash_word(digest, (uint32_t)count);
        hash_word(digest, bytes);
        if (bytes) {
            digest->remaining_nodes -= (size_t)count;
            if (packed && count)
                common_sha256_update(&digest->hash, value->array_val.packed_bytes, (size_t)count);
            else
                for (int i = 0; i < count; ++i) {
                    const uint8_t byte = (uint8_t)value->array_val.elements[i].uint_val;
                    common_sha256_update(&digest->hash, &byte, 1);
                }
        } else
            for (int i = 0; i < count; ++i)
                if (!hash_value(digest, &value->array_val.elements[i], depth + 1))
                    return false;
        return true;
    }
    case VAL_TYPE_NONE:
        return true;
    }
    return false;
}

bool typetree_value_digest(const TypeTreeValue *value, uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!digest)
        return false;
    ValueDigest context = {.remaining_nodes = VALUE_DIGEST_MAX_NODES};
    common_sha256_init(&context.hash);
    static const char domain[] = "UnityCommon.OrderedTypeTreeValue.v1";
    common_sha256_update(&context.hash, domain, sizeof(domain));
    if (!hash_value(&context, value, 0))
        return false;
    common_sha256_final(&context.hash, digest);
    return true;
}
