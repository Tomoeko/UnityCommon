// SPDX-License-Identifier: GPL-3.0-only

#include "common/relative_path.h"
#include "common/utf8.h"

#include <stdint.h>
#include <string.h>

static CommonRelativePathResult make_result(
    CommonRelativePathStatus status,
    size_t offset)
{
    CommonRelativePathResult result;

    result.status = status;
    result.offset = offset;
    return result;
}

static bool component_is_dot(
    const uint8_t *bytes,
    size_t begin,
    size_t end)
{
    const size_t length = end - begin;

    return length == 1U && bytes[begin] == (uint8_t)'.';
}

static bool component_is_dot_dot(
    const uint8_t *bytes,
    size_t begin,
    size_t end)
{
    const size_t length = end - begin;

    return length == 2U && bytes[begin] == (uint8_t)'.' &&
        bytes[begin + 1U] == (uint8_t)'.';
}

static CommonRelativePathResult component_result(
    const uint8_t *bytes,
    size_t begin,
    size_t end)
{
    if (begin == end)
        return make_result(COMMON_RELATIVE_PATH_EMPTY_COMPONENT, begin);
    if (component_is_dot(bytes, begin, end) ||
        component_is_dot_dot(bytes, begin, end))
    {
        return make_result(COMMON_RELATIVE_PATH_DOT_COMPONENT, begin);
    }
    return make_result(COMMON_RELATIVE_PATH_OK, SIZE_MAX);
}

CommonRelativePathResult common_relative_path_validate_utf8(
    const char *path,
    size_t path_size)
{
    const uint8_t *bytes = (const uint8_t *)path;
    size_t component_begin = 0U;
    size_t position = 0U;

    if (path == NULL)
    {
        return make_result(
            COMMON_RELATIVE_PATH_INVALID_ARGUMENT,
            SIZE_MAX);
    }
    if (path_size == 0U)
        return make_result(COMMON_RELATIVE_PATH_EMPTY, 0U);
    while (position < path_size)
    {
        const uint8_t value = bytes[position];

        if (value == (uint8_t)'/')
        {
            CommonRelativePathResult component = component_result(
                bytes, component_begin, position);

            if (component.status != COMMON_RELATIVE_PATH_OK)
                return component;
            component_begin = ++position;
            continue;
        }
        if (value == (uint8_t)'\\')
        {
            return make_result(
                COMMON_RELATIVE_PATH_BACKSLASH,
                position);
        }
        if (value < UINT8_C(0x20))
        {
            return make_result(
                COMMON_RELATIVE_PATH_CONTROL_CHARACTER,
                position);
        }
        {
            uint32_t scalar;
            size_t sequence_size;
            CommonUtf8Result utf8_result = common_utf8_decode_one(
                bytes + position,
                path_size - position,
                &scalar,
                &sequence_size);

            if (utf8_result.status != COMMON_UTF8_OK)
            {
                return make_result(
                    COMMON_RELATIVE_PATH_INVALID_UTF8,
                    position);
            }
            position += sequence_size;
        }
    }
    return component_result(bytes, component_begin, path_size);
}

int common_relative_path_compare_bytes(
    const char *left,
    size_t left_size,
    const char *right,
    size_t right_size)
{
    const size_t common = left_size < right_size ? left_size : right_size;
    int order = 0;

    if (common != 0U)
    {
        if (left == NULL && right == NULL)
            order = 0;
        else if (left == NULL)
            return -1;
        else if (right == NULL)
            return 1;
        else
            order = memcmp(left, right, common);
    }
    if (order != 0)
        return order;
    if (left_size < right_size)
        return -1;
    if (left_size > right_size)
        return 1;
    return 0;
}

bool common_relative_path_is_equal_or_descendant(
    const char *candidate,
    size_t candidate_size,
    const char *root,
    size_t root_size)
{
    if (candidate == NULL || root == NULL || root_size == 0U ||
        candidate_size < root_size)
    {
        return false;
    }
    if (memcmp(candidate, root, root_size) != 0)
        return false;
    return candidate_size == root_size || candidate[root_size] == '/';
}

const char *common_relative_path_status_name(CommonRelativePathStatus status)
{
    switch (status)
    {
        case COMMON_RELATIVE_PATH_OK: return "ok";
        case COMMON_RELATIVE_PATH_INVALID_ARGUMENT: return "invalid-argument";
        case COMMON_RELATIVE_PATH_EMPTY: return "empty";
        case COMMON_RELATIVE_PATH_EMPTY_COMPONENT: return "empty-component";
        case COMMON_RELATIVE_PATH_DOT_COMPONENT: return "dot-component";
        case COMMON_RELATIVE_PATH_BACKSLASH: return "backslash";
        case COMMON_RELATIVE_PATH_CONTROL_CHARACTER: return "control-character";
        case COMMON_RELATIVE_PATH_INVALID_UTF8: return "invalid-utf8";
        default: return NULL;
    }
}
