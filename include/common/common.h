// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Locale-independent ASCII comparison for protocol tokens, semantics, and
 * file suffixes.  Unlike strcasecmp this is available under strict MSVC C. */
static inline int dxbc_ascii_strcasecmp(const char* left, const char* right) {
    if (!left || !right) return left ? 1 : (right ? -1 : 0);
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return a < b ? -1 : 1;
    }
    return *left ? 1 : (*right ? -1 : 0);
}

// Logging macros
#define LOG_INFO(...)  do { printf("[INFO] " __VA_ARGS__); printf("\n"); } while(0)
#define LOG_WARN(...)  do { printf("[WARN] " __VA_ARGS__); printf("\n"); } while(0)
#define LOG_ERROR(...) do { fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

// Memory tracking (for simple leak checking)
extern atomic_size_t g_allocated_bytes;
extern atomic_size_t g_allocations_count;

static inline void* mem_alloc(size_t size) {
    if (size == 0) return NULL;
    void* ptr = malloc(size);
    if (ptr) {
        atomic_fetch_add_explicit(&g_allocated_bytes, size,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&g_allocations_count, 1,
                                  memory_order_relaxed);
    }
    return ptr;
}

static inline void* mem_realloc(void* ptr, size_t old_size, size_t new_size) {
    /* Keep realloc(ptr, 0) out of the implementation-defined ownership
     * corner: callers retain ptr when this helper returns NULL. */
    if (new_size == 0) return NULL;
    void* new_ptr = realloc(ptr, new_size);
    if (new_ptr) {
        if (new_size >= old_size) {
            atomic_fetch_add_explicit(&g_allocated_bytes,
                                      new_size - old_size,
                                      memory_order_relaxed);
        } else {
            atomic_fetch_sub_explicit(&g_allocated_bytes,
                                      old_size - new_size,
                                      memory_order_relaxed);
        }
        if (!ptr) {
            atomic_fetch_add_explicit(&g_allocations_count, 1,
                                      memory_order_relaxed);
        }
    }
    return new_ptr;
}

static inline void mem_free(void* ptr, size_t size) {
    if (ptr) {
        free(ptr);
        atomic_fetch_sub_explicit(&g_allocated_bytes, size,
                                  memory_order_relaxed);
        atomic_fetch_sub_explicit(&g_allocations_count, 1,
                                  memory_order_relaxed);
    }
}

/* Release one allocation from leak tracking without freeing it. This is used
 * only when ownership crosses to an API whose returned buffer is freed with
 * the C allocator directly (for example StringBuilder detach). */
static inline void mem_untrack_allocation(size_t size) {
    atomic_fetch_sub_explicit(&g_allocated_bytes, size,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_allocations_count, 1,
                              memory_order_relaxed);
}

/* Checked size arithmetic shared by binary parsers.  Keeping the operands as
 * size_t avoids compiler-specific "always false" diagnostics when a serialized
 * 32-bit count is checked on a 64-bit host, while retaining the check on
 * 32-bit targets. */
static inline bool dxbc_size_add_overflows(size_t left, size_t right) {
    return left > SIZE_MAX - right;
}

static inline bool dxbc_size_multiply_overflows(size_t left, size_t right) {
    return left != 0U && right > SIZE_MAX / left;
}

// Endianness Helpers
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_IS_BIG_ENDIAN 1
#else
#define HOST_IS_BIG_ENDIAN 0
#endif

static inline uint16_t swap_uint16(uint16_t val) {
    return (uint16_t)((val << 8) | (val >> 8));
}

static inline uint32_t swap_uint32(uint32_t val) {
    return ((val & 0x000000FFu) << 24) |
           ((val & 0x0000FF00u) << 8)  |
           ((val & 0x00FF0000u) >> 8)  |
           ((val & 0xFF000000u) >> 24);
}

static inline uint64_t swap_uint64(uint64_t val) {
    return ((val & 0x00000000000000FFULL) << 56) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0xFF00000000000000ULL) >> 56);
}

static inline uint16_t read_be16(uint16_t val) {
    return HOST_IS_BIG_ENDIAN ? val : swap_uint16(val);
}

static inline uint32_t read_be32(uint32_t val) {
    return HOST_IS_BIG_ENDIAN ? val : swap_uint32(val);
}

static inline uint64_t read_be64(uint64_t val) {
    return HOST_IS_BIG_ENDIAN ? val : swap_uint64(val);
}

static inline uint16_t read_le16(uint16_t val) {
    return HOST_IS_BIG_ENDIAN ? swap_uint16(val) : val;
}

static inline uint32_t read_le32(uint32_t val) {
    return HOST_IS_BIG_ENDIAN ? swap_uint32(val) : val;
}

static inline uint64_t read_le64(uint64_t val) {
    return HOST_IS_BIG_ENDIAN ? swap_uint64(val) : val;
}

#endif // COMMON_H
