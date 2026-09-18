#ifndef UNITY_COMMON_TEST_FILE_MUTATION_H
#define UNITY_COMMON_TEST_FILE_MUTATION_H

#include <stdbool.h>
#include <stddef.h>

/* Replaces an existing pathname while any delete-sharing identity anchor may
 * remain open. This models the same stale-path identity transition on POSIX
 * and Windows instead of relying on delete-pending Windows name semantics. */
bool test_replace_regular_file(
    const char* path, const void* data, size_t size);

#endif /* UNITY_COMMON_TEST_FILE_MUTATION_H */
