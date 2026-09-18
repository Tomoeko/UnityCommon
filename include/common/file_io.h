// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_FILE_IO_H
#define COMMON_FILE_IO_H

#include "common/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMMON_FILE_OK = 0,
    COMMON_FILE_INVALID_ARGUMENT,
    COMMON_FILE_NOT_FOUND,
    COMMON_FILE_NOT_REGULAR,
    COMMON_FILE_TOO_LARGE,
    COMMON_FILE_ALLOCATION_FAILED,
    COMMON_FILE_ALREADY_EXISTS,
    COMMON_FILE_IO_ERROR,
} CommonFileStatus;

typedef struct {
    uint8_t* data;
    size_t size;
} CommonFileBytes;

typedef struct {
    /* Read-only mapped bytes. NULL is the canonical empty-file view. */
    const uint8_t* data;
    size_t size;
    /* Opaque owned mapping/handle state; callers must not inspect it. */
    void* implementation;
    /* Digest of the immutable captured image, verified before exposure. */
    uint8_t captured_sha256[COMMON_SHA256_DIGEST_SIZE];
    bool captured_sha256_recorded;
} CommonFileView;

/*
 * Opens one regular file without copying its contents into the C heap. A
 * nonempty POSIX view uses an anonymous, read-only process-private mapping;
 * Windows maps a delete-on-close process-exclusive snapshot. Later source
 * truncation therefore cannot invalidate readable view bytes. The exact opened
 * source object and its opening namespace remain held until close. The
 * captured bytes are SHA-256 checked against a second held-source read before
 * exposure. Symbolic links/reparse points, non-regular objects, oversize
 * files, and observed metadata/content races fail closed. Failure clears
 * out_view.
 */
CommonFileStatus common_file_view_open_regular(
    const char* path, size_t max_size, CommonFileView* out_view);

/* Copies the already-verified captured-image digest without re-hashing. */
bool common_file_view_sha256(
    const CommonFileView* view,
    uint8_t out_digest[COMMON_SHA256_DIGEST_SIZE]);

/*
 * Re-hashes the held source against the verified digest of the immutable
 * captured image, then validates identity, size, mode/attributes,
 * write/change timestamps, and the opening pathname before unmapping and
 * closing. Cleanup always occurs and view is cleared, including on failure.
 * These are point-in-time comparisons: normal
 * user-space I/O cannot prove that a hostile shared-map writer did not perform
 * an A-to-B-to-A mutation entirely between validation samples.
 */
CommonFileStatus common_file_view_close(CommonFileView* view);

/*
 * Reads at most capacity prefix bytes from one stable regular file without
 * allocating the complete file.  This is intended for structural magic/header
 * discovery in large directory trees.  Symbolic links/reparse points are
 * rejected using the same rules as common_file_read_regular().
 */
CommonFileStatus common_file_read_prefix_regular(
    const char* path, void* buffer, size_t capacity,
    size_t* out_prefix_size, uint64_t* out_file_size);

/*
 * Reads one sampled regular-file image. Symbolic links/reparse points and
 * non-regular objects are rejected. File identity and metadata are checked
 * again after the final byte; callers that retain publication authority over
 * time must use CommonFileView and its close-time content validation.
 * max_size is an explicit allocation bound; SIZE_MAX means unbounded by the
 * caller. Empty files are represented by {NULL, 0}.
 */
CommonFileStatus common_file_read_regular(
    const char* path, size_t max_size, CommonFileBytes* out_file);

/* Same stable read, with one extra trailing NUL byte outside out_file->size. */
CommonFileStatus common_file_read_regular_terminated(
    const char* path, size_t max_size, CommonFileBytes* out_file);

void common_file_bytes_dispose(CommonFileBytes* file);

/*
 * Publishes a new regular file through a platform atomic no-replace primitive
 * after requesting the applicable file and namespace flushes.
 * The destination is never overwritten; a concurrent publisher wins or loses
 * atomically. POSIX also requests a sync of the held parent directory after
 * publication. These flushes do not promise survival across power loss beyond
 * the guarantees of the host OS, filesystem, and storage device. The opening
 * destination parent is held through publication, temporary cleanup is
 * confined to that held directory/object, and success is reported only after
 * a point-in-time parent/destination identity recheck. Replacing the external
 * parent pathname during the operation therefore fails closed and cannot
 * redirect cleanup into the replacement directory. POSIX destination parents
 * are expected to be trusted against hostile same-principal name swapping
 * inside the already-held directory during failure cleanup. Linux requires a
 * destination filesystem with O_TMPFILE plus either usable AT_EMPTY_PATH or
 * /proc/self/fd; unsupported environments fail with COMMON_FILE_IO_ERROR.
 * data may be NULL only for an empty file.
 */
CommonFileStatus common_file_write_new_atomic(
    const char* path, const void* data, size_t size);

const char* common_file_status_name(CommonFileStatus status);

#endif /* COMMON_FILE_IO_H */
