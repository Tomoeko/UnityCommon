// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_TYPE_SHAPE_INTERNAL_H
#define SERIALIZED_TYPE_SHAPE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* Ordinary/reference v22 prefixes under an established exact35/exact29 premise.
 * Signed16 script indexes use their sign bit; UINT16_MAX is not the only
 * negative representation. The class -1/negative-index combination remains
 * outside the common reader/writer grammar. */
static inline bool serialized_type_v22_hash_shape(
    uint32_t class_id_bits, uint16_t script_index_bits, bool* out_has_script_hash) {
    const bool negative_index = (script_index_bits & UINT16_C(0x8000)) != 0U;
    if (class_id_bits == UINT32_MAX && negative_index) {
        return false;
    }
    *out_has_script_hash = class_id_bits == 114U || !negative_index;
    return true;
}

#endif
