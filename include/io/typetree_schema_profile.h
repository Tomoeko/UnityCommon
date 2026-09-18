// SPDX-License-Identifier: GPL-3.0-only

#ifndef TYPETREE_SCHEMA_PROFILE_H
#define TYPETREE_SCHEMA_PROFILE_H

#include "common/sha256.h"
#include "io/typetree.h"

/*
 * A known profile is selected by the complete external schema identity
 * (Unity version, class ID, and Unity type hash).  UNKNOWN means the caller
 * did not claim one of the profiles owned by this module.  INVALID means it
 * did claim one, but the resolved semantic TypeTree shape differs.
 */
typedef enum {
    TYPETREE_SCHEMA_PROFILE_UNKNOWN = 0,
    TYPETREE_SCHEMA_PROFILE_VALID,
    TYPETREE_SCHEMA_PROFILE_INVALID
} TypeTreeSchemaProfileResult;

/*
 * Computes a portable digest of the resolved schema shape.  Local string
 * table offsets are deliberately excluded; node order, hierarchy, wire
 * types, sizes, flags, names, dependencies, and reference-type identity are
 * included.
 */
bool typetree_schema_shape_digest(
    const TypeTreeType* schema,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);

/* Validates the pinned 2021.3.35f1 schemas and their explicitly reviewed
 * 2021.3.29f1 Material/Shader identity bindings. */
TypeTreeSchemaProfileResult typetree_schema_validate_known_profile(
    const char* unity_version, size_t unity_version_size,
    int32_t class_id, const uint8_t type_hash[16],
    const TypeTreeType* schema);

#endif /* TYPETREE_SCHEMA_PROFILE_H */
