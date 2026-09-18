// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_METADATA_MATERIALIZATION_INTERNAL_H
#define SERIALIZED_METADATA_MATERIALIZATION_INTERNAL_H

#include "io/serialized_file_prefix.h"

#include "common/common.h"

/* Preserve two's-complement wire values without converting an out-of-range
 * unsigned value directly to a signed C integer. */
static inline int32_t serialized_metadata_signed32(uint32_t bits) {
    return bits <= INT32_MAX ? (int32_t)bits : -1 - (int32_t)(UINT32_MAX - bits);
}

static inline int64_t serialized_metadata_signed64(uint64_t bits) {
    return bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
}

/* The live immutable owner has established that this complete nonempty span
 * ends at its first NUL. Copy that exact extent without rescanning or choosing
 * an encoding. The caller owns the returned mem_alloc allocation, or NULL on
 * failure; its enclosing semantic owner supplies cleanup and byte accounting. */
static inline char* serialized_metadata_copy_terminated_string(SerializedFilePrefixSpan source) {
    if (!source.data || !source.size || source.data[source.size - 1U] != 0U) {
        return NULL;
    }
    char* copy = mem_alloc(source.size);
    if (copy) {
        memcpy(copy, source.data, source.size);
    }
    return copy;
}

#endif
