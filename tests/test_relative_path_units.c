#include "common/relative_path.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check_impl(int condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(
            stderr,
            "test_relative_path_units:%d: check failed: %s\n",
            line,
            expression);
        ++g_failures;
    }
}

#define CHECK(expression) check_impl((expression) != 0, #expression, __LINE__)

static void expect_status(
    const uint8_t *bytes,
    size_t size,
    CommonRelativePathStatus status,
    size_t offset)
{
    CommonRelativePathResult result = common_relative_path_validate_utf8(
        (const char *)bytes, size);

    CHECK(result.status == status);
    CHECK(result.offset == offset);
}

static void test_validation(void)
{
    static const uint8_t embedded_nul[] = {'a', 0U, 'b'};
    static const uint8_t control[] = {'a', UINT8_C(0x1f), 'b'};
    static const uint8_t overlong[] = {'a', '/', UINT8_C(0xc0), UINT8_C(0x80)};
    static const uint8_t truncated_two[] = {'a', '/', UINT8_C(0xc2)};
    static const uint8_t truncated_three[] = {'a', '/', UINT8_C(0xe1), UINT8_C(0x80)};
    static const uint8_t truncated_four[] = {
        'a', '/', UINT8_C(0xf1), UINT8_C(0x80), UINT8_C(0x80)
    };
    static const uint8_t surrogate[] = {
        'a', '/', UINT8_C(0xed), UINT8_C(0xa0), UINT8_C(0x80)
    };
    static const uint8_t too_large[] = {
        'a', '/', UINT8_C(0xf4), UINT8_C(0x90), UINT8_C(0x80), UINT8_C(0x80)
    };
    static const uint8_t dot_before_invalid_utf8[] = {
        '.', '/', UINT8_C(0xff)
    };
    static const uint8_t invalid_utf8_before_control[] = {
        'a', '/', UINT8_C(0xe1), UINT8_C(0x1f), UINT8_C(0x80)
    };
    static const uint8_t invalid_utf8_before_backslash[] = {
        'a', '/', UINT8_C(0xe1), UINT8_C(0x80), '\\'
    };
    static const uint8_t valid_utf8[] = {
        'c', 'a', 'f', UINT8_C(0xc3), UINT8_C(0xa9), '/',
        UINT8_C(0xe9), UINT8_C(0x9b), UINT8_C(0xaa)
    };

    expect_status(NULL, 0U, COMMON_RELATIVE_PATH_INVALID_ARGUMENT, SIZE_MAX);
    expect_status((const uint8_t *)"", 0U, COMMON_RELATIVE_PATH_EMPTY, 0U);
    expect_status((const uint8_t *)"/a", 2U,
                  COMMON_RELATIVE_PATH_EMPTY_COMPONENT, 0U);
    expect_status((const uint8_t *)"a/", 2U,
                  COMMON_RELATIVE_PATH_EMPTY_COMPONENT, 2U);
    expect_status((const uint8_t *)"a//b", 4U,
                  COMMON_RELATIVE_PATH_EMPTY_COMPONENT, 2U);
    expect_status((const uint8_t *)".", 1U,
                  COMMON_RELATIVE_PATH_DOT_COMPONENT, 0U);
    expect_status((const uint8_t *)"a/../b", 6U,
                  COMMON_RELATIVE_PATH_DOT_COMPONENT, 2U);
    expect_status((const uint8_t *)"a\\b", 3U,
                  COMMON_RELATIVE_PATH_BACKSLASH, 1U);
    expect_status(embedded_nul, sizeof(embedded_nul),
                  COMMON_RELATIVE_PATH_CONTROL_CHARACTER, 1U);
    expect_status(control, sizeof(control),
                  COMMON_RELATIVE_PATH_CONTROL_CHARACTER, 1U);
    expect_status(overlong, sizeof(overlong),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(truncated_two, sizeof(truncated_two),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(truncated_three, sizeof(truncated_three),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(truncated_four, sizeof(truncated_four),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(surrogate, sizeof(surrogate),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(too_large, sizeof(too_large),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(dot_before_invalid_utf8, sizeof(dot_before_invalid_utf8),
                  COMMON_RELATIVE_PATH_DOT_COMPONENT, 0U);
    expect_status(invalid_utf8_before_control,
                  sizeof(invalid_utf8_before_control),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(invalid_utf8_before_backslash,
                  sizeof(invalid_utf8_before_backslash),
                  COMMON_RELATIVE_PATH_INVALID_UTF8, 2U);
    expect_status(valid_utf8, sizeof(valid_utf8),
                  COMMON_RELATIVE_PATH_OK, SIZE_MAX);
    expect_status((const uint8_t *)"a:b/file.", sizeof("a:b/file.") - 1U,
                  COMMON_RELATIVE_PATH_OK, SIZE_MAX);
}

static void test_relations_and_names(void)
{
    CHECK(common_relative_path_compare_bytes("a", 1U, "b", 1U) < 0);
    CHECK(common_relative_path_compare_bytes("a", 1U, "a", 1U) == 0);
    CHECK(common_relative_path_compare_bytes("a", 1U, "a/b", 3U) < 0);
    CHECK(common_relative_path_compare_bytes("a/b", 3U, "a", 1U) > 0);
    CHECK(common_relative_path_compare_bytes(NULL, 0U, NULL, 0U) == 0);

    CHECK(common_relative_path_is_equal_or_descendant("a", 1U, "a", 1U));
    CHECK(common_relative_path_is_equal_or_descendant("a/b", 3U, "a", 1U));
    CHECK(!common_relative_path_is_equal_or_descendant("a-b", 3U, "a", 1U));
    CHECK(!common_relative_path_is_equal_or_descendant("A/b", 3U, "a", 1U));
    CHECK(!common_relative_path_is_equal_or_descendant(NULL, 0U, "a", 1U));
    CHECK(!common_relative_path_is_equal_or_descendant("a", 1U, NULL, 0U));

    CHECK(strcmp(common_relative_path_status_name(
                     COMMON_RELATIVE_PATH_INVALID_UTF8),
                 "invalid-utf8") == 0);
    CHECK(common_relative_path_status_name((CommonRelativePathStatus)99) ==
          NULL);
}

int main(void)
{
    test_validation();
    test_relations_and_names();
    if (g_failures != 0)
    {
        fprintf(stderr, "%d relative-path test(s) failed\n", g_failures);
        return 1;
    }
    puts("UnityCommon relative-path tests passed");
    return 0;
}
