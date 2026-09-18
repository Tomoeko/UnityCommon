#include "test_support/file_mutation.h"

#include "common/file_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#endif

#ifdef _WIN32
static bool windows_replace_file_posix(
    const wchar_t* sibling, const wchar_t* destination) {
    DWORD required = GetFullPathNameW(destination, 0U, NULL, NULL);
    if (required == 0U) return false;
    wchar_t* absolute = (wchar_t*)malloc(
        (size_t)required * sizeof(*absolute));
    if (!absolute ||
        GetFullPathNameW(destination, required, absolute, NULL) == 0U) {
        free(absolute);
        return false;
    }
    HANDLE source = CreateFileW(
        sibling, DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (source == INVALID_HANDLE_VALUE) {
        free(absolute);
        return false;
    }
    size_t name_size = wcslen(absolute) * sizeof(*absolute);
    size_t allocation_size = offsetof(FILE_RENAME_INFO, FileName) + name_size;
    bool renamed = false;
    if (allocation_size <= UINT32_MAX) {
        FILE_RENAME_INFO* information =
            (FILE_RENAME_INFO*)calloc(1U, allocation_size);
        if (information) {
            information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                                 FILE_RENAME_FLAG_POSIX_SEMANTICS;
            information->FileNameLength = (DWORD)name_size;
            memcpy(information->FileName, absolute, name_size);
            /* FILE_INFO_BY_HANDLE_CLASS value 22 is FileRenameInfoEx.
             * Current MinGW headers expose FILE_RENAME_INFO::Flags but omit
             * the matching enumerator, so keep this test portable by using
             * the documented ABI value explicitly. */
            renamed = SetFileInformationByHandle(
                source, (FILE_INFO_BY_HANDLE_CLASS)22, information,
                (DWORD)allocation_size) != 0;
            free(information);
        }
    }
    if (!CloseHandle(source)) renamed = false;
    free(absolute);
    return renamed;
}
#endif

bool test_replace_regular_file(
    const char* path, const void* data, size_t size) {
    if (!path || !path[0] || (!data && size != 0U)) return false;
#ifdef _WIN32
    size_t path_size = strlen(path);
    if (path_size > SIZE_MAX - 96U) return false;
    char* sibling = (char*)malloc(path_size + 96U);
    if (!sibling) return false;
    bool published = false;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(
            sibling, path_size + 96U, "%s.replace.%lu.%lu.%llu.%u", path,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)GetTickCount64(), attempt);
        if (written < 0 || (size_t)written >= path_size + 96U) break;
        CommonFileStatus status = common_file_write_new_atomic(
            sibling, data, size);
        if (status == COMMON_FILE_ALREADY_EXISTS) continue;
        if (status != COMMON_FILE_OK) break;

        wchar_t* wide_sibling = common_windows_utf8_to_wide(sibling);
        wchar_t* wide_path = common_windows_utf8_to_wide(path);
        if (wide_sibling && wide_path) {
            published = MoveFileExW(
                wide_sibling, wide_path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (!published) {
                published = ReplaceFileW(
                    wide_path, wide_sibling, NULL, REPLACEFILE_WRITE_THROUGH,
                    NULL, NULL) != 0;
            }
            if (!published) {
                published = windows_replace_file_posix(
                    wide_sibling, wide_path);
            }
            if (!published) {
                fprintf(stderr, "Windows pathname replacement failed: %lu\n",
                        (unsigned long)GetLastError());
            }
        } else {
            fprintf(stderr, "UTF-8 test replacement path conversion failed\n");
        }
        if (!published && wide_sibling) (void)DeleteFileW(wide_sibling);
        free(wide_path);
        free(wide_sibling);
        break;
    }
    free(sibling);
    return published;
#else
    return remove(path) == 0 &&
        common_file_write_new_atomic(path, data, size) == COMMON_FILE_OK;
#endif
}
