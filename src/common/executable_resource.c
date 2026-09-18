// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/executable_resource.h"

#include "common/file_io.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

static bool is_separator(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    return value == '/';
#endif
}

static char native_separator(void) {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

static bool relative_share_path_is_safe(const char* path) {
    if (!path || !path[0] || is_separator(path[0])) return false;
#ifdef _WIN32
    if (path[1] == ':') return false;
#endif
    const char* component = path;
    for (const char* cursor = path;; ++cursor) {
        if (*cursor != '\0' && !is_separator(*cursor)) continue;
        size_t size = (size_t)(cursor - component);
        if (size == 0U || (size == 1U && component[0] == '.') ||
            (size == 2U && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (*cursor == '\0') return true;
        component = cursor + 1;
    }
}

static char* join_path(const char* left, const char* right) {
    if (!left || !right) return NULL;
    size_t left_size = strlen(left);
    size_t right_size = strlen(right);
    bool separator = left_size != 0U && !is_separator(left[left_size - 1U]);
    size_t extra = separator ? 1U : 0U;
    if (left_size > SIZE_MAX - extra ||
        left_size + extra > SIZE_MAX - right_size ||
        left_size + extra + right_size == SIZE_MAX) {
        return NULL;
    }
    size_t total = left_size + extra + right_size;
    char* result = (char*)malloc(total + 1U);
    if (!result) return NULL;
    memcpy(result, left, left_size);
    if (separator) result[left_size] = native_separator();
    memcpy(result + left_size + extra, right, right_size + 1U);
    return result;
}

static char* directory_from_path(const char* path) {
    if (!path || !path[0]) return NULL;
    const char* last = NULL;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (is_separator(*cursor)) last = cursor;
    }
    if (!last) return NULL;
    size_t size = (size_t)(last - path);
    if (size == 0U) size = 1U;
    char* result = (char*)malloc(size + 1U);
    if (!result) return NULL;
    memcpy(result, path, size);
    result[size] = '\0';
    return result;
}

static char* running_executable_path(void) {
#ifdef _WIN32
    DWORD capacity = 512U;
    while (capacity <= 32768U) {
        wchar_t* wide = (wchar_t*)malloc(
            (size_t)capacity * sizeof(*wide));
        if (!wide) return NULL;
        SetLastError(ERROR_SUCCESS);
        DWORD size = GetModuleFileNameW(NULL, wide, capacity);
        if (size != 0U && size < capacity) {
            wide[size] = L'\0';
            char* result = common_windows_wide_to_utf8(wide);
            free(wide);
            return result;
        }
        DWORD error = GetLastError();
        free(wide);
        if (size == 0U || (size < capacity &&
                           error != ERROR_INSUFFICIENT_BUFFER) ||
            capacity > 16384U) {
            return NULL;
        }
        capacity *= 2U;
    }
    return NULL;
#elif defined(__APPLE__)
    uint32_t capacity = 1024U;
    while (capacity <= 1024U * 1024U) {
        char* path = (char*)malloc((size_t)capacity);
        if (!path) return NULL;
        uint32_t required = capacity;
        if (_NSGetExecutablePath(path, &required) == 0) {
            char* canonical = realpath(path, NULL);
            if (canonical) {
                free(path);
                return canonical;
            }
            return path;
        }
        free(path);
        if (required <= capacity) return NULL;
        capacity = required;
    }
    return NULL;
#elif defined(__linux__)
    size_t capacity = 512U;
    while (capacity <= 1024U * 1024U) {
        char* path = (char*)malloc(capacity + 1U);
        if (!path) return NULL;
        ssize_t size = readlink("/proc/self/exe", path, capacity);
        if (size >= 0 && (size_t)size < capacity) {
            path[(size_t)size] = '\0';
            return path;
        }
        free(path);
        if (size < 0 || capacity > (1024U * 1024U) / 2U) return NULL;
        capacity *= 2U;
    }
    return NULL;
#else
    return NULL;
#endif
}

static char* executable_directory(const char* executable) {
    char* running = running_executable_path();
    if (running) {
        char* directory = directory_from_path(running);
        free(running);
        if (directory) return directory;
    }
    return directory_from_path(executable);
}

static bool regular_file_exists(const char* path) {
    CommonFileView view = {0};
    if (common_file_view_open_regular(path, SIZE_MAX, &view) !=
        COMMON_FILE_OK) {
        return false;
    }
    return common_file_view_close(&view) == COMMON_FILE_OK;
}

char* common_executable_resource_find(
    const char* executable, const char* share_relative_path) {
    if (!relative_share_path_is_safe(share_relative_path)) return NULL;
    char* directory = executable_directory(executable);
    if (!directory) return NULL;

    const char* share_roots[] = {"share", "../share"};
    char* result = NULL;
    for (size_t index = 0U;
         index < sizeof(share_roots) / sizeof(share_roots[0]); ++index) {
        char* share = join_path(directory, share_roots[index]);
        if (!share) break;
        char* candidate = join_path(share, share_relative_path);
        free(share);
        if (!candidate) break;
        if (regular_file_exists(candidate)) {
            result = candidate;
            break;
        }
        free(candidate);
    }
    free(directory);
    return result;
}
