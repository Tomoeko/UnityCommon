// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUNDLE_ARCHIVE_H
#define BUNDLE_ARCHIVE_H

#include "common/common.h"

typedef struct {
    uint32_t decompressed_size;
    uint32_t compressed_size;
    uint16_t flags;
} BundleBlockInfo;

typedef struct {
    uint64_t offset;
    uint64_t decompressed_size;
    uint32_t flags;
    char* name;
} BundleDirectoryInfo;

enum {
    UNITYFS_NODE_DIRECTORY = 1u,
    UNITYFS_NODE_DELETED = 2u,
    UNITYFS_NODE_SERIALIZED_FILE = 4u,
    UNITYFS_NODE_KNOWN_FLAGS = 7u,
};

typedef enum {
    BUNDLE_MEMBER_INVALID = 0,
    BUNDLE_MEMBER_SERIALIZED_FILE,
    BUNDLE_MEMBER_RESOURCE,
    BUNDLE_MEMBER_DIRECTORY,
    BUNDLE_MEMBER_DELETED,
} BundleMemberKind;

typedef struct {
    char* signature;
    uint32_t version;
    char* generation_version;
    char* engine_version;
    
    uint64_t total_file_size;
    uint32_t compressed_info_size;
    uint32_t decompressed_info_size;
    uint32_t flags;
    
    int block_count;
    BundleBlockInfo* blocks;
    
    int directory_count;
    BundleDirectoryInfo* directories;
    
    uint8_t* payload;         // Contiguous fully decompressed payload
    size_t payload_size;
} BundleArchive;

// Opens the pinned UnityFS v8 / Unity 2021.3.35f1 bundle dialect from memory.
// Other engine versions, historical signatures, and unsupported flag
// semantics are rejected explicitly; they do not share this verified layout.
// Returns true on success.
bool bundle_open(BundleArchive* archive, const uint8_t* file_data, size_t file_size);

// Closes and releases all resources associated with the bundle.
void bundle_close(BundleArchive* archive);

/* Retrieves a borrowed member view.  A valid empty member returns true with
 * *out_data == NULL and *out_size == 0; false always clears both outputs. */
bool bundle_get_member_view(const BundleArchive* archive, size_t member_index,
                            const uint8_t** out_data, size_t* out_size);

bool bundle_get_file_view(const BundleArchive* archive, const char* name,
                          const uint8_t** out_data, size_t* out_size);

/* Classifies UnityFS directory flags and external-resource filename forms.
 * Conflicting/unknown flags and unrecognized unflagged names remain invalid. */
BundleMemberKind bundle_member_classify(const BundleDirectoryInfo* member);

/* Compatibility pointer API.  Empty members and failures both return NULL;
 * use bundle_get_file_view() when that distinction matters.  out_size is
 * always cleared on failure or for an empty member. */
const uint8_t* bundle_get_file(BundleArchive* archive, const char* name, size_t* out_size);

#endif // BUNDLE_ARCHIVE_H
