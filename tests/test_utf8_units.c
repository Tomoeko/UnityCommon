#include "common/utf8.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check_impl(int condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(
            stderr,
            "test_utf8_units:%d: check failed: %s\n",
            line,
            expression);
        ++g_failures;
    }
}

#define CHECK(expression) check_impl((expression) != 0, #expression, __LINE__)

static void expect_status(
    const uint8_t *bytes,
    size_t byte_count,
    CommonUtf8Status status,
    size_t offset)
{
    CommonUtf8Result result = common_utf8_validate(bytes, byte_count);

    CHECK(result.status == status);
    CHECK(result.offset == offset);
}

static void test_valid_boundaries(void)
{
    static const uint8_t empty[] = {0U};
    static const uint8_t ascii[] = {0U, UINT8_C(0x1f), 'A', UINT8_C(0x7f)};
    static const uint8_t two_byte[] = {
        UINT8_C(0xc2), UINT8_C(0x80),
        UINT8_C(0xdf), UINT8_C(0xbf)
    };
    static const uint8_t three_byte[] = {
        UINT8_C(0xe0), UINT8_C(0xa0), UINT8_C(0x80),
        UINT8_C(0xed), UINT8_C(0x9f), UINT8_C(0xbf),
        UINT8_C(0xee), UINT8_C(0x80), UINT8_C(0x80),
        UINT8_C(0xef), UINT8_C(0xbf), UINT8_C(0xbf)
    };
    static const uint8_t four_byte[] = {
        UINT8_C(0xf0), UINT8_C(0x90), UINT8_C(0x80), UINT8_C(0x80),
        UINT8_C(0xf4), UINT8_C(0x8f), UINT8_C(0xbf), UINT8_C(0xbf)
    };

    expect_status(empty, 0U, COMMON_UTF8_OK, SIZE_MAX);
    expect_status(ascii, sizeof(ascii), COMMON_UTF8_OK, SIZE_MAX);
    expect_status(two_byte, sizeof(two_byte), COMMON_UTF8_OK, SIZE_MAX);
    expect_status(three_byte, sizeof(three_byte), COMMON_UTF8_OK, SIZE_MAX);
    expect_status(four_byte, sizeof(four_byte), COMMON_UTF8_OK, SIZE_MAX);
}

static void test_invalid_sequences(void)
{
    static const uint8_t stray_continuation[] = {UINT8_C(0x80)};
    static const uint8_t overlong_two_c0[] = {UINT8_C(0xc0), UINT8_C(0x80)};
    static const uint8_t overlong_two_c1[] = {UINT8_C(0xc1), UINT8_C(0xbf)};
    static const uint8_t bad_two_continuation[] = {UINT8_C(0xc2), 'A'};
    static const uint8_t truncated_two[] = {UINT8_C(0xc2)};
    static const uint8_t overlong_three[] = {
        UINT8_C(0xe0), UINT8_C(0x9f), UINT8_C(0xbf)
    };
    static const uint8_t bad_three_continuation[] = {
        UINT8_C(0xe1), UINT8_C(0x80), 'A'
    };
    static const uint8_t truncated_three[] = {
        UINT8_C(0xe1), UINT8_C(0x80)
    };
    static const uint8_t surrogate_first[] = {
        UINT8_C(0xed), UINT8_C(0xa0), UINT8_C(0x80)
    };
    static const uint8_t surrogate_last[] = {
        UINT8_C(0xed), UINT8_C(0xbf), UINT8_C(0xbf)
    };
    static const uint8_t overlong_four[] = {
        UINT8_C(0xf0), UINT8_C(0x8f), UINT8_C(0xbf), UINT8_C(0xbf)
    };
    static const uint8_t too_large_boundary[] = {
        UINT8_C(0xf4), UINT8_C(0x90), UINT8_C(0x80), UINT8_C(0x80)
    };
    static const uint8_t too_large_lead[] = {
        UINT8_C(0xf5), UINT8_C(0x80), UINT8_C(0x80), UINT8_C(0x80)
    };
    static const uint8_t truncated_four[] = {
        UINT8_C(0xf0), UINT8_C(0x90), UINT8_C(0x80)
    };
    static const uint8_t invalid_after_prefix[] = {
        'A', UINT8_C(0xc2), UINT8_C(0xa2), 'B', UINT8_C(0xed),
        UINT8_C(0xa0), UINT8_C(0x80)
    };

    expect_status(NULL, 0U, COMMON_UTF8_INVALID_ARGUMENT, SIZE_MAX);
    expect_status(stray_continuation, sizeof(stray_continuation),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(overlong_two_c0, sizeof(overlong_two_c0),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(overlong_two_c1, sizeof(overlong_two_c1),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(bad_two_continuation, sizeof(bad_two_continuation),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(truncated_two, sizeof(truncated_two),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(overlong_three, sizeof(overlong_three),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(bad_three_continuation, sizeof(bad_three_continuation),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(truncated_three, sizeof(truncated_three),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(surrogate_first, sizeof(surrogate_first),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(surrogate_last, sizeof(surrogate_last),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(overlong_four, sizeof(overlong_four),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(too_large_boundary, sizeof(too_large_boundary),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(too_large_lead, sizeof(too_large_lead),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(truncated_four, sizeof(truncated_four),
                  COMMON_UTF8_INVALID_ENCODING, 0U);
    expect_status(invalid_after_prefix, sizeof(invalid_after_prefix),
                  COMMON_UTF8_INVALID_ENCODING, 4U);
}

static void expect_decode(
    const uint8_t *bytes,
    size_t byte_count,
    uint32_t expected_scalar,
    size_t expected_size)
{
    uint32_t scalar = UINT32_MAX;
    size_t sequence_size = SIZE_MAX;
    CommonUtf8Result result = common_utf8_decode_one(
        bytes,
        byte_count,
        &scalar,
        &sequence_size);

    CHECK(result.status == COMMON_UTF8_OK);
    CHECK(result.offset == SIZE_MAX);
    CHECK(scalar == expected_scalar);
    CHECK(sequence_size == expected_size);
}

static void test_decode_one(void)
{
    static const uint8_t ascii[] = {'A', UINT8_C(0xff)};
    static const uint8_t cent[] = {UINT8_C(0xc2), UINT8_C(0xa2), 'A'};
    static const uint8_t euro[] = {
        UINT8_C(0xe2), UINT8_C(0x82), UINT8_C(0xac), 'A'
    };
    static const uint8_t maximum[] = {
        UINT8_C(0xf4), UINT8_C(0x8f), UINT8_C(0xbf), UINT8_C(0xbf), 'A'
    };
    static const uint8_t malformed[] = {UINT8_C(0xe0), UINT8_C(0x80), 0U};
    uint32_t scalar = UINT32_C(0x12345678);
    size_t sequence_size = 123U;
    CommonUtf8Result result;

    expect_decode(ascii, sizeof(ascii), UINT32_C(0x41), 1U);
    expect_decode(cent, sizeof(cent), UINT32_C(0xa2), 2U);
    expect_decode(euro, sizeof(euro), UINT32_C(0x20ac), 3U);
    expect_decode(maximum, sizeof(maximum), UINT32_C(0x10ffff), 4U);

    result = common_utf8_decode_one(
        malformed,
        sizeof(malformed),
        &scalar,
        &sequence_size);
    CHECK(result.status == COMMON_UTF8_INVALID_ENCODING);
    CHECK(result.offset == 0U);
    CHECK(scalar == UINT32_C(0x12345678));
    CHECK(sequence_size == 123U);

    result = common_utf8_decode_one(
        malformed,
        0U,
        &scalar,
        &sequence_size);
    CHECK(result.status == COMMON_UTF8_INVALID_ENCODING);
    CHECK(result.offset == 0U);
    CHECK(scalar == UINT32_C(0x12345678));
    CHECK(sequence_size == 123U);

    result = common_utf8_decode_one(
        NULL,
        0U,
        &scalar,
        &sequence_size);
    CHECK(result.status == COMMON_UTF8_INVALID_ARGUMENT);
    CHECK(result.offset == SIZE_MAX);
    CHECK(scalar == UINT32_C(0x12345678));
    CHECK(sequence_size == 123U);

    result = common_utf8_decode_one(
        ascii,
        sizeof(ascii),
        NULL,
        &sequence_size);
    CHECK(result.status == COMMON_UTF8_INVALID_ARGUMENT);
    CHECK(sequence_size == 123U);

    result = common_utf8_decode_one(
        ascii,
        sizeof(ascii),
        &scalar,
        NULL);
    CHECK(result.status == COMMON_UTF8_INVALID_ARGUMENT);
    CHECK(scalar == UINT32_C(0x12345678));
}

static void test_status_names(void)
{
    CHECK(strcmp(common_utf8_status_name(COMMON_UTF8_OK), "ok") == 0);
    CHECK(strcmp(
              common_utf8_status_name(COMMON_UTF8_INVALID_ARGUMENT),
              "invalid-argument") == 0);
    CHECK(strcmp(
              common_utf8_status_name(COMMON_UTF8_INVALID_ENCODING),
              "invalid-encoding") == 0);
    CHECK(common_utf8_status_name((CommonUtf8Status)99) == NULL);
}

int main(void)
{
    test_valid_boundaries();
    test_invalid_sequences();
    test_decode_one();
    test_status_names();
    if (g_failures != 0)
    {
        fprintf(stderr, "%d UTF-8 test(s) failed\n", g_failures);
        return 1;
    }
    puts("UnityCommon UTF-8 tests passed");
    return 0;
}
