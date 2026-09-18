// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_FILE_MANAGED_PROFILE_INTERNAL_H
#define SERIALIZED_FILE_MANAGED_PROFILE_INTERNAL_H

#include "io/serialized_file_managed_values.h"

typedef enum ManagedProfileKind {
    MANAGED_PROFILE_HOST = 1,
    MANAGED_PROFILE_TEXT,
    MANAGED_PROFILE_FLOAT
} ManagedProfileKind;

typedef struct ManagedProfile {
    ManagedProfileKind kind;
    size_t registry_ordinal;
} ManagedProfile;

typedef struct ManagedProfileResult {
    SerializedFileManagedValuesStatus status;
    SerializedFileManagedValuesField field;
    size_t type_ordinal;
    size_t node_ordinal;
    uint64_t error_offset;
    uint64_t work_used;
} ManagedProfileResult;

/* Private qualification over an internally constructed, genuine immutable
 * schema. Required pointers and output must be disjoint from schema storage and
 * backing; the caller owns their admission. No allocation or byte copy occurs.
 * Output is unchanged on failure and borrows a static profile on success.
 *
 * Constant-time engine/row/count admission precedes work. Each original node
 * costs one unit, each name-length admission one (including authored names),
 * then one per fixed-name byte compared, stopping at the first mismatch. A
 * failed charge consumes no work. LIMIT_EXCEEDED means only work exhaustion;
 * diagnostics identify original file-relative node or name-offset words.
 * Matching the complete level sequence fixes all hierarchy links under the
 * genuine Schema contract. This does not authenticate a selected reference
 * identity or replace the caller's genuine context query and lineage checks.
 */
ManagedProfileResult serialized_file_managed_profile_qualify(const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind context_kind,
    uint64_t max_work,
    const ManagedProfile** out_profile);

#endif
