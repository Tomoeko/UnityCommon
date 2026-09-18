// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_DIRECTORY_INTERNAL_H
#define SERIALIZED_FILE_DIRECTORY_INTERNAL_H

#include "io/serialized_file_directory.h"

/* Exposes the exact retained allocation only for disjoint-range admission.
 * Require genuine handles and non-NULL, disjoint output scalar pointers.
 * Returns false for NULL/empty owners, leaving outputs untouched. A successful
 * borrowed range is immutable and valid only until directory disposal. */
bool serialized_file_directory_storage_range_internal(
    const SerializedFileDirectory* directory, const void** out_data, size_t* out_size);

/* Shared owner-storage mechanics. Empty ranges never overlap. Array planning
 * requires nonzero alignment and disjoint, non-NULL scalar outputs; overflow
 * leaves both outputs unchanged. Neither helper dereferences source ranges. */
bool serialized_file_storage_overlaps_internal(
    const void* first, size_t first_size, const void* second, size_t second_size);
bool serialized_file_storage_append_array_internal(
    size_t* total, size_t alignment, size_t count, size_t width, size_t* out_offset);

#endif
