// SPDX-License-Identifier: GPL-3.0-only

#include "common/utf8.h"

#include <stdbool.h>

static CommonUtf8Result make_result(CommonUtf8Status status, size_t offset)
{
    CommonUtf8Result result;

    result.status = status;
    result.offset = offset;
    return result;
}

static bool is_continuation(uint8_t value)
{
    return value >= UINT8_C(0x80) && value <= UINT8_C(0xbf);
}

CommonUtf8Result common_utf8_decode_one(
    const uint8_t *bytes,
    size_t byte_count,
    uint32_t *scalar,
    size_t *sequence_size)
{
    uint32_t decoded;
    size_t decoded_size;
    uint8_t first;

    if (bytes == NULL || scalar == NULL || sequence_size == NULL)
        return make_result(COMMON_UTF8_INVALID_ARGUMENT, SIZE_MAX);
    if (byte_count == 0U)
        return make_result(COMMON_UTF8_INVALID_ENCODING, 0U);

    first = bytes[0];
    if (first <= UINT8_C(0x7f))
    {
        decoded = first;
        decoded_size = 1U;
    }
    else if (first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf))
    {
        if (byte_count < 2U || !is_continuation(bytes[1]))
            return make_result(COMMON_UTF8_INVALID_ENCODING, 0U);
        decoded = ((uint32_t)(first & UINT8_C(0x1f)) << 6U) |
            (uint32_t)(bytes[1] & UINT8_C(0x3f));
        decoded_size = 2U;
    }
    else if (first >= UINT8_C(0xe0) && first <= UINT8_C(0xef))
    {
        if (byte_count < 3U || !is_continuation(bytes[1]) ||
            !is_continuation(bytes[2]) ||
            (first == UINT8_C(0xe0) && bytes[1] < UINT8_C(0xa0)) ||
            (first == UINT8_C(0xed) && bytes[1] > UINT8_C(0x9f)))
        {
            return make_result(COMMON_UTF8_INVALID_ENCODING, 0U);
        }
        decoded = ((uint32_t)(first & UINT8_C(0x0f)) << 12U) |
            ((uint32_t)(bytes[1] & UINT8_C(0x3f)) << 6U) |
            (uint32_t)(bytes[2] & UINT8_C(0x3f));
        decoded_size = 3U;
    }
    else if (first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4))
    {
        if (byte_count < 4U || !is_continuation(bytes[1]) ||
            !is_continuation(bytes[2]) || !is_continuation(bytes[3]) ||
            (first == UINT8_C(0xf0) && bytes[1] < UINT8_C(0x90)) ||
            (first == UINT8_C(0xf4) && bytes[1] > UINT8_C(0x8f)))
        {
            return make_result(COMMON_UTF8_INVALID_ENCODING, 0U);
        }
        decoded = ((uint32_t)(first & UINT8_C(0x07)) << 18U) |
            ((uint32_t)(bytes[1] & UINT8_C(0x3f)) << 12U) |
            ((uint32_t)(bytes[2] & UINT8_C(0x3f)) << 6U) |
            (uint32_t)(bytes[3] & UINT8_C(0x3f));
        decoded_size = 4U;
    }
    else
    {
        return make_result(COMMON_UTF8_INVALID_ENCODING, 0U);
    }

    *scalar = decoded;
    *sequence_size = decoded_size;
    return make_result(COMMON_UTF8_OK, SIZE_MAX);
}

CommonUtf8Result common_utf8_validate(
    const uint8_t *bytes,
    size_t byte_count)
{
    size_t position = 0U;

    if (bytes == NULL)
        return make_result(COMMON_UTF8_INVALID_ARGUMENT, SIZE_MAX);
    while (position < byte_count)
    {
        uint32_t scalar;
        size_t sequence_size;
        CommonUtf8Result result = common_utf8_decode_one(
            bytes + position,
            byte_count - position,
            &scalar,
            &sequence_size);

        if (result.status != COMMON_UTF8_OK)
            return make_result(result.status, position);
        position += sequence_size;
    }
    return make_result(COMMON_UTF8_OK, SIZE_MAX);
}

const char *common_utf8_status_name(CommonUtf8Status status)
{
    switch (status)
    {
        case COMMON_UTF8_OK: return "ok";
        case COMMON_UTF8_INVALID_ARGUMENT: return "invalid-argument";
        case COMMON_UTF8_INVALID_ENCODING: return "invalid-encoding";
        default: return NULL;
    }
}
