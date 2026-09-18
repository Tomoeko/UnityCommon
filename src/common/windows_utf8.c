// SPDX-License-Identifier: GPL-3.0-only

#include "common/windows_utf8.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static bool windows_is_separator(char value) {
    return value == '/' || value == '\\';
}

static bool ascii_equal_ignore_case(char left, char right) {
    if (left >= 'a' && left <= 'z') left = (char)(left - ('a' - 'A'));
    if (right >= 'a' && right <= 'z') right = (char)(right - ('a' - 'A'));
    return left == right;
}

static bool windows_unc_root_length(const char* path, size_t length,
                                    size_t start, size_t* out_length) {
    size_t position = start;
    for (unsigned component = 0U; component < 2U; ++component) {
        size_t component_start = position;
        while (position < length &&
               !windows_is_separator(path[position])) {
            ++position;
        }
        if (position == component_start) return false;
        if (component == 0U) {
            if (position == length) return false;
            while (position < length &&
                   windows_is_separator(path[position])) {
                ++position;
            }
        }
    }
    while (position < length && windows_is_separator(path[position])) {
        ++position;
    }
    *out_length = position;
    return true;
}

bool common_windows_directory_root_length(
    const char* path, size_t length, size_t* out_length) {
    if (!path || !out_length) return false;
    *out_length = 0U;
    if (length >= 2U && path[1] == ':') {
        *out_length = length >= 3U && windows_is_separator(path[2]) ?
            3U : 2U;
        return true;
    }
    if (length == 0U || !windows_is_separator(path[0])) return true;
    if (length == 1U || !windows_is_separator(path[1])) {
        *out_length = 1U;
        return true;
    }

    size_t start = 2U;
    if (length >= 4U && path[2] == '?' &&
        windows_is_separator(path[3])) {
        start = 4U;
        if (length >= 8U &&
            ascii_equal_ignore_case(path[4], 'U') &&
            ascii_equal_ignore_case(path[5], 'N') &&
            ascii_equal_ignore_case(path[6], 'C') &&
            windows_is_separator(path[7])) {
            return windows_unc_root_length(path, length, 8U, out_length);
        }
        if (length >= 7U && path[5] == ':' &&
            windows_is_separator(path[6])) {
            *out_length = 7U;
            return true;
        }
        return false;
    }
    return windows_unc_root_length(path, length, start, out_length);
}

wchar_t* common_windows_utf8_to_wide(const char* value) {
    if (!value) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (required <= 0) return NULL;
    if ((size_t)required > SIZE_MAX / sizeof(wchar_t)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    wchar_t* result = (wchar_t*)malloc(
        (size_t)required * sizeof(*result));
    if (!result) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
            result, required) != required) {
        DWORD error = GetLastError();
        free(result);
        SetLastError(error);
        return NULL;
    }
    return result;
}

char* common_windows_wide_to_utf8(const wchar_t* value) {
    if (!value) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (required <= 0) return NULL;
    char* result = (char*)malloc((size_t)required);
    if (!result) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
            result, required, NULL, NULL) != required) {
        DWORD error = GetLastError();
        free(result);
        SetLastError(error);
        return NULL;
    }
    return result;
}

int common_windows_run_utf8_main(
    int argc, wchar_t** wide_argv,
    CommonWindowsUtf8MainFunction implementation) {
    if (argc < 0 || (!wide_argv && argc != 0) || !implementation ||
        (size_t)argc == SIZE_MAX) {
        fputs("invalid Windows command line\n", stderr);
        return 1;
    }
    size_t count = (size_t)argc;
    if (count + 1U > SIZE_MAX / sizeof(char*)) {
        fputs("Windows command line is too large\n", stderr);
        return 1;
    }
    char** argv = (char**)calloc(count + 1U, sizeof(*argv));
    if (!argv) {
        fputs("could not allocate UTF-8 command line\n", stderr);
        return 1;
    }
    size_t converted = 0U;
    for (; converted < count; ++converted) {
        argv[converted] = common_windows_wide_to_utf8(
            wide_argv[converted]);
        if (!argv[converted]) break;
    }
    if (converted != count) {
        fputs("could not convert Windows command line to UTF-8\n", stderr);
        for (size_t index = 0U; index < converted; ++index) {
            free(argv[index]);
        }
        free(argv);
        return 1;
    }
    int result = implementation(argc, argv);
    for (size_t index = 0U; index < count; ++index) free(argv[index]);
    free(argv);
    return result;
}

#endif
