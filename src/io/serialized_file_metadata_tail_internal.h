// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_METADATA_TAIL_INTERNAL_H
#define SERIALIZED_FILE_METADATA_TAIL_INTERNAL_H

#include "io/serialized_file_metadata_tail.h"

/* Exact retained allocation for alias admission; requires a genuine handle
 * and non-NULL disjoint scalar outputs. NULL/empty owner leaves outputs alone.
 * A successful borrowed range is valid only until tail disposal. */
bool serialized_file_metadata_tail_storage_range_internal(
    const SerializedFileMetadataTail* tail, const void** out_data, size_t* out_size);

#endif
