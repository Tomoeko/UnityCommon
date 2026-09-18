// SPDX-License-Identifier: GPL-3.0-only

#include "io/lz4_decompress.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
    LZ4_EXTENDED_LENGTH = 15,
    LZ4_EXTENSION_CONTINUES = 255,
    LZ4_MINIMUM_MATCH = 4,
    LZ4_FINAL_LITERALS = 5,
    LZ4_LAST_MATCH_DISTANCE = 12
};

static bool read_length(
    const uint8_t* source, size_t source_bytes, size_t* input_position, size_t* length) {
    if (*length != LZ4_EXTENDED_LENGTH) {
        return true;
    }
    uint8_t extension;
    do {
        if (*input_position >= source_bytes) {
            return false;
        }
        extension = source[*input_position];
        ++*input_position;
        if (*length > SIZE_MAX - (size_t)extension) {
            return false;
        }
        *length += extension;
    } while (extension == LZ4_EXTENSION_CONTINUES);
    return true;
}

static int decompress_block(const uint8_t* source,
    uint8_t* destination,
    int source_bytes,
    int destination_bytes,
    bool exact) {
    if (source_bytes < 0 || destination_bytes < 0 || (!source && source_bytes != 0) ||
        (!destination && destination_bytes != 0)) {
        return -1;
    }
    const size_t source_size = (size_t)source_bytes;
    const size_t destination_size = (size_t)destination_bytes;
    size_t input_position = 0;
    size_t output_position = 0;
    size_t last_match_start = 0;
    bool matched = false;

    while (input_position < source_size) {
        uint8_t token = source[input_position];
        ++input_position;
        size_t literal_length = token >> 4;
        if (!read_length(source, source_size, &input_position, &literal_length)) {
            return -1;
        }

        /* Compare integer extents before forming any derived pointer. */
        if (literal_length > source_size - input_position ||
            literal_length > destination_size - output_position) {
            return -1;
        }
        if (literal_length != 0) {
            if (!destination) {
                return -1;
            }
            memcpy(destination + output_position, source + input_position, literal_length);
        }
        output_position += literal_length;
        input_position += literal_length;

        if (input_position == source_size) {
            bool final_size_matches = output_position == destination_size;
            bool final_match_window_valid = !matched ||
                (literal_length >= LZ4_FINAL_LITERALS &&
                    output_position - last_match_start >= LZ4_LAST_MATCH_DISTANCE);
            if (exact && (!final_size_matches || !final_match_window_valid)) {
                return -1;
            }
            return (int)output_position;
        }

        if (source_size - input_position < 2u) {
            return -1;
        }
        uint16_t offset = (uint16_t)source[input_position] |
            (uint16_t)((uint16_t)source[input_position + 1u] << 8u);
        input_position += 2u;
        if (offset == 0 || (size_t)offset > output_position) {
            return -1;
        }

        size_t match_length = token & 0x0f;
        if (!read_length(source, source_size, &input_position, &match_length) ||
            match_length > SIZE_MAX - LZ4_MINIMUM_MATCH) {
            return -1;
        }
        match_length += LZ4_MINIMUM_MATCH;

        if (match_length > destination_size - output_position) {
            return -1;
        }
        size_t match_position = output_position - (size_t)offset;
        last_match_start = output_position;
        matched = true;

        /* Newly written bytes are available to later bytes of an overlapping match. */
        for (size_t index = 0; index < match_length; ++index) {
            destination[output_position] = destination[match_position];
            ++output_position;
            ++match_position;
        }
    }

    /* Only the legacy entry point admits empty input or a match-ending block. */
    return exact ? -1 : (int)output_position;
}

int lz4_decompress_safe(
    const uint8_t* source, uint8_t* destination, int source_bytes, int destination_bytes) {
    return decompress_block(source, destination, source_bytes, destination_bytes, false);
}

int lz4_decompress_exact(
    const uint8_t* source, uint8_t* destination, int source_bytes, int destination_bytes) {
    return decompress_block(source, destination, source_bytes, destination_bytes, true);
}
