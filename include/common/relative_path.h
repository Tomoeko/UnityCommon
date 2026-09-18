// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_RELATIVE_PATH_H
#define COMMON_RELATIVE_PATH_H

#include <stdbool.h>
#include <stddef.h>

/*
 * This is a portable identity grammar, not host-filesystem canonicalization.
 * It preserves Unicode-scalar UTF-8 bytes exactly, uses '/' separators, and
 * rejects empty, '.' and '..' components, backslashes, and C0 controls.
 * Target-specific case equivalence, reserved names, normalization, links,
 * reparse points, and file identity remain obligations of an anchored resolver.
 */
typedef enum CommonRelativePathStatus
{
    COMMON_RELATIVE_PATH_OK = 0,
    COMMON_RELATIVE_PATH_INVALID_ARGUMENT,
    COMMON_RELATIVE_PATH_EMPTY,
    COMMON_RELATIVE_PATH_EMPTY_COMPONENT,
    COMMON_RELATIVE_PATH_DOT_COMPONENT,
    COMMON_RELATIVE_PATH_BACKSLASH,
    COMMON_RELATIVE_PATH_CONTROL_CHARACTER,
    COMMON_RELATIVE_PATH_INVALID_UTF8
} CommonRelativePathStatus;

typedef struct CommonRelativePathResult
{
    CommonRelativePathStatus status;
    /* First invalid byte/component offset, SIZE_MAX on success. */
    size_t offset;
} CommonRelativePathResult;

CommonRelativePathResult common_relative_path_validate_utf8(
    const char *path,
    size_t path_size);

/*
 * Deterministic unsigned-byte ordering for already validated path identities.
 * A NULL pointer is permitted only when its corresponding size is zero.
 */
int common_relative_path_compare_bytes(
    const char *left,
    size_t left_size,
    const char *right,
    size_t right_size);

/* Exact component-boundary relation over path identity bytes. */
bool common_relative_path_is_equal_or_descendant(
    const char *candidate,
    size_t candidate_size,
    const char *root,
    size_t root_size);

const char *common_relative_path_status_name(CommonRelativePathStatus status);

#endif /* COMMON_RELATIVE_PATH_H */
