// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_UTF8_H
#define COMMON_UTF8_H

#include <stddef.h>
#include <stdint.h>

typedef enum CommonUtf8Status
{
    COMMON_UTF8_OK = 0,
    COMMON_UTF8_INVALID_ARGUMENT,
    COMMON_UTF8_INVALID_ENCODING
} CommonUtf8Status;

typedef struct CommonUtf8Result
{
    CommonUtf8Status status;
    /* First byte of the first ill-formed sequence, SIZE_MAX on success. */
    size_t offset;
} CommonUtf8Result;

/*
 * Validates exactly byte_count bytes as strict Unicode-scalar UTF-8.
 * Embedded NUL and other control scalar values are valid UTF-8. No locale,
 * normalization, or NUL-termination rule is involved. A NULL byte pointer is
 * invalid even when byte_count is zero.
 */
CommonUtf8Result common_utf8_validate(
    const uint8_t *bytes,
    size_t byte_count);

/*
 * Decodes the first scalar from a nonempty counted span. On success,
 * *scalar and *sequence_size are set and trailing bytes are not inspected.
 * On every failure both output values remain unchanged.
 */
CommonUtf8Result common_utf8_decode_one(
    const uint8_t *bytes,
    size_t byte_count,
    uint32_t *scalar,
    size_t *sequence_size);

const char *common_utf8_status_name(CommonUtf8Status status);

#endif /* COMMON_UTF8_H */
