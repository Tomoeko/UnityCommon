// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_PREFIX_H
#define SERIALIZED_FILE_PREFIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIALIZED_FILE_V22_HEADER_SIZE 48U
#define SERIALIZED_FILE_PREFIX_NO_OFFSET UINT64_MAX

typedef enum {
    SERIALIZED_FILE_PREFIX_OK = 0,
    SERIALIZED_FILE_PREFIX_INVALID_ARGUMENT,
    SERIALIZED_FILE_PREFIX_UNSUPPORTED_VERSION,
    SERIALIZED_FILE_PREFIX_UNSUPPORTED_ENDIAN,
    SERIALIZED_FILE_PREFIX_MALFORMED_LAYOUT,
    SERIALIZED_FILE_PREFIX_INCOMPLETE_MAPPING,
    SERIALIZED_FILE_PREFIX_MALFORMED_METADATA,
    SERIALIZED_FILE_PREFIX_VERSION_LIMIT,
    SERIALIZED_FILE_PREFIX_WORK_LIMIT,
} SerializedFilePrefixStatus;

/* A borrowed, actually mapped span. Offset is relative to the file slice. */
typedef struct {
    const uint8_t* data;
    uint64_t offset;
    size_t size;
} SerializedFilePrefixSpan;

/* Coordinates only: these bytes need not be mapped or inspected. */
typedef struct {
    uint64_t offset;
    uint64_t size;
} SerializedFilePrefixRange;

typedef struct {
    SerializedFilePrefixSpan header_source;
    SerializedFilePrefixSpan legacy_metadata_size_source;
    SerializedFilePrefixSpan legacy_file_size_source;
    SerializedFilePrefixSpan format_version_source;
    SerializedFilePrefixSpan legacy_data_offset_source;
    SerializedFilePrefixSpan metadata_size_source;
    SerializedFilePrefixSpan file_size_source;
    SerializedFilePrefixSpan data_offset_source;
    SerializedFilePrefixSpan endian_selector_source;
    SerializedFilePrefixSpan opaque_source;
    uint32_t format_version;
    uint64_t metadata_size;
    uint64_t file_size;
    uint64_t data_offset;
    uint8_t endian_selector;
    SerializedFilePrefixRange metadata;
    SerializedFilePrefixRange metadata_to_data_gap;
    SerializedFilePrefixRange data;
} SerializedFileHeaderView;

typedef struct {
    SerializedFileHeaderView header;
    SerializedFilePrefixSpan metadata_prefix_source;
    /* Arbitrary version bytes, excluding the separately retained NUL. Empty
     * strings retain the mapped address of their terminator with size zero. */
    SerializedFilePrefixSpan version_source;
    SerializedFilePrefixSpan version_terminator_source;
    SerializedFilePrefixSpan target_platform_source;
    SerializedFilePrefixSpan type_tree_source;
    uint32_t target_platform;
    uint8_t type_tree_enabled_raw;
    SerializedFilePrefixRange remaining_metadata;
} SerializedFilePrefixView;

typedef struct {
    /* Maximum visited version bytes, including the required NUL. Zero permits
     * no version read; neither zero limit means unlimited. */
    size_t max_version_bytes;
    uint64_t max_work;
} SerializedFilePrefixLimits;

typedef struct {
    SerializedFilePrefixStatus status;
    uint64_t work_used;
    /* First unavailable byte (metadata/mapping end), invalid field start, or
     * NO_OFFSET when a failure is not tied to input bytes. Successful queries
     * use NO_OFFSET. Work/scan limits identify the next field or version byte. */
    uint64_t error_offset;
} SerializedFilePrefixResult;

/*
 * Allocation-free v22 physical header query. mapped_prefix begins at the
 * standalone file or bundle-member slice origin; logical_size is a complete
 * slice-length premise authenticated by the caller, not by this query.
 * mapped_size must not exceed logical_size. NULL bytes are permitted only
 * with mapped_size zero. out_header is required and must not overlap mapped
 * bytes. All nonempty input spans must remain readable throughout the call.
 *
 * Require format 22, selector 0 or 1, declared size equal to logical_size, and
 * checked 48 + metadata_size <= data_offset <= file_size. Only the 48-byte
 * header must be mapped; zero metadata and header-only mappings are permitted.
 * Legacy and opaque bytes are retained without zero-value requirements.
 *
 * Failure precedence: arguments, logical header extent, mapped header extent,
 * header work, format, selector, declared length, data extent, metadata extent,
 * publication work. Charge 48 units atomically before any header byte is read
 * and one unit before publication. Success therefore costs exactly 49 units.
 * An unsuccessful charge consumes no units. Zero max_work is a real limit.
 *
 * Every failure leaves out_header unchanged. Success borrows mapped_prefix;
 * it neither owns nor authenticates those bytes and requires their unchanged
 * backing to outlive the view. No disposal is needed. Coordinates do not imply
 * mapped metadata, valid object tables, schemas or complete file admission.
 */
SerializedFilePrefixResult serialized_file_header_query(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    uint64_t max_work,
    SerializedFileHeaderView* out_header);

/*
 * Extends the same physical header query through version NUL, target scalar
 * and raw TypeTree-presence byte. limits and out_prefix are required; output
 * must not overlap either limits or mapped bytes. Header/prefix ownership and
 * transactional guarantees above apply. Selector 0 decodes the target as
 * little-endian; selector 1 as big-endian. Preserve arbitrary version bytes,
 * all target bits and raw tree values, including noncanonical values.
 *
 * Header work is 48 units with no intermediate publication. Charge one unit
 * per visited version byte (including NUL), four before target decoding, one
 * before reading the tree byte, and one before final publication. For each
 * prefix field, declared metadata extent is checked before mapping, then scan
 * limit (version only), then work. A missing NUL at metadata end is malformed;
 * a missing mapped byte within metadata is incomplete mapping. max_version_bytes
 * includes NUL. Neither limit is a promise of complete metadata inspection.
 *
 * This view supplies no target enum, engine-version compatibility, canonical
 * tree-flag domain, object schema, platform/backend/build-flavor admission or
 * exactness certificate. Remaining metadata is an uninspected coordinate range.
 */
SerializedFilePrefixResult serialized_file_prefix_query(const uint8_t* mapped_prefix,
    size_t mapped_size,
    uint64_t logical_size,
    const SerializedFilePrefixLimits* limits,
    SerializedFilePrefixView* out_prefix);

#ifdef __cplusplus
}
#endif

#endif /* SERIALIZED_FILE_PREFIX_H */
