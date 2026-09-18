// SPDX-License-Identifier: GPL-3.0-only

#include "common/ascii_glob.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool read_class_atom(const char** cursor, const char* end,
                            unsigned char* value) {
    if (!cursor || !*cursor || *cursor >= end || !value) return false;
    const char* current = *cursor;
    if (*current == '\\') {
        current++;
        if (current >= end) return false;
        *value = (unsigned char)*current++;
    } else {
        if (*current == '-') return false;
        *value = (unsigned char)*current++;
    }
    *cursor = current;
    return true;
}

/* Parses one complete class beginning at '['. When target and out_match are
 * supplied, also evaluates membership while validating the same grammar. */
static AsciiGlobStatus parse_class(const char* pattern,
                                   const char** out_after,
                                   const unsigned char* target,
                                   bool* out_match) {
    if (!pattern || pattern[0] != '[' || !out_after ||
        ((target == NULL) != (out_match == NULL))) {
        return ASCII_GLOB_INVALID_ARGUMENT;
    }

    const char* cursor = pattern + 1;
    bool negated = false;
    if (*cursor == '!' || *cursor == '^') {
        negated = true;
        cursor++;
    }
    const char* content = cursor;

    /* Locate the first unescaped closing bracket. */
    while (*cursor != '\0' && *cursor != ']') {
        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0') return ASCII_GLOB_MALFORMED_PATTERN;
        }
        cursor++;
    }
    if (*cursor != ']') return ASCII_GLOB_MALFORMED_PATTERN;
    const char* end = cursor;
    *out_after = end + 1;
    if (content == end) return ASCII_GLOB_MALFORMED_PATTERN;

    bool matched = false;
    size_t item_count = 0U;
    cursor = content;
    while (cursor < end) {
        /* A raw hyphen is a literal only at a class edge. Ranges involving a
         * hyphen endpoint remain expressible by escaping that endpoint. */
        if (*cursor == '-') {
            if (item_count != 0U && cursor + 1 != end) {
                return ASCII_GLOB_MALFORMED_PATTERN;
            }
            if (target && *target == (unsigned char)'-') matched = true;
            cursor++;
            item_count++;
            continue;
        }

        unsigned char first = 0U;
        if (!read_class_atom(&cursor, end, &first)) {
            return ASCII_GLOB_MALFORMED_PATTERN;
        }
        if (cursor < end && *cursor == '-' && cursor + 1 < end) {
            cursor++;
            unsigned char last = 0U;
            if (!read_class_atom(&cursor, end, &last) || first > last) {
                return ASCII_GLOB_MALFORMED_PATTERN;
            }
            if (target && *target >= first && *target <= last) matched = true;
        } else if (target && *target == first) {
            matched = true;
        }
        item_count++;
    }
    if (item_count == 0U) return ASCII_GLOB_MALFORMED_PATTERN;
    if (out_match) *out_match = negated ? !matched : matched;
    return ASCII_GLOB_OK;
}

AsciiGlobStatus ascii_glob_validate(const char* pattern) {
    if (!pattern) return ASCII_GLOB_INVALID_ARGUMENT;
    const char* cursor = pattern;
    while (*cursor != '\0') {
        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0') return ASCII_GLOB_MALFORMED_PATTERN;
            cursor++;
        } else if (*cursor == '[') {
            const char* after = NULL;
            AsciiGlobStatus status = parse_class(
                cursor, &after, NULL, NULL);
            if (status != ASCII_GLOB_OK) return status;
            cursor = after;
        } else {
            cursor++;
        }
    }
    return ASCII_GLOB_OK;
}

AsciiGlobStatus ascii_glob_match(const char* pattern, const char* text,
                                 bool* out_match) {
    if (out_match) *out_match = false;
    if (!pattern || !text || !out_match) return ASCII_GLOB_INVALID_ARGUMENT;

    AsciiGlobStatus status = ascii_glob_validate(pattern);
    if (status != ASCII_GLOB_OK) return status;

    size_t text_size = strlen(text);
    if (text_size == SIZE_MAX || text_size + 1U > SIZE_MAX / 2U) {
        return ASCII_GLOB_ALLOCATION_FAILED;
    }
    size_t row_size = text_size + 1U;
    uint8_t* rows = (uint8_t*)calloc(row_size * 2U, sizeof(*rows));
    if (!rows) return ASCII_GLOB_ALLOCATION_FAILED;
    uint8_t* previous = rows;
    uint8_t* current = rows + row_size;
    previous[0] = 1U;

    const char* cursor = pattern;
    while (*cursor != '\0') {
        if (*cursor == '*') {
            /* Consecutive stars have exactly one-star semantics. Collapsing
             * them preserves the O(P*T) bound while avoiding useless rows. */
            do {
                cursor++;
            } while (*cursor == '*');
            current[0] = previous[0];
            for (size_t index = 1U; index <= text_size; ++index) {
                current[index] = (uint8_t)(
                    previous[index] != 0U || current[index - 1U] != 0U);
            }
        } else if (*cursor == '?') {
            cursor++;
            current[0] = 0U;
            for (size_t index = 1U; index <= text_size; ++index) {
                current[index] = previous[index - 1U];
            }
        } else if (*cursor == '[') {
            const char* after = NULL;
            current[0] = 0U;
            for (size_t index = 1U; index <= text_size; ++index) {
                unsigned char target = (unsigned char)text[index - 1U];
                bool class_match = false;
                status = parse_class(cursor, &after, &target, &class_match);
                if (status != ASCII_GLOB_OK) {
                    free(rows);
                    return status;
                }
                current[index] = (uint8_t)(
                    previous[index - 1U] != 0U && class_match);
            }
            if (text_size == 0U) {
                status = parse_class(cursor, &after, NULL, NULL);
                if (status != ASCII_GLOB_OK) {
                    free(rows);
                    return status;
                }
            }
            cursor = after;
        } else {
            unsigned char literal;
            if (*cursor == '\\') {
                cursor++;
                literal = (unsigned char)*cursor++;
            } else {
                literal = (unsigned char)*cursor++;
            }
            current[0] = 0U;
            for (size_t index = 1U; index <= text_size; ++index) {
                current[index] = (uint8_t)(
                    previous[index - 1U] != 0U &&
                    (unsigned char)text[index - 1U] == literal);
            }
        }

        uint8_t* swap = previous;
        previous = current;
        current = swap;
    }

    *out_match = previous[text_size] != 0U;
    free(rows);
    return ASCII_GLOB_OK;
}

const char* ascii_glob_status_name(AsciiGlobStatus status) {
    switch (status) {
        case ASCII_GLOB_OK: return "ok";
        case ASCII_GLOB_INVALID_ARGUMENT: return "invalid-argument";
        case ASCII_GLOB_MALFORMED_PATTERN: return "malformed-pattern";
        case ASCII_GLOB_ALLOCATION_FAILED: return "allocation-failed";
        default: return "unknown";
    }
}
