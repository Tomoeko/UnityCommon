// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_EXECUTABLE_RESOURCE_H
#define COMMON_EXECUTABLE_RESOURCE_H

/*
 * Finds a regular file below the package's share directory. The returned
 * path is owned by the caller and must be freed with free().
 *
 * share_relative_path is relative to "share" and must not contain an empty,
 * ".", or ".." component. Discovery is anchored to the running executable,
 * not the process working directory. Both layouts used by this project are
 * supported:
 *
 *   <build>/tool + <build>/share/...
 *   <prefix>/bin/tool + <prefix>/share/...
 *
 * Symbolic links/reparse points and non-regular resources fail closed.
 */
char* common_executable_resource_find(
    const char* executable, const char* share_relative_path);

#endif /* COMMON_EXECUTABLE_RESOURCE_H */
