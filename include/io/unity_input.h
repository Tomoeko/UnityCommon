// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_INPUT_H
#define UNITY_INPUT_H

#include "common/common.h"
#include "common/sha256.h"

typedef enum {
    UNITY_INPUT_KIND_UNRELATED = 0,
    UNITY_INPUT_KIND_SERIALIZED_FILE_V22,
    UNITY_INPUT_KIND_UNITYFS,
    UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE,
    UNITY_INPUT_KIND_UNSUPPORTED_UNITY_ARCHIVE,
} UnityInputKind;

typedef struct {
    UnityInputKind kind;
    uint32_t serialized_file_version;
    uint64_t file_size;
} UnityInputProbe;

typedef enum {
    UNITY_INPUT_OK = 0,
    UNITY_INPUT_INVALID_ARGUMENT,
    UNITY_INPUT_FILE_ERROR,
    UNITY_INPUT_UNRELATED,
    UNITY_INPUT_UNSUPPORTED,
    UNITY_INPUT_CONTAINER_INVALID,
    UNITY_INPUT_MEMBER_INVALID,
    UNITY_INPUT_VISITOR_FAILED,
} UnityInputStatus;

typedef struct {
    const char* outer_path;
    /* NULL for a standalone SerializedFile; borrowed bundle member name
     * otherwise. */
    const char* member_name;
    /* Exact archive directory ordinal. Zero for a standalone source. */
    size_t member_index;
    bool is_bundle_member;
    const uint8_t* data;
    size_t size;
} UnitySerializedSource;

typedef bool (*UnitySerializedSourceVisitor)(
    const UnitySerializedSource* source, void* context);

typedef struct {
    size_t serialized_files;
    size_t resource_members;
    size_t directory_members;
    size_t deleted_members;
} UnityInputVisitStats;

/*
 * Opaque captured-file authority retained across multiple visits. The input
 * path is fixed to a stable absolute spelling at open. An identity anchor and
 * initial content digest remain owned until close; read-only bytes may be
 * unmapped between visits and are remapped only when their digest matches the
 * initial captured image. Each validation samples the held source bytes and
 * current pathname, so a persistent mutation, unlink, or same-content pathname
 * replacement fails closed.
 */
typedef struct {
    void* implementation;
} UnityInputSnapshot;

/* Header-only structural classification. */
UnityInputStatus unity_input_probe_bytes(const uint8_t* prefix,
                                         size_t prefix_size,
                                         uint64_t file_size,
                                         UnityInputProbe* out_probe);
UnityInputStatus unity_input_probe_path(const char* path,
                                        UnityInputProbe* out_probe);

void unity_input_snapshot_init(UnityInputSnapshot* snapshot);
UnityInputStatus unity_input_snapshot_open(
    const char* path, UnityInputSnapshot* snapshot);
bool unity_input_snapshot_is_open(const UnityInputSnapshot* snapshot);
const char* unity_input_snapshot_path(const UnityInputSnapshot* snapshot);
/* SHA-256 of the complete captured outer file (including a bundle's storage
 * bytes), after revalidating its held file identity, content and pathname.
 * Works while the mapping is suspended; never substitutes a pathname reopen.
 * Failure leaves digest unchanged. This does not validate member schemas. */
UnityInputStatus unity_input_snapshot_digest(
    const UnityInputSnapshot* snapshot,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);
UnityInputStatus unity_input_snapshot_visit(
    UnityInputSnapshot* snapshot, UnitySerializedSourceVisitor visitor,
    void* context, UnityInputVisitStats* out_stats);
/* Releases the large mapping while retaining an open identity anchor and the
 * initial digest. A later visit remaps only the same anchored file object and
 * requires exact byte equality with the initial capture before use. UnityFS
 * decoded member storage remains owned by the snapshot. */
UnityInputStatus unity_input_snapshot_suspend_mapping(
    UnityInputSnapshot* snapshot);
/* Always releases the snapshot. A non-OK result means the held object, sampled
 * bytes, or pathname identity did not match the opening capture. */
UnityInputStatus unity_input_snapshot_close(UnityInputSnapshot* snapshot);

/*
 * One-shot wrapper around snapshot open, visit, and identity-checking close.
 * The borrowed source bytes remain valid only for the callback. UnityFS
 * resource streams are counted but never misclassified as SerializedFiles.
 */
UnityInputStatus unity_input_visit_serialized(
    const char* path, UnitySerializedSourceVisitor visitor, void* context,
    UnityInputVisitStats* out_stats);

const char* unity_input_kind_name(UnityInputKind kind);
const char* unity_input_status_name(UnityInputStatus status);

#endif /* UNITY_INPUT_H */
