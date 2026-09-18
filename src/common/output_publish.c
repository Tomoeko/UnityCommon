// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/output_publish.h"

#include "common/file_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFDIR) == _S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static bool path_is_directory(const char* path) {
#ifdef _WIN32
    wchar_t* wide_path = path ? common_windows_utf8_to_wide(path) : NULL;
    if (!wide_path) return false;
    DWORD attributes = GetFileAttributesW(wide_path);
    free(wide_path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
#else
    struct stat status;
    return path && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
#endif
}

static bool make_one_directory(const char* path) {
    if (path_is_directory(path)) return true;
#ifdef _WIN32
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    int result = _wmkdir(wide_path);
    free(wide_path);
    if (result == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST && path_is_directory(path);
}

static bool is_separator(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    return value == '/';
#endif
}

bool common_output_ensure_directory_tree(const char* path) {
    if (!path || !path[0]) return false;
    size_t length = strlen(path);
    char* copy = (char*)malloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, path, length + 1U);
#ifdef _WIN32
    size_t root_length = 0U;
    if (!common_windows_directory_root_length(
            copy, length, &root_length)) {
        free(copy);
        return false;
    }
    if (root_length == length && root_length != 0U) {
        bool ok = path_is_directory(copy);
        free(copy);
        return ok;
    }
    size_t first_component = root_length > 1U ? root_length : 1U;
#else
    size_t first_component = 1U;
#endif
    for (size_t i = first_component; i < length; ++i) {
        if (!is_separator(copy[i])) continue;
        char saved = copy[i];
        copy[i] = '\0';
        bool ok = copy[0] == '\0' || make_one_directory(copy);
        copy[i] = saved;
        if (!ok) {
            free(copy);
            return false;
        }
    }
    bool ok = make_one_directory(copy);
    free(copy);
    return ok;
}

char* common_output_join_path(const char* directory, const char* name) {
    if (!directory || !name) return NULL;
    size_t directory_size = strlen(directory);
    size_t name_size = strlen(name);
    bool separator = directory_size != 0U &&
        !is_separator(directory[directory_size - 1U]);
    size_t extra = separator ? 1U : 0U;
    if (directory_size > SIZE_MAX - extra ||
        directory_size + extra > SIZE_MAX - name_size ||
        directory_size + extra + name_size == SIZE_MAX) {
        return NULL;
    }
    size_t total = directory_size + extra + name_size;
    char* result = (char*)malloc(total + 1U);
    if (!result) return NULL;
    memcpy(result, directory, directory_size);
    if (separator) {
#ifdef _WIN32
        result[directory_size] = '\\';
#else
        result[directory_size] = '/';
#endif
    }
    memcpy(result + directory_size + extra, name, name_size + 1U);
    return result;
}

CommonOutputPreflightStatus common_output_preflight_exact(
    const char* path, const void* data, size_t size) {
    if (!path || (!data && size != 0U)) {
        return COMMON_OUTPUT_PREFLIGHT_IO_ERROR;
    }
    CommonFileView existing = {0};
    CommonFileStatus status = common_file_view_open_regular(
        path, SIZE_MAX, &existing);
    if (status == COMMON_FILE_NOT_FOUND) {
        return COMMON_OUTPUT_PREFLIGHT_MISSING;
    }
    if (status != COMMON_FILE_OK) return COMMON_OUTPUT_PREFLIGHT_IO_ERROR;
    bool equal = existing.size == size &&
        (size == 0U || memcmp(existing.data, data, size) == 0);
    if (common_file_view_close(&existing) != COMMON_FILE_OK) {
        return COMMON_OUTPUT_PREFLIGHT_IO_ERROR;
    }
    return equal ? COMMON_OUTPUT_PREFLIGHT_UNCHANGED
                 : COMMON_OUTPUT_PREFLIGHT_COLLISION;
}

CommonOutputPublishStatus common_output_publish_exact(
    const char* path, const void* data, size_t size) {
    CommonFileStatus status = common_file_write_new_atomic(path, data, size);
    if (status == COMMON_FILE_OK) return COMMON_OUTPUT_PUBLISH_EMITTED;
    if (status != COMMON_FILE_ALREADY_EXISTS) {
        return COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }
    CommonFileView existing = {0};
    status = common_file_view_open_regular(path, SIZE_MAX, &existing);
    bool equal = status == COMMON_FILE_OK && existing.size == size &&
        (size == 0U || memcmp(existing.data, data, size) == 0);
    if (status == COMMON_FILE_OK &&
        common_file_view_close(&existing) != COMMON_FILE_OK) {
        return COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }
    if (equal) return COMMON_OUTPUT_PUBLISH_UNCHANGED;
    return status == COMMON_FILE_OK ? COMMON_OUTPUT_PUBLISH_COLLISION
                                    : COMMON_OUTPUT_PUBLISH_IO_ERROR;
}

bool common_output_publish_exact_residue_possible(
    CommonOutputPreflightStatus prior_preflight,
    CommonOutputPublishStatus publish_status,
    const char* path, const void* data, size_t size) {
    return prior_preflight == COMMON_OUTPUT_PREFLIGHT_MISSING &&
        publish_status == COMMON_OUTPUT_PUBLISH_IO_ERROR &&
        common_output_preflight_exact(path, data, size) ==
            COMMON_OUTPUT_PREFLIGHT_UNCHANGED;
}

const char* common_output_publish_status_name(
    CommonOutputPublishStatus status) {
    switch (status) {
        case COMMON_OUTPUT_PUBLISH_EMITTED: return "emitted";
        case COMMON_OUTPUT_PUBLISH_UNCHANGED: return "unchanged";
        case COMMON_OUTPUT_PUBLISH_COLLISION: return "collision";
        case COMMON_OUTPUT_PUBLISH_IO_ERROR: return "io-error";
        default: return "unknown";
    }
}

const char* common_output_preflight_status_name(
    CommonOutputPreflightStatus status) {
    switch (status) {
        case COMMON_OUTPUT_PREFLIGHT_MISSING: return "missing";
        case COMMON_OUTPUT_PREFLIGHT_UNCHANGED: return "unchanged";
        case COMMON_OUTPUT_PREFLIGHT_COLLISION: return "collision";
        case COMMON_OUTPUT_PREFLIGHT_IO_ERROR: return "io-error";
        default: return "unknown";
    }
}
