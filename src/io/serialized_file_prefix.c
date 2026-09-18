// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_prefix.h"

#include <stdbool.h>

_Static_assert(SIZE_MAX <= UINT64_MAX, "Mapped byte counts must fit file coordinates");

enum {
    V22_FORMAT_VERSION = 22,
    V22_FORMAT_OFFSET = 8,
    V22_METADATA_SIZE_OFFSET = 16,
    V22_FILE_SIZE_OFFSET = 24,
    V22_DATA_OFFSET = 32,
    V22_ENDIAN_SELECTOR_OFFSET = 40,
    V22_OPAQUE_OFFSET = 41,
    V22_OPAQUE_SIZE = 7,
    TARGET_PLATFORM_SIZE = 4,
};

typedef struct {
    uint64_t maximum;
    uint64_t used;
} PrefixWork;

static SerializedFilePrefixResult query_result(
    SerializedFilePrefixStatus status, uint64_t work_used, uint64_t error_offset) {
    SerializedFilePrefixResult result = {status, work_used, error_offset};
    return result;
}

static bool charge_work(PrefixWork* work, uint64_t amount) {
    if (amount > work->maximum - work->used) {
        return false;
    }
    work->used += amount;
    return true;
}

static bool storage_overlaps(
    const void* first, size_t first_size, const void* second, size_t second_size) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }

    uintptr_t first_address = (uintptr_t)first;
    uintptr_t second_address = (uintptr_t)second;
    if (first_address <= second_address) {
        return second_address - first_address < first_size;
    }
    return first_address - second_address < second_size;
}

static bool valid_mapping_argument(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    const void* output,
    size_t output_size) {
    return output != NULL && (bytes != NULL || mapped_size == 0) &&
        (uint64_t)mapped_size <= logical_size &&
        !storage_overlaps(bytes, mapped_size, output, output_size);
}

static uint32_t read_be32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
        (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t* bytes) {
    return ((uint64_t)read_be32(bytes) << 32U) | (uint64_t)read_be32(bytes + 4);
}

static uint32_t read_target_platform(const uint8_t* bytes, uint8_t endian_selector) {
    if (endian_selector == 1) {
        return read_be32(bytes);
    }
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

/* Every caller has already established that this complete span is mapped. */
static SerializedFilePrefixSpan mapped_span(const uint8_t* bytes, uint64_t offset, size_t size) {
    SerializedFilePrefixSpan span = {bytes + (size_t)offset, offset, size};
    return span;
}

static SerializedFilePrefixResult query_header(const uint8_t* bytes,
    size_t mapped_size,
    uint64_t logical_size,
    PrefixWork* work,
    SerializedFileHeaderView* header) {
    if (logical_size < SERIALIZED_FILE_V22_HEADER_SIZE) {
        return query_result(SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, work->used, logical_size);
    }
    if (mapped_size < SERIALIZED_FILE_V22_HEADER_SIZE) {
        return query_result(SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, work->used, mapped_size);
    }
    if (!charge_work(work, SERIALIZED_FILE_V22_HEADER_SIZE)) {
        return query_result(SERIALIZED_FILE_PREFIX_WORK_LIMIT, work->used, 0);
    }

    uint32_t format_version = read_be32(bytes + V22_FORMAT_OFFSET);
    if (format_version != V22_FORMAT_VERSION) {
        return query_result(
            SERIALIZED_FILE_PREFIX_UNSUPPORTED_VERSION, work->used, V22_FORMAT_OFFSET);
    }
    uint8_t endian_selector = bytes[V22_ENDIAN_SELECTOR_OFFSET];
    if (endian_selector > 1) {
        return query_result(
            SERIALIZED_FILE_PREFIX_UNSUPPORTED_ENDIAN, work->used, V22_ENDIAN_SELECTOR_OFFSET);
    }

    /* The v22 writer stores three complete big-endian 64-bit values. Byte 16
     * belongs to the metadata count; it is not the metadata byte-order byte. */
    uint64_t metadata_size = read_be64(bytes + V22_METADATA_SIZE_OFFSET);
    uint64_t file_size = read_be64(bytes + V22_FILE_SIZE_OFFSET);
    uint64_t data_offset = read_be64(bytes + V22_DATA_OFFSET);
    if (file_size != logical_size) {
        return query_result(
            SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, work->used, V22_FILE_SIZE_OFFSET);
    }
    if (data_offset > file_size) {
        return query_result(SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, work->used, V22_DATA_OFFSET);
    }
    if (metadata_size > UINT64_MAX - SERIALIZED_FILE_V22_HEADER_SIZE) {
        return query_result(
            SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, work->used, V22_METADATA_SIZE_OFFSET);
    }
    uint64_t metadata_end = SERIALIZED_FILE_V22_HEADER_SIZE + metadata_size;
    if (metadata_end > data_offset) {
        return query_result(
            SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT, work->used, V22_METADATA_SIZE_OFFSET);
    }

    header->header_source = mapped_span(bytes, 0, SERIALIZED_FILE_V22_HEADER_SIZE);
    header->legacy_metadata_size_source = mapped_span(bytes, 0, 4);
    header->legacy_file_size_source = mapped_span(bytes, 4, 4);
    header->format_version_source = mapped_span(bytes, V22_FORMAT_OFFSET, 4);
    header->legacy_data_offset_source = mapped_span(bytes, 12, 4);
    header->metadata_size_source = mapped_span(bytes, V22_METADATA_SIZE_OFFSET, 8);
    header->file_size_source = mapped_span(bytes, V22_FILE_SIZE_OFFSET, 8);
    header->data_offset_source = mapped_span(bytes, V22_DATA_OFFSET, 8);
    header->endian_selector_source = mapped_span(bytes, V22_ENDIAN_SELECTOR_OFFSET, 1);
    header->opaque_source = mapped_span(bytes, V22_OPAQUE_OFFSET, V22_OPAQUE_SIZE);
    header->format_version = format_version;
    header->metadata_size = metadata_size;
    header->file_size = file_size;
    header->data_offset = data_offset;
    header->endian_selector = endian_selector;
    header->metadata = (SerializedFilePrefixRange){SERIALIZED_FILE_V22_HEADER_SIZE, metadata_size};
    header->metadata_to_data_gap =
        (SerializedFilePrefixRange){metadata_end, data_offset - metadata_end};
    header->data = (SerializedFilePrefixRange){data_offset, file_size - data_offset};
    return query_result(SERIALIZED_FILE_PREFIX_OK, work->used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
}

static SerializedFilePrefixResult require_metadata_bytes(uint64_t cursor,
    size_t field_size,
    uint64_t metadata_end,
    size_t mapped_size,
    uint64_t work_used) {
    if (cursor > metadata_end || (uint64_t)field_size > metadata_end - cursor) {
        return query_result(SERIALIZED_FILE_PREFIX_MALFORMED_METADATA, work_used, metadata_end);
    }
    if (cursor > (uint64_t)mapped_size || field_size > mapped_size - (size_t)cursor) {
        return query_result(SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING, work_used, mapped_size);
    }
    return query_result(SERIALIZED_FILE_PREFIX_OK, work_used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
}

SerializedFilePrefixResult serialized_file_header_query(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    uint64_t max_work,
    SerializedFileHeaderView* out_header) {
    if (!valid_mapping_argument(
            mapped_prefix, mapped_size, logical_size, out_header, sizeof(*out_header))) {
        return query_result(
            SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT, 0, SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }

    PrefixWork work = {max_work, 0};
    SerializedFileHeaderView header = {0};
    SerializedFilePrefixResult result =
        query_header(mapped_prefix, mapped_size, logical_size, &work, &header);
    if (result.status != SERIALIZED_FILE_PREFIX_OK) {
        return result;
    }
    if (!charge_work(&work, 1)) {
        return query_result(
            SERIALIZED_FILE_PREFIX_WORK_LIMIT, work.used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }

    *out_header = header;
    return query_result(SERIALIZED_FILE_PREFIX_OK, work.used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
}

SerializedFilePrefixResult serialized_file_prefix_query(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFilePrefixLimits* limits,
    SerializedFilePrefixView* out_prefix) {
    if (limits == NULL ||
        !valid_mapping_argument(
            mapped_prefix, mapped_size, logical_size, out_prefix, sizeof(*out_prefix)) ||
        storage_overlaps(limits, sizeof(*limits), out_prefix, sizeof(*out_prefix))) {
        return query_result(
            SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT, 0, SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }

    PrefixWork work = {limits->max_work, 0};
    SerializedFilePrefixView prefix = {0};
    SerializedFilePrefixResult result =
        query_header(mapped_prefix, mapped_size, logical_size, &work, &prefix.header);
    if (result.status != SERIALIZED_FILE_PREFIX_OK) {
        return result;
    }

    uint64_t metadata_end = prefix.header.metadata_to_data_gap.offset;
    uint64_t cursor = SERIALIZED_FILE_V22_HEADER_SIZE;
    size_t version_size = 0;
    for (;;) {
        result = require_metadata_bytes(cursor, 1, metadata_end, mapped_size, work.used);
        if (result.status != SERIALIZED_FILE_PREFIX_OK) {
            return result;
        }
        if (version_size >= limits->max_version_bytes) {
            return query_result(SERIALIZED_FILE_PREFIX_VERSION_LIMIT, work.used, cursor);
        }
        if (!charge_work(&work, 1)) {
            return query_result(SERIALIZED_FILE_PREFIX_WORK_LIMIT, work.used, cursor);
        }
        if (mapped_prefix[(size_t)cursor] == 0) {
            break;
        }
        ++cursor;
        ++version_size;
    }
    prefix.version_source =
        mapped_span(mapped_prefix, SERIALIZED_FILE_V22_HEADER_SIZE, version_size);
    prefix.version_terminator_source = mapped_span(mapped_prefix, cursor, 1);
    ++cursor;

    result =
        require_metadata_bytes(cursor, TARGET_PLATFORM_SIZE, metadata_end, mapped_size, work.used);
    if (result.status != SERIALIZED_FILE_PREFIX_OK) {
        return result;
    }
    if (!charge_work(&work, TARGET_PLATFORM_SIZE)) {
        return query_result(SERIALIZED_FILE_PREFIX_WORK_LIMIT, work.used, cursor);
    }
    prefix.target_platform_source = mapped_span(mapped_prefix, cursor, TARGET_PLATFORM_SIZE);
    prefix.target_platform =
        read_target_platform(mapped_prefix + (size_t)cursor, prefix.header.endian_selector);
    cursor += TARGET_PLATFORM_SIZE;

    result = require_metadata_bytes(cursor, 1, metadata_end, mapped_size, work.used);
    if (result.status != SERIALIZED_FILE_PREFIX_OK) {
        return result;
    }
    if (!charge_work(&work, 1)) {
        return query_result(SERIALIZED_FILE_PREFIX_WORK_LIMIT, work.used, cursor);
    }
    prefix.type_tree_source = mapped_span(mapped_prefix, cursor, 1);
    prefix.type_tree_enabled_raw = mapped_prefix[(size_t)cursor];
    ++cursor;
    prefix.metadata_prefix_source = mapped_span(mapped_prefix,
        SERIALIZED_FILE_V22_HEADER_SIZE,
        (size_t)(cursor - SERIALIZED_FILE_V22_HEADER_SIZE));
    prefix.remaining_metadata = (SerializedFilePrefixRange){cursor, metadata_end - cursor};
    if (!charge_work(&work, 1)) {
        return query_result(
            SERIALIZED_FILE_PREFIX_WORK_LIMIT, work.used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
    }

    *out_prefix = prefix;
    return query_result(SERIALIZED_FILE_PREFIX_OK, work.used, SERIALIZED_FILE_PREFIX_NO_OFFSET);
}
