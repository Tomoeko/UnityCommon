// SPDX-License-Identifier: GPL-3.0-only

#ifndef TYPETREE_COMMON_STRINGS_INTERNAL_H
#define TYPETREE_COMMON_STRINGS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Borrow the complete immutable exact-2021.3.35f1 Unity common-string buffer.
 * Includes the final extra NUL. out_size is required; no allocation/disposal.
 * This source space has buffer-relative offsets, never serialized-file offsets.
 */
const uint8_t* typetree_common_string_table(size_t* out_size);

#endif
