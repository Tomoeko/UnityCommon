// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_METADATA_READER_INTERNAL_H
#define SERIALIZED_METADATA_READER_INTERNAL_H

#include "io/serialized_file_directory.h"

typedef enum SerializedMetadataReadStatus {
    SERIALIZED_METADATA_READ_OK = 0,
    SERIALIZED_METADATA_READ_METADATA_END,
    SERIALIZED_METADATA_READ_MAPPING_END,
    SERIALIZED_METADATA_READ_BYTE_LIMIT,
    SERIALIZED_METADATA_READ_WORK_LIMIT,
    SERIALIZED_METADATA_READ_NEGATIVE_COUNT,
    SERIALIZED_METADATA_READ_TYPE_SHAPE,
    SERIALIZED_METADATA_READ_TREE_SHAPE,
    SERIALIZED_METADATA_READ_NODE_LIMIT,
    SERIALIZED_METADATA_READ_STRING_LIMIT,
    SERIALIZED_METADATA_READ_TERMINATED_STRING_LIMIT
} SerializedMetadataReadStatus;

/* Private physical cursor. Callers establish a non-NULL backing, metadata_end,
 * selector 0/1,
 * byte_budget_origin <= position and *work_used <= max_work. Keep the backing
 * immutable and the output spans disjoint from it. Table/row diagnostics and
 * ownership remain with the caller; error_offset always uses slice coordinates.
 * A failure may advance this private cursor before the failing field, but never
 * beyond a checked extent. Stop using it after its first failure. */
typedef struct SerializedMetadataReader {
    const uint8_t* bytes;
    size_t mapped_size;
    uint64_t metadata_end;
    uint64_t byte_budget_origin;
    uint64_t max_bytes;
    uint64_t max_work;
    uint64_t* work_used;
    uint64_t position;
    uint8_t endian_selector;
    SerializedMetadataReadStatus status;
    uint64_t error_offset;
} SerializedMetadataReader;

typedef struct SerializedMetadataTypePrefix {
    SerializedFilePrefixSpan class_id_source;
    SerializedFilePrefixSpan stripped_source;
    SerializedFilePrefixSpan script_index_source;
    SerializedFilePrefixSpan script_hash_source;
    SerializedFilePrefixSpan type_hash_source;
    uint32_t class_id_bits;
    uint16_t script_index_bits;
    uint8_t stripped_raw;
    bool has_script_hash;
} SerializedMetadataTypePrefix;

/* Aggregate counters start at or below their corresponding caps. */
typedef struct SerializedMetadataTreeBudget {
    uint64_t max_node_records;
    uint64_t max_string_bytes;
    uint64_t* node_records;
    uint64_t* string_bytes;
} SerializedMetadataTreeBudget;

/* Decode only already admitted bytes. The selector is an established 0 or 1. */
uint16_t serialized_metadata_decode_u16(const uint8_t* bytes, uint8_t endian_selector);
uint32_t serialized_metadata_decode_u32(const uint8_t* bytes, uint8_t endian_selector);
uint64_t serialized_metadata_decode_u64(const uint8_t* bytes, uint8_t endian_selector);

/* Check declared metadata, mapping, distinct-byte cap and work in that order;
 * charge width before reading. Successful empty spans retain their real address. */
bool serialized_metadata_take(
    SerializedMetadataReader* reader, uint64_t width, SerializedFilePrefixSpan* out_span);
bool serialized_metadata_read_u32(
    SerializedMetadataReader* reader, SerializedFilePrefixSpan* out_source, uint32_t* out_value);
bool serialized_metadata_read_count(
    SerializedMetadataReader* reader, SerializedFilePrefixSpan* out_source, uint32_t* out_value);

/* Read through an available NUL, retaining it in the output span. The live
 * aggregate counter starts at or below max_terminated_bytes. Admit metadata,
 * mapping and distinct-byte caps, then the terminated-byte cap, then charge
 * each visited byte before reading. No unbounded scan or byte copy occurs.
 * On failure the output span is untouched; cursor/counter may retain a prefix.
 * Reader, counter and output are disjoint writable objects, disjoint from the
 * immutable backing; all pointers are required. */
bool serialized_metadata_read_terminated_string(SerializedMetadataReader* reader,
    uint64_t max_terminated_bytes,
    uint64_t* terminated_bytes,
    SerializedFilePrefixSpan* out_source);

/* Exact 2021.3.35f1/2021.3.29f1 type-prefix premise is established by the owner.
 * This reads no ordinary dependency or reference-name trailer. */
bool serialized_metadata_read_type_prefix(
    SerializedMetadataReader* reader, SerializedMetadataTypePrefix* out_prefix);
bool serialized_metadata_read_tree(SerializedMetadataReader* reader,
    const SerializedMetadataTreeBudget* budget,
    SerializedFileDirectoryTree* out_tree);

#endif
