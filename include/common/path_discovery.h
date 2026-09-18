// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_PATH_DISCOVERY_H
#define COMMON_PATH_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Portable, content-agnostic discovery of regular files. Directory traversal
 * never follows symbolic links or Windows reparse points. An explicit link is
 * rejected; links encountered below a directory input are ignored unless the
 * caller enables strict descendant handling.
 */

typedef enum {
    COMMON_PATH_DISCOVERY_OK = 0,
    COMMON_PATH_DISCOVERY_INVALID_ARGUMENT,
    COMMON_PATH_DISCOVERY_NOT_FOUND,
    COMMON_PATH_DISCOVERY_LINK_REJECTED,
    COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE,
    COMMON_PATH_DISCOVERY_ALLOCATION_FAILED,
    COMMON_PATH_DISCOVERY_IO_ERROR,
    /* Appended compatibility values: do not reorder the statuses above. */
    COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED,
    COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED,
    COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED,
    COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED,
} CommonPathDiscoveryStatus;

typedef struct {
    /* Owned NUL-terminated path, preserving the spelling produced from input. */
    char* path;

    /* True when this exact output path was also supplied as a regular-file
     * input. If a path is both discovered below a directory and supplied
     * explicitly, the deduplicated result retains true. */
    bool explicit_file;
} CommonDiscoveredPath;

typedef struct {
    CommonDiscoveredPath* paths;
    size_t count;
} CommonPathDiscoveryResult;

typedef struct {
    /* The default is true. False scans only direct children of each directory
     * input. Explicit directory inputs are always scanned once. */
    bool recursive;

    /* Both defaults are false, preserving the original behavior. Explicit
     * links and unsupported nodes remain errors regardless of these fields. */
    bool reject_descendant_links;
    bool reject_descendant_unsupported_nodes;

    /* Zero means unlimited for every limit, including in a zero-initialized
     * options structure. File and directory limits count unique exact path
     * spellings retained or queued. Each explicit directory has depth zero;
     * max_depth limits descendant directory nesting and has no effect when
     * recursive is false. max_path_bytes counts path bytes excluding NUL and
     * applies to explicit inputs and every discovered child path. */
    size_t max_files;
    size_t max_directories;
    size_t max_depth;
    size_t max_path_bytes;
} CommonPathDiscoveryOptions;

void common_path_discovery_options_default(CommonPathDiscoveryOptions* options);
void common_path_discovery_result_init(CommonPathDiscoveryResult* result);
void common_path_discovery_result_dispose(CommonPathDiscoveryResult* result);

/* Linearly merges two results that are already in the sorted, unique form
 * produced by common_path_discover or this function. Exact duplicate paths
 * retain the logical OR of their explicit_file flags. The operation takes
 * O(destination->count + addition->count) bytewise comparisons, leaves
 * addition unchanged, and has a strong destination guarantee on failure. */
CommonPathDiscoveryStatus common_path_discovery_result_merge(
    CommonPathDiscoveryResult* destination,
    const CommonPathDiscoveryResult* addition);

/*
 * Discovers regular files from one or more explicit file/directory inputs.
 * Results are sorted by bytewise strcmp order and deduplicated by exact output
 * path spelling. No path is canonicalized and no file content is inspected.
 *
 * options may be NULL to use the recursive default. result must have been
 * initialized. The operation has a strong output guarantee: failure leaves an
 * existing result unchanged, while success replaces it.
 */
CommonPathDiscoveryStatus common_path_discover(
    const char* const* inputs, size_t input_count,
    const CommonPathDiscoveryOptions* options,
    CommonPathDiscoveryResult* result);

const char* common_path_discovery_status_name(
    CommonPathDiscoveryStatus status);

#endif /* COMMON_PATH_DISCOVERY_H */
