// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_SCHEMA_INTERNAL_H
#define SERIALIZED_FILE_SCHEMA_INTERNAL_H

#include "io/serialized_file_schema.h"

/* Exact retained allocation of a genuine schema, solely for range admission.
 * Require non-NULL, disjoint output scalars. NULL/empty owners return false
 * without changing outputs. The borrowed range lasts until schema disposal. */
bool serialized_file_schema_storage_range_internal(
    const SerializedFileSchema* schema, const void** out_data, size_t* out_size);

#endif
