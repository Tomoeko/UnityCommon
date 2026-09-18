// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_OUTPUT_PUBLISH_H
#define COMMON_OUTPUT_PUBLISH_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMMON_OUTPUT_PUBLISH_EMITTED = 0,
    COMMON_OUTPUT_PUBLISH_UNCHANGED,
    COMMON_OUTPUT_PUBLISH_COLLISION,
    COMMON_OUTPUT_PUBLISH_IO_ERROR,
} CommonOutputPublishStatus;

typedef enum {
    COMMON_OUTPUT_PREFLIGHT_MISSING = 0,
    COMMON_OUTPUT_PREFLIGHT_UNCHANGED,
    COMMON_OUTPUT_PREFLIGHT_COLLISION,
    COMMON_OUTPUT_PREFLIGHT_IO_ERROR,
} CommonOutputPreflightStatus;

/* Creates every missing directory component. Existing directory aliases and
 * Windows junctions are accepted; file publication remains no-overwrite. */
bool common_output_ensure_directory_tree(const char* path);

/* Allocates a native-separator path. The caller owns the returned string. */
char* common_output_join_path(const char* directory, const char* name);

/* Read-only collision check for a related artifact set. Callers preflight
 * every member before publishing the first one, preventing a known later
 * collision from leaving a partial set on disk. Publication remains the
 * final race-safe authority. */
CommonOutputPreflightStatus common_output_preflight_exact(
    const char* path, const void* data, size_t size);

/* Atomically creates a regular file, reports byte-identical pre-existence as
 * unchanged, and rejects differing pre-existing bytes as a collision. */
CommonOutputPublishStatus common_output_publish_exact(
    const char* path, const void* data, size_t size);

/* Detects the fail-closed ambiguity where publication reported IO_ERROR after
 * a missing preflight but the complete exact destination is now present
 * (for example, parent-directory durability failed after create). */
bool common_output_publish_exact_residue_possible(
    CommonOutputPreflightStatus prior_preflight,
    CommonOutputPublishStatus publish_status,
    const char* path, const void* data, size_t size);

const char* common_output_publish_status_name(
    CommonOutputPublishStatus status);
const char* common_output_preflight_status_name(
    CommonOutputPreflightStatus status);

#endif /* COMMON_OUTPUT_PUBLISH_H */
