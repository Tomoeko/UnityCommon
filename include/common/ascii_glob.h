// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_ASCII_GLOB_H
#define COMMON_ASCII_GLOB_H

#include <stdbool.h>

/*
 * Locale-independent glob matching over NUL-terminated byte strings.
 * Matching is case-sensitive and operates on bytes, not Unicode code points.
 *
 * Syntax:
 *   *          zero or more bytes
 *   ?          exactly one byte
 *   [abc]      one listed byte
 *   [a-z]      one byte in an inclusive ascending range
 *   [!abc]     one byte not listed (`^` is an equivalent negation marker)
 *   \x         literal x, both inside and outside a bracket class
 *
 * An unescaped '-' is literal only as the first or last class item. Reverse
 * ranges and ambiguous chained ranges are malformed. A literal ']' within a
 * class must be escaped. Malformed patterns are never interpreted literally.
 *
 * Validation is O(P). Matching is O(P*T) worst-case time and O(T) auxiliary
 * memory for pattern length P and text length T. It is iterative and does not
 * use recursive or exponential backtracking.
 */

typedef enum {
    ASCII_GLOB_OK = 0,
    ASCII_GLOB_INVALID_ARGUMENT,
    ASCII_GLOB_MALFORMED_PATTERN,
    ASCII_GLOB_ALLOCATION_FAILED,
} AsciiGlobStatus;

/* Validates syntax without matching or allocating. */
AsciiGlobStatus ascii_glob_validate(const char* pattern);

/* On every failure, *out_match is false. */
AsciiGlobStatus ascii_glob_match(const char* pattern, const char* text,
                                 bool* out_match);

const char* ascii_glob_status_name(AsciiGlobStatus status);

#endif /* COMMON_ASCII_GLOB_H */
