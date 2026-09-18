#include "common/ascii_glob.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static bool expect_match(const char* pattern, const char* text,
                         bool expected) {
    bool matched = !expected;
    return ascii_glob_validate(pattern) == ASCII_GLOB_OK &&
           ascii_glob_match(pattern, text, &matched) == ASCII_GLOB_OK &&
           matched == expected;
}

static bool expect_malformed(const char* pattern) {
    bool matched = true;
    return ascii_glob_validate(pattern) ==
               ASCII_GLOB_MALFORMED_PATTERN &&
           ascii_glob_match(pattern, "anything", &matched) ==
               ASCII_GLOB_MALFORMED_PATTERN &&
           !matched;
}

static bool test_pathological_patterns(void) {
    enum {
        ALTERNATING_COUNT = 2048,
        STAR_COUNT = 65536,
        STAR_TEXT_COUNT = 4096,
    };

    size_t alternating_size = (size_t)ALTERNATING_COUNT * 2U + 2U;
    char* alternating = (char*)malloc(alternating_size);
    char* alternating_text =
        (char*)malloc((size_t)ALTERNATING_COUNT + 1U);
    if (!alternating || !alternating_text) {
        free(alternating);
        free(alternating_text);
        return false;
    }
    for (size_t index = 0U; index < ALTERNATING_COUNT; ++index) {
        alternating[index * 2U] = '*';
        alternating[index * 2U + 1U] = 'a';
        alternating_text[index] = 'a';
    }
    alternating[(size_t)ALTERNATING_COUNT * 2U] = 'b';
    alternating[(size_t)ALTERNATING_COUNT * 2U + 1U] = '\0';
    alternating_text[ALTERNATING_COUNT] = '\0';
    bool ok = expect_match(alternating, alternating_text, false);
    free(alternating);
    free(alternating_text);
    if (!ok) return false;

    char* stars = (char*)malloc((size_t)STAR_COUNT + 2U);
    char* star_text = (char*)malloc((size_t)STAR_TEXT_COUNT + 2U);
    if (!stars || !star_text) {
        free(stars);
        free(star_text);
        return false;
    }
    memset(stars, '*', STAR_COUNT);
    stars[STAR_COUNT] = 'z';
    stars[STAR_COUNT + 1U] = '\0';
    memset(star_text, 'a', STAR_TEXT_COUNT);
    star_text[STAR_TEXT_COUNT] = 'z';
    star_text[STAR_TEXT_COUNT + 1U] = '\0';
    ok = expect_match(stars, star_text, true);
    free(stars);
    free(star_text);
    return ok;
}

int main(void) {
    CHECK(expect_match("", "", true));
    CHECK(expect_match("", "x", false));
    CHECK(expect_match("literal", "literal", true));
    CHECK(expect_match("literal", "Literal", false));
    CHECK(expect_match("*", "", true));
    CHECK(expect_match("*", "anything", true));
    CHECK(expect_match("a*b", "ab", true));
    CHECK(expect_match("a*b", "a/middle/b", true));
    CHECK(expect_match("a**b", "axxb", true));
    CHECK(expect_match("Hidden/*", "Hidden/Internal/Shader", true));
    CHECK(expect_match("?", "x", true));
    CHECK(expect_match("?", "", false));
    CHECK(expect_match("a?c", "abc", true));
    CHECK(expect_match("a?c", "abbc", false));

    CHECK(expect_match("a\\*b", "a*b", true));
    CHECK(expect_match("a\\*b", "axxb", false));
    CHECK(expect_match("\\?", "?", true));
    CHECK(expect_match("\\[", "[", true));
    CHECK(expect_match("\\\\", "\\", true));
    CHECK(expect_match("]", "]", true));

    CHECK(expect_match("[abc]", "a", true));
    CHECK(expect_match("[abc]", "d", false));
    CHECK(expect_match("[a-c]", "b", true));
    CHECK(expect_match("[a-cx-z]", "y", true));
    CHECK(expect_match("[a-cx-z]", "w", false));
    CHECK(expect_match("[A-Z]", "a", false));
    CHECK(expect_match("[!a-c]", "d", true));
    CHECK(expect_match("[!a-c]", "b", false));
    CHECK(expect_match("[^a-c]", "d", true));
    CHECK(expect_match("[^a-c]", "a", false));
    CHECK(expect_match("[-a]", "-", true));
    CHECK(expect_match("[a-]", "-", true));
    CHECK(expect_match("[a\\-c]", "-", true));
    CHECK(expect_match("[\\]]", "]", true));
    CHECK(expect_match("[\\\\]", "\\", true));
    CHECK(expect_match("[!!]", "!", false));
    CHECK(expect_match("[^^]", "^", false));

    /* Byte matching is independent of locale and does not decode UTF-8. */
    const char utf8_e_acute[] = "\xc3\xa9";
    CHECK(expect_match("??", utf8_e_acute, true));
    const char high_byte[] = "\xe9";
    const char high_range[] = "[\x80-\xff]";
    CHECK(expect_match(high_range, high_byte, true));

    CHECK(expect_malformed("["));
    CHECK(expect_malformed("[]"));
    CHECK(expect_malformed("[!]"));
    CHECK(expect_malformed("[^]"));
    CHECK(expect_malformed("[z-a]"));
    CHECK(expect_malformed("[a--b]"));
    CHECK(expect_malformed("[a-b-c]"));
    CHECK(expect_malformed("[\\]"));
    CHECK(expect_malformed("trailing\\"));

    bool matched = true;
    CHECK(ascii_glob_validate(NULL) == ASCII_GLOB_INVALID_ARGUMENT);
    CHECK(ascii_glob_match(NULL, "", &matched) ==
          ASCII_GLOB_INVALID_ARGUMENT && !matched);
    matched = true;
    CHECK(ascii_glob_match("*", NULL, &matched) ==
          ASCII_GLOB_INVALID_ARGUMENT && !matched);
    CHECK(ascii_glob_match("*", "", NULL) == ASCII_GLOB_INVALID_ARGUMENT);
    CHECK(strcmp(ascii_glob_status_name(ASCII_GLOB_MALFORMED_PATTERN),
                 "malformed-pattern") == 0);

    CHECK(test_pathological_patterns());
    printf("UnityCommon ASCII glob unit tests passed\n");
    return 0;
}
