// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_PPTR_RESOLVER_H
#define UNITY_PPTR_RESOLVER_H

#include "io/serialized_file.h"

/*
 * Deterministic resolver for player-build PPtr references.
 *
 * The resolver never searches by object or asset name.  Its graph is the
 * caller's already-discovered, bounded input set.  Source locators and scope
 * roots are borrowed UTF-8 strings; all other pointers are borrowed from the
 * corresponding SerializedFile.  A graph and its backing metadata must stay
 * alive for the duration of a call and while its result is inspected.
 */

typedef enum {
    UNITY_PPTR_RESOLVE_LOCAL = 0,
    UNITY_PPTR_RESOLVE_EXTERNAL_EXACT,
    UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT,
    UNITY_PPTR_RESOLVE_NULL,
    UNITY_PPTR_RESOLVE_OUT_OF_SCOPE,
    UNITY_PPTR_RESOLVE_AMBIGUOUS,
    UNITY_PPTR_RESOLVE_INVALID_FILE_ID,
    UNITY_PPTR_RESOLVE_TARGET_MISSING,
    UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS,
} UnityPPtrResolveStatus;

typedef struct {
    const SerializedFile* file;

    /* Exact pathname emitted by discovery for a loose SerializedFile, or the
     * containing UnityFS pathname for a member. */
    const char* outer_path;

    /* Exact UnityFS directory-table name. Required for bundle members and
     * ignored for loose SerializedFiles. */
    const char* member_name;
    size_t member_index;
    bool is_bundle_member;

    /* Lexical discovery boundary. Resolution never walks above this root and
     * never crosses to a node with a different normalized root.  Supplying
     * the same broad root is safe: nearest-ancestor resolution keeps sibling
     * player Data directories separate, and an equal-depth duplicate is
     * AMBIGUOUS rather than selected by basename. */
    const char* scope_root;

    /* Optional intrinsic asset GUID authority. Player SerializedFiles do not
     * generally carry their own GUID, so this must remain false unless the
     * caller obtained the GUID from an independent exact source. Non-zero
     * external GUIDs cannot resolve to an unqualified ordinary node. */
    bool has_asset_guid;
    uint8_t asset_guid[16];
} UnityPPtrSourceNode;

typedef struct {
    const UnityPPtrSourceNode* nodes;
    size_t node_count;
} UnityPPtrResolverGraph;

/* No Unity class uses INT32_MIN. This explicit sentinel avoids overloading
 * zero (Object) or negative script/type IDs. */
#define UNITY_PPTR_TARGET_CLASS_ANY INT32_MIN

typedef struct {
    UnityPPtrResolveStatus status;
    size_t source_index;
    size_t target_index;
    size_t external_index;
    const AssetFileExternal* external;
    const AssetObjectInfo* object;
} UnityPPtrResolveResult;

void unity_pptr_resolve_result_init(UnityPPtrResolveResult* result);

/*
 * Resolves one PPtr. The call returns false only for an invalid API argument
 * or structurally invalid source node; all ordinary evidence failures are a
 * true return with one of the fail-closed statuses above. The result is reset
 * before argument validation when result is non-NULL.
 *
 * FileID is interpreted exactly as Unity serializes it: zero is local and a
 * positive value is a one-based index into SerializedFile.externals. PathID
 * zero is NULL and is handled before FileID validation.
 */
bool unity_pptr_resolve(const UnityPPtrResolverGraph* graph,
                        size_t source_index, const AssetPPtr* pointer,
                        int32_t expected_target_class_id,
                        UnityPPtrResolveResult* result);

bool unity_pptr_resolve_status_is_success(UnityPPtrResolveStatus status);
const char* unity_pptr_resolve_status_name(UnityPPtrResolveStatus status);

#endif /* UNITY_PPTR_RESOLVER_H */
