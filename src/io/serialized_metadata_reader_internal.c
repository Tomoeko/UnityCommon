// SPDX-License-Identifier: GPL-3.0-only

#include "serialized_metadata_reader_internal.h"

#include "serialized_type_shape_internal.h"

enum {
    TYPE_CLASS_BYTES = 4,
    TYPE_STRIPPED_BYTES = 1,
    TYPE_SCRIPT_INDEX_BYTES = 2,
    TYPE_HASH_BYTES = 16,
    COUNT_BYTES = 4,
    TREE_NODE_BYTES = 32
};

static bool reject_reader(
    SerializedMetadataReader* reader, SerializedMetadataReadStatus status, uint64_t offset) {
    reader->status = status;
    reader->error_offset = offset;
    return false;
}

uint16_t serialized_metadata_decode_u16(const uint8_t* bytes, uint8_t endian_selector) {
    if (endian_selector) {
        return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
    }
    return (uint16_t)(((uint16_t)bytes[1] << 8U) | bytes[0]);
}

uint32_t serialized_metadata_decode_u32(const uint8_t* bytes, uint8_t endian_selector) {
    if (endian_selector) {
        return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
            ((uint32_t)bytes[2] << 8U) | bytes[3];
    }
    return ((uint32_t)bytes[3] << 24U) | ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[1] << 8U) |
        bytes[0];
}

uint64_t serialized_metadata_decode_u64(const uint8_t* bytes, uint8_t endian_selector) {
    if (endian_selector) {
        return ((uint64_t)serialized_metadata_decode_u32(bytes, endian_selector) << 32U) |
            serialized_metadata_decode_u32(bytes + 4U, endian_selector);
    }
    return ((uint64_t)serialized_metadata_decode_u32(bytes + 4U, endian_selector) << 32U) |
        serialized_metadata_decode_u32(bytes, endian_selector);
}

static bool admit_extent(SerializedMetadataReader* reader, uint64_t width) {
    if (reader->position > reader->metadata_end ||
        width > reader->metadata_end - reader->position) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_METADATA_END, reader->metadata_end);
    }
    if (reader->position > reader->mapped_size || width > reader->mapped_size - reader->position) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_MAPPING_END, reader->mapped_size);
    }
    const uint64_t consumed = reader->position - reader->byte_budget_origin;
    if (consumed > reader->max_bytes || width > reader->max_bytes - consumed) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_BYTE_LIMIT, reader->position);
    }
    return true;
}

/* Called only after extent admission. Mapping bounds make both casts exact. */
static bool consume_span(
    SerializedMetadataReader* reader, uint64_t width, SerializedFilePrefixSpan* out_span) {
    if (width > reader->max_work - *reader->work_used) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_WORK_LIMIT, reader->position);
    }
    *reader->work_used += width;
    *out_span = (SerializedFilePrefixSpan){
        reader->bytes + (size_t)reader->position, reader->position, (size_t)width};
    reader->position += width;
    return true;
}

bool serialized_metadata_take(
    SerializedMetadataReader* reader, uint64_t width, SerializedFilePrefixSpan* out_span) {
    return admit_extent(reader, width) && consume_span(reader, width, out_span);
}

bool serialized_metadata_read_u32(
    SerializedMetadataReader* reader, SerializedFilePrefixSpan* out_source, uint32_t* out_value) {
    if (!serialized_metadata_take(reader, COUNT_BYTES, out_source)) {
        return false;
    }
    *out_value = serialized_metadata_decode_u32(out_source->data, reader->endian_selector);
    return true;
}

bool serialized_metadata_read_count(
    SerializedMetadataReader* reader, SerializedFilePrefixSpan* out_source, uint32_t* out_value) {
    if (!serialized_metadata_read_u32(reader, out_source, out_value)) {
        return false;
    }
    if (*out_value > INT32_MAX) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_NEGATIVE_COUNT, out_source->offset);
    }
    return true;
}

bool serialized_metadata_read_type_prefix(
    SerializedMetadataReader* reader, SerializedMetadataTypePrefix* out_prefix) {
    const uint64_t start = reader->position;
    *out_prefix = (SerializedMetadataTypePrefix){
        .script_hash_source = {NULL, SERIALIZED_FILE_PREFIX_NO_OFFSET, 0}};
    if (!serialized_metadata_take(reader, TYPE_CLASS_BYTES, &out_prefix->class_id_source) ||
        !serialized_metadata_take(reader, TYPE_STRIPPED_BYTES, &out_prefix->stripped_source) ||
        !serialized_metadata_take(
            reader, TYPE_SCRIPT_INDEX_BYTES, &out_prefix->script_index_source)) {
        return false;
    }
    out_prefix->class_id_bits =
        serialized_metadata_decode_u32(out_prefix->class_id_source.data, reader->endian_selector);
    out_prefix->stripped_raw = out_prefix->stripped_source.data[0];
    out_prefix->script_index_bits = serialized_metadata_decode_u16(
        out_prefix->script_index_source.data, reader->endian_selector);
    if (!serialized_type_v22_hash_shape(out_prefix->class_id_bits,
            out_prefix->script_index_bits,
            &out_prefix->has_script_hash)) {
        return reject_reader(reader, SERIALIZED_METADATA_READ_TYPE_SHAPE, start);
    }
    if (out_prefix->has_script_hash &&
        !serialized_metadata_take(reader, TYPE_HASH_BYTES, &out_prefix->script_hash_source)) {
        return false;
    }
    return serialized_metadata_take(reader, TYPE_HASH_BYTES, &out_prefix->type_hash_source);
}

bool serialized_metadata_read_tree(SerializedMetadataReader* reader,
    const SerializedMetadataTreeBudget* budget,
    SerializedFileDirectoryTree* out_tree) {
    if (!serialized_metadata_read_u32(
            reader, &out_tree->node_count_source, &out_tree->node_count)) {
        return false;
    }
    if (!out_tree->node_count) {
        /* The pinned readers and writer consume different zero-node headers.
         * Refuse an ambiguous boundary instead of selecting either layout. */
        return reject_reader(
            reader, SERIALIZED_METADATA_READ_TREE_SHAPE, out_tree->node_count_source.offset);
    }
    if (out_tree->node_count > budget->max_node_records - *budget->node_records) {
        return reject_reader(
            reader, SERIALIZED_METADATA_READ_NODE_LIMIT, out_tree->node_count_source.offset);
    }
    *budget->node_records += out_tree->node_count;
    if (!serialized_metadata_read_u32(
            reader, &out_tree->string_count_source, &out_tree->string_byte_count)) {
        return false;
    }
    if (out_tree->string_byte_count > budget->max_string_bytes - *budget->string_bytes) {
        return reject_reader(
            reader, SERIALIZED_METADATA_READ_STRING_LIMIT, out_tree->string_count_source.offset);
    }
    *budget->string_bytes += out_tree->string_byte_count;
    return serialized_metadata_take(
               reader, (uint64_t)out_tree->node_count * TREE_NODE_BYTES, &out_tree->nodes_source) &&
        serialized_metadata_take(reader, out_tree->string_byte_count, &out_tree->strings_source);
}

bool serialized_metadata_read_terminated_string(SerializedMetadataReader* reader,
    uint64_t max_terminated_bytes,
    uint64_t* terminated_bytes,
    SerializedFilePrefixSpan* out_source) {
    const uint64_t start = reader->position;
    for (;;) {
        if (!admit_extent(reader, 1U)) {
            return false;
        }
        if (*terminated_bytes == max_terminated_bytes) {
            return reject_reader(
                reader, SERIALIZED_METADATA_READ_TERMINATED_STRING_LIMIT, reader->position);
        }
        SerializedFilePrefixSpan byte;
        if (!consume_span(reader, 1U, &byte)) {
            return false;
        }
        ++*terminated_bytes;
        if (byte.data[0] == 0U) {
            *out_source = (SerializedFilePrefixSpan){
                reader->bytes + (size_t)start, start, (size_t)(reader->position - start)};
            return true;
        }
    }
}
