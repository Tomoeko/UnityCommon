// SPDX-License-Identifier: GPL-3.0-only
#ifndef TYPETREE_VALUE_DIGEST_H
#define TYPETREE_VALUE_DIGEST_H

#include "common/sha256.h"
#include "io/typetree.h"

/* Ordered decoded-value identity, paired with a separately validated schema.
 * Includes node names/types, integer signedness, decoded double bits, exact
 * strings, child counts and order. Maps remain ordered. Packed UInt8 arrays
 * and equivalent expanded UInt8 arrays share an encoding; byte-element labels
 * are schema authority and are not repeated. This is not a wire-file digest.
 *
 * Allocation-free and bounded to 256 nesting levels, 1,048,576 logical nodes
 * (including packed bytes), and 16 MiB per string. Input strings and child
 * spans must be valid C objects and remain immutable. Invalid models, cycles
 * exceeding the depth bound, or exhausted limits fail without changing output. */
bool typetree_value_digest(const TypeTreeValue *value, uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);

#endif
