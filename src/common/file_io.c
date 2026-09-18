// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/file_io.h"
#include "common/sha256.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif
#ifdef __APPLE__
#include <sys/stdio.h>
#endif
#include <unistd.h>
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

CommonFileStatus common_file_read_regular_terminated(
    const char* path, size_t max_size, CommonFileBytes* out_file) {
    CommonFileStatus status = common_file_read_regular(
        path, max_size, out_file);
    if (status != COMMON_FILE_OK) return status;
    if (out_file->size == SIZE_MAX) {
        common_file_bytes_dispose(out_file);
        return COMMON_FILE_TOO_LARGE;
    }
    uint8_t* terminated = (uint8_t*)realloc(
        out_file->data, out_file->size + 1U);
    if (!terminated) {
        common_file_bytes_dispose(out_file);
        return COMMON_FILE_ALLOCATION_FAILED;
    }
    terminated[out_file->size] = 0U;
    out_file->data = terminated;
    return COMMON_FILE_OK;
}

void common_file_bytes_dispose(CommonFileBytes* file) {
    if (!file) return;
    free(file->data);
    file->data = NULL;
    file->size = 0U;
}

const char* common_file_status_name(CommonFileStatus status) {
    switch (status) {
        case COMMON_FILE_OK: return "ok";
        case COMMON_FILE_INVALID_ARGUMENT: return "invalid-argument";
        case COMMON_FILE_NOT_FOUND: return "not-found";
        case COMMON_FILE_NOT_REGULAR: return "not-regular";
        case COMMON_FILE_TOO_LARGE: return "too-large";
        case COMMON_FILE_ALLOCATION_FAILED: return "allocation-failed";
        case COMMON_FILE_ALREADY_EXISTS: return "already-exists";
        case COMMON_FILE_IO_ERROR: return "io-error";
        default: return "unknown";
    }
}

bool common_file_view_sha256(
    const CommonFileView* view,
    uint8_t out_digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!view || !view->implementation || !view->captured_sha256_recorded ||
        !out_digest) {
        return false;
    }
    memcpy(out_digest, view->captured_sha256,
           sizeof(view->captured_sha256));
    return true;
}

#ifdef _WIN32

static CommonFileStatus windows_open_error(DWORD error) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return COMMON_FILE_NOT_FOUND;
    }
    return COMMON_FILE_IO_ERROR;
}

static CommonFileStatus windows_conversion_error(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) {
        return COMMON_FILE_ALLOCATION_FAILED;
    }
    if (error == ERROR_NO_UNICODE_TRANSLATION ||
        error == ERROR_INVALID_PARAMETER) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    return COMMON_FILE_IO_ERROR;
}

static wchar_t* windows_absolute_path(const wchar_t* path) {
    if (!path || !path[0]) return NULL;
    DWORD required = GetFullPathNameW(path, 0U, NULL, NULL);
    if (required == 0U) return NULL;
    wchar_t* absolute = (wchar_t*)malloc(
        (size_t)required * sizeof(*absolute));
    if (!absolute) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    DWORD written = GetFullPathNameW(path, required, absolute, NULL);
    if (written == 0U || written >= required) {
        free(absolute);
        return NULL;
    }
    return absolute;
}

static bool windows_regular_file_info(
    HANDLE handle, BY_HANDLE_FILE_INFORMATION* out_information) {
    BY_HANDLE_FILE_INFORMATION information;
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
          FILE_ATTRIBUTE_DEVICE)) != 0U) {
        return false;
    }
    if (out_information) *out_information = information;
    return true;
}

static uint64_t windows_file_size(
    const BY_HANDLE_FILE_INFORMATION* information) {
    return ((uint64_t)information->nFileSizeHigh << 32U) |
           (uint64_t)information->nFileSizeLow;
}

static bool windows_file_unchanged(
    const BY_HANDLE_FILE_INFORMATION* before,
    const BY_HANDLE_FILE_INFORMATION* after) {
    return before->dwVolumeSerialNumber == after->dwVolumeSerialNumber &&
        before->nFileIndexHigh == after->nFileIndexHigh &&
        before->nFileIndexLow == after->nFileIndexLow &&
        before->nFileSizeHigh == after->nFileSizeHigh &&
        before->nFileSizeLow == after->nFileSizeLow &&
        before->ftLastWriteTime.dwHighDateTime ==
            after->ftLastWriteTime.dwHighDateTime &&
        before->ftLastWriteTime.dwLowDateTime ==
            after->ftLastWriteTime.dwLowDateTime &&
        before->dwFileAttributes == after->dwFileAttributes;
}

static bool windows_basic_file_info(HANDLE handle,
                                    FILE_BASIC_INFO* out_information) {
    return handle != INVALID_HANDLE_VALUE && out_information &&
        GetFileInformationByHandleEx(
            handle, FileBasicInfo, out_information,
            sizeof(*out_information)) != 0;
}

static bool windows_file_id_info(
    HANDLE handle, FILE_ID_INFO* out_information) {
    return handle != INVALID_HANDLE_VALUE && out_information &&
        GetFileInformationByHandleEx(
            handle, FileIdInfo, out_information,
            sizeof(*out_information)) != 0;
}

static bool windows_standard_file_info(
    HANDLE handle, FILE_STANDARD_INFO* out_information) {
    return handle != INVALID_HANDLE_VALUE && out_information &&
        GetFileInformationByHandleEx(
            handle, FileStandardInfo, out_information,
            sizeof(*out_information)) != 0;
}

static bool windows_file_id_unchanged(
    const FILE_ID_INFO* before, const FILE_ID_INFO* after) {
    return before && after &&
        before->VolumeSerialNumber == after->VolumeSerialNumber &&
        memcmp(before->FileId.Identifier, after->FileId.Identifier,
               sizeof(before->FileId.Identifier)) == 0;
}

static bool windows_basic_file_unchanged(const FILE_BASIC_INFO* before,
                                         const FILE_BASIC_INFO* after) {
    return before->CreationTime.QuadPart == after->CreationTime.QuadPart &&
        before->LastWriteTime.QuadPart == after->LastWriteTime.QuadPart &&
        before->ChangeTime.QuadPart == after->ChangeTime.QuadPart &&
        before->FileAttributes == after->FileAttributes;
}

typedef struct {
    HANDLE file_handle;
    HANDLE mapping_handle;
    BY_HANDLE_FILE_INFORMATION before;
    FILE_BASIC_INFO before_basic;
    FILE_ID_INFO before_id;
    FILE_STANDARD_INFO before_standard;
    uint8_t snapshot_digest[COMMON_SHA256_DIGEST_SIZE];
    wchar_t path[];
} WindowsFileView;

static bool windows_mapping_allocation_error(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY ||
        error == ERROR_OUTOFMEMORY || error == ERROR_COMMITMENT_LIMIT;
}

static HANDLE windows_create_snapshot_file(void) {
    wchar_t directory[MAX_PATH + 1U];
    DWORD directory_size = GetTempPathW(
        (DWORD)(sizeof(directory) / sizeof(directory[0])), directory);
    if (directory_size == 0U ||
        directory_size >= sizeof(directory) / sizeof(directory[0])) {
        return INVALID_HANDLE_VALUE;
    }
    static volatile LONG sequence = 0;
    const LONG ticket = InterlockedIncrement(&sequence);
    wchar_t candidate[MAX_PATH + 96U];
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        int written = _snwprintf(
            candidate, sizeof(candidate) / sizeof(candidate[0]),
            L"%lsunity_common_file_view.%lu.%lu.%llu.%ld.%u",
            directory,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)GetTickCount64(), (long)ticket, attempt);
        if (written < 0 ||
            (size_t)written >=
                sizeof(candidate) / sizeof(candidate[0])) {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return INVALID_HANDLE_VALUE;
        }
        HANDLE handle = CreateFileW(
            candidate, GENERIC_READ | GENERIC_WRITE, 0U, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                FILE_FLAG_SEQUENTIAL_SCAN,
            NULL);
        if (handle != INVALID_HANDLE_VALUE) return handle;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return INVALID_HANDLE_VALUE;
        }
    }
    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
}

static bool windows_set_file_position(HANDLE handle, uint64_t position) {
    if (position > INT64_MAX) return false;
    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)position;
    return SetFilePointerEx(handle, offset, NULL, FILE_BEGIN) != 0;
}

static bool windows_copy_exact(
    HANDLE source, HANDLE snapshot, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!windows_set_file_position(source, 0U) ||
        !windows_set_file_position(snapshot, 0U)) {
        return false;
    }
    uint8_t buffer[64U * 1024U];
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        DWORD amount = remaining < sizeof(buffer)
            ? (DWORD)remaining : (DWORD)sizeof(buffer);
        DWORD received = 0U;
        DWORD written = 0U;
        if (!ReadFile(source, buffer, amount, &received, NULL) ||
            received == 0U ||
            !WriteFile(snapshot, buffer, received, &written, NULL) ||
            written != received) {
            return false;
        }
        common_sha256_update(&hash, buffer, received);
        position += (size_t)received;
    }
    common_sha256_final(&hash, digest);
    return true;
}

static bool windows_digest_exact(
    HANDLE handle, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!windows_set_file_position(handle, 0U)) return false;
    uint8_t buffer[64U * 1024U];
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        DWORD amount = remaining < sizeof(buffer)
            ? (DWORD)remaining : (DWORD)sizeof(buffer);
        DWORD received = 0U;
        if (!ReadFile(handle, buffer, amount, &received, NULL) ||
            received == 0U) {
            return false;
        }
        common_sha256_update(&hash, buffer, received);
        position += (size_t)received;
    }
    common_sha256_final(&hash, digest);
    return true;
}

CommonFileStatus common_file_view_open_regular(
    const char* path, size_t max_size, CommonFileView* out_view) {
    if (!out_view) return COMMON_FILE_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    if (!path || !path[0]) return COMMON_FILE_INVALID_ARGUMENT;

    wchar_t* requested_path = common_windows_utf8_to_wide(path);
    if (!requested_path) return windows_conversion_error(GetLastError());
    wchar_t* wide_path = windows_absolute_path(requested_path);
    DWORD absolute_error = wide_path ? ERROR_SUCCESS : GetLastError();
    free(requested_path);
    if (!wide_path) return windows_conversion_error(absolute_error);
    HANDLE file_handle = CreateFileW(
        wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        free(wide_path);
        return windows_open_error(error);
    }

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    BY_HANDLE_FILE_INFORMATION before;
    FILE_BASIC_INFO before_basic;
    FILE_ID_INFO before_id;
    FILE_STANDARD_INFO before_standard;
    HANDLE snapshot_handle = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle = NULL;
    const uint8_t* data = NULL;
    WindowsFileView* implementation = NULL;
    if (!windows_regular_file_info(file_handle, &before) ||
        !windows_basic_file_info(file_handle, &before_basic) ||
        !windows_file_id_info(file_handle, &before_id) ||
        !windows_standard_file_info(file_handle, &before_standard) ||
        before_standard.DeletePending || before_standard.Directory) {
        result = COMMON_FILE_NOT_REGULAR;
        goto fail;
    }
    uint64_t encoded_size = windows_file_size(&before);
    if (encoded_size > (uint64_t)SIZE_MAX ||
        encoded_size > (uint64_t)max_size) {
        result = COMMON_FILE_TOO_LARGE;
        goto fail;
    }
    size_t size = (size_t)encoded_size;
    size_t path_length = wcslen(wide_path);
    if (path_length >
        (SIZE_MAX - sizeof(*implementation)) / sizeof(*wide_path) - 1U) {
        result = COMMON_FILE_TOO_LARGE;
        goto fail;
    }
    implementation = (WindowsFileView*)malloc(
        sizeof(*implementation) +
        (path_length + 1U) * sizeof(*wide_path));
    if (!implementation) {
        result = COMMON_FILE_ALLOCATION_FAILED;
        goto fail;
    }
    uint8_t snapshot_digest[COMMON_SHA256_DIGEST_SIZE];
    if (size != 0U) {
        snapshot_handle = windows_create_snapshot_file();
        if (snapshot_handle == INVALID_HANDLE_VALUE) {
            result = windows_mapping_allocation_error(GetLastError())
                ? COMMON_FILE_ALLOCATION_FAILED : COMMON_FILE_IO_ERROR;
            goto fail;
        }
        if (!windows_copy_exact(
                file_handle, snapshot_handle, size, snapshot_digest)) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
        uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
        BY_HANDLE_FILE_INFORMATION source_after;
        FILE_BASIC_INFO source_basic_after;
        FILE_ID_INFO source_id_after;
        FILE_STANDARD_INFO source_standard_after;
        if (!windows_digest_exact(file_handle, size, source_digest) ||
            memcmp(snapshot_digest, source_digest,
                   sizeof(snapshot_digest)) != 0 ||
            !windows_regular_file_info(file_handle, &source_after) ||
            !windows_basic_file_info(file_handle, &source_basic_after) ||
            !windows_file_id_info(file_handle, &source_id_after) ||
            !windows_standard_file_info(
                file_handle, &source_standard_after) ||
            source_standard_after.DeletePending ||
            source_standard_after.Directory ||
            !windows_file_unchanged(&before, &source_after) ||
            !windows_basic_file_unchanged(
                &before_basic, &source_basic_after) ||
            !windows_file_id_unchanged(&before_id, &source_id_after)) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
        mapping_handle = CreateFileMappingW(
            snapshot_handle, NULL, PAGE_READONLY, 0U, 0U, NULL);
        if (!mapping_handle) {
            result = windows_mapping_allocation_error(GetLastError())
                ? COMMON_FILE_ALLOCATION_FAILED : COMMON_FILE_IO_ERROR;
            goto fail;
        }
        data = (const uint8_t*)MapViewOfFile(
            mapping_handle, FILE_MAP_READ, 0U, 0U, 0U);
        if (!data) {
            result = windows_mapping_allocation_error(GetLastError())
                ? COMMON_FILE_ALLOCATION_FAILED : COMMON_FILE_IO_ERROR;
            goto fail;
        }
        if (!CloseHandle(snapshot_handle)) {
            snapshot_handle = INVALID_HANDLE_VALUE;
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
        snapshot_handle = INVALID_HANDLE_VALUE;
    } else {
        common_sha256(NULL, 0U, snapshot_digest);
        uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
        if (!windows_digest_exact(file_handle, 0U, source_digest) ||
            memcmp(snapshot_digest, source_digest,
                   sizeof(snapshot_digest)) != 0) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
    }

    implementation->file_handle = file_handle;
    implementation->mapping_handle = mapping_handle;
    implementation->before = before;
    implementation->before_basic = before_basic;
    implementation->before_id = before_id;
    implementation->before_standard = before_standard;
    memcpy(implementation->snapshot_digest, snapshot_digest,
           sizeof(snapshot_digest));
    memcpy(implementation->path, wide_path,
           (path_length + 1U) * sizeof(*wide_path));
    free(wide_path);
    out_view->data = data;
    out_view->size = size;
    out_view->implementation = implementation;
    memcpy(out_view->captured_sha256, snapshot_digest,
           sizeof(out_view->captured_sha256));
    out_view->captured_sha256_recorded = true;
    return COMMON_FILE_OK;

fail:
    if (data) (void)UnmapViewOfFile(data);
    if (mapping_handle) (void)CloseHandle(mapping_handle);
    if (snapshot_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(snapshot_handle);
    }
    free(implementation);
    (void)CloseHandle(file_handle);
    free(wide_path);
    return result;
}

CommonFileStatus common_file_view_close(CommonFileView* view) {
    if (!view || !view->implementation) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    WindowsFileView* implementation =
        (WindowsFileView*)view->implementation;
    CommonFileStatus result = COMMON_FILE_OK;

    BY_HANDLE_FILE_INFORMATION after;
    FILE_BASIC_INFO after_basic;
    FILE_ID_INFO after_id;
    FILE_STANDARD_INFO after_standard;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    bool after_valid = windows_regular_file_info(
        implementation->file_handle, &after) &&
        windows_basic_file_info(
            implementation->file_handle, &after_basic) &&
        windows_file_id_info(implementation->file_handle, &after_id) &&
        windows_standard_file_info(
            implementation->file_handle, &after_standard) &&
        windows_digest_exact(
            implementation->file_handle, view->size, source_digest);
    if (!after_valid ||
        memcmp(implementation->snapshot_digest, source_digest,
               sizeof(source_digest)) != 0 ||
        !windows_file_unchanged(&implementation->before, &after) ||
        !windows_basic_file_unchanged(
            &implementation->before_basic, &after_basic) ||
        !windows_file_id_unchanged(
            &implementation->before_id, &after_id) ||
        after_standard.DeletePending || after_standard.Directory ||
        after_standard.NumberOfLinks !=
            implementation->before_standard.NumberOfLinks) {
        result = COMMON_FILE_IO_ERROR;
    }

    HANDLE path_handle = CreateFileW(
        implementation->path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION path_after;
    FILE_BASIC_INFO path_after_basic;
    FILE_ID_INFO path_after_id;
    if (!after_valid || path_handle == INVALID_HANDLE_VALUE ||
        !windows_regular_file_info(path_handle, &path_after) ||
        !windows_basic_file_info(path_handle, &path_after_basic) ||
        !windows_file_id_info(path_handle, &path_after_id) ||
        !windows_file_unchanged(&after, &path_after) ||
        !windows_basic_file_unchanged(&after_basic,
                                      &path_after_basic) ||
        !windows_file_id_unchanged(&after_id, &path_after_id)) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (path_handle != INVALID_HANDLE_VALUE && !CloseHandle(path_handle)) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (view->data && !UnmapViewOfFile(view->data)) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (implementation->mapping_handle &&
        !CloseHandle(implementation->mapping_handle)) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (!CloseHandle(implementation->file_handle)) {
        result = COMMON_FILE_IO_ERROR;
    }
    free(implementation);
    memset(view, 0, sizeof(*view));
    return result;
}

CommonFileStatus common_file_read_prefix_regular(
    const char* path, void* buffer, size_t capacity,
    size_t* out_prefix_size, uint64_t* out_file_size) {
    if (out_prefix_size) *out_prefix_size = 0U;
    if (out_file_size) *out_file_size = 0U;
    if (!path || !path[0] || (!buffer && capacity != 0U) ||
        !out_prefix_size || !out_file_size) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return windows_conversion_error(GetLastError());
    HANDLE handle = CreateFileW(
        wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD open_error = handle == INVALID_HANDLE_VALUE
        ? GetLastError() : ERROR_SUCCESS;
    free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_open_error(open_error);
    }
    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    BY_HANDLE_FILE_INFORMATION before;
    if (!windows_regular_file_info(handle, &before)) {
        result = COMMON_FILE_NOT_REGULAR;
        goto done;
    }
    uint64_t encoded_size = windows_file_size(&before);
    size_t amount = capacity;
    if (encoded_size < (uint64_t)amount) amount = (size_t)encoded_size;
    size_t position = 0U;
    while (position < amount) {
        size_t remaining = amount - position;
        DWORD requested = remaining > (size_t)UINT32_MAX
            ? UINT32_MAX : (DWORD)remaining;
        DWORD received = 0U;
        if (!ReadFile(handle, (uint8_t*)buffer + position, requested,
                      &received, NULL) || received == 0U) {
            goto done;
        }
        position += (size_t)received;
    }
    BY_HANDLE_FILE_INFORMATION after;
    if (!windows_regular_file_info(handle, &after) ||
        !windows_file_unchanged(&before, &after)) {
        goto done;
    }
    *out_prefix_size = amount;
    *out_file_size = encoded_size;
    result = COMMON_FILE_OK;

done:
    if (!CloseHandle(handle) && result == COMMON_FILE_OK) {
        *out_prefix_size = 0U;
        *out_file_size = 0U;
        result = COMMON_FILE_IO_ERROR;
    }
    return result;
}

CommonFileStatus common_file_read_regular(
    const char* path, size_t max_size, CommonFileBytes* out_file) {
    if (!out_file) return COMMON_FILE_INVALID_ARGUMENT;
    out_file->data = NULL;
    out_file->size = 0U;
    if (!path || !path[0]) return COMMON_FILE_INVALID_ARGUMENT;

    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return windows_conversion_error(GetLastError());
    HANDLE handle = CreateFileW(
        wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD open_error = handle == INVALID_HANDLE_VALUE
        ? GetLastError() : ERROR_SUCCESS;
    free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_open_error(open_error);
    }

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    BY_HANDLE_FILE_INFORMATION before;
    uint8_t* data = NULL;
    size_t size = 0U;
    if (!windows_regular_file_info(handle, &before)) {
        result = COMMON_FILE_NOT_REGULAR;
        goto done;
    }
    uint64_t encoded_size = windows_file_size(&before);
    if (encoded_size > (uint64_t)SIZE_MAX ||
        encoded_size > (uint64_t)max_size) {
        result = COMMON_FILE_TOO_LARGE;
        goto done;
    }
    size = (size_t)encoded_size;
    if (size != 0U) {
        data = (uint8_t*)malloc(size);
        if (!data) {
            result = COMMON_FILE_ALLOCATION_FAILED;
            goto done;
        }
    }

    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        DWORD amount = remaining > (size_t)UINT32_MAX
            ? UINT32_MAX : (DWORD)remaining;
        DWORD received = 0U;
        if (!ReadFile(handle, data + position, amount, &received, NULL) ||
            received == 0U) {
            goto done;
        }
        position += (size_t)received;
    }
    uint8_t extra = 0U;
    DWORD extra_size = 0U;
    BY_HANDLE_FILE_INFORMATION after;
    if (!ReadFile(handle, &extra, 1U, &extra_size, NULL) ||
        extra_size != 0U || !windows_regular_file_info(handle, &after) ||
        !windows_file_unchanged(&before, &after)) {
        goto done;
    }

    out_file->data = data;
    out_file->size = size;
    data = NULL;
    result = COMMON_FILE_OK;

done:
    free(data);
    if (!CloseHandle(handle) && result == COMMON_FILE_OK) {
        common_file_bytes_dispose(out_file);
        result = COMMON_FILE_IO_ERROR;
    }
    return result;
}

static bool windows_write_all(HANDLE handle, const uint8_t* data,
                              size_t size) {
    while (size != 0U) {
        DWORD amount = size > (size_t)UINT32_MAX
            ? UINT32_MAX : (DWORD)size;
        DWORD written = 0U;
        if (!WriteFile(handle, data, amount, &written, NULL) ||
            written == 0U) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

static bool windows_directory_identity(
    HANDLE handle, FILE_ID_INFO* out_identity) {
    BY_HANDLE_FILE_INFORMATION information;
    return handle != INVALID_HANDLE_VALUE && out_identity &&
        GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle, &information) != 0 &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0U &&
        windows_file_id_info(handle, out_identity);
}

static HANDLE windows_open_directory_anchor(
    const wchar_t* path, FILE_ID_INFO* out_identity) {
    HANDLE handle = CreateFileW(
        path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    if (!windows_directory_identity(handle, out_identity)) {
        (void)CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

static wchar_t* windows_destination_parent(
    const wchar_t* absolute_path, const wchar_t** out_name) {
    if (!absolute_path || !absolute_path[0] || !out_name) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }
    const wchar_t* separator = wcsrchr(absolute_path, L'\\');
    const wchar_t* alternate = wcsrchr(absolute_path, L'/');
    if (!separator || (alternate && alternate > separator)) {
        separator = alternate;
    }
    if (!separator || !separator[1] ||
        (separator[1] == L'.' && !separator[2]) ||
        (separator[1] == L'.' && separator[2] == L'.' &&
         !separator[3])) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }
    size_t parent_size = (size_t)(separator - absolute_path);
    if (parent_size == 0U ||
        (parent_size == 2U && absolute_path[1] == L':')) {
        ++parent_size;
    }
    wchar_t* parent = (wchar_t*)malloc(
        (parent_size + 1U) * sizeof(*parent));
    if (!parent) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(parent, absolute_path, parent_size * sizeof(*parent));
    parent[parent_size] = L'\0';
    *out_name = separator + 1;
    return parent;
}

static bool windows_namespace_parent_matches(
    const wchar_t* parent_path, const FILE_ID_INFO* expected) {
    FILE_ID_INFO observed;
    HANDLE reopened = windows_open_directory_anchor(
        parent_path, &observed);
    bool matches = reopened != INVALID_HANDLE_VALUE &&
        windows_file_id_unchanged(expected, &observed);
    if (reopened != INVALID_HANDLE_VALUE && !CloseHandle(reopened)) {
        matches = false;
    }
    return matches;
}

static bool windows_path_file_matches(
    const wchar_t* path, const FILE_ID_INFO* expected) {
    HANDLE reopened = CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    FILE_ID_INFO observed;
    bool matches = reopened != INVALID_HANDLE_VALUE &&
        windows_regular_file_info(reopened, NULL) &&
        windows_file_id_info(reopened, &observed) &&
        windows_file_id_unchanged(expected, &observed);
    if (reopened != INVALID_HANDLE_VALUE && !CloseHandle(reopened)) {
        matches = false;
    }
    return matches;
}

static bool windows_delete_open_file(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition;
    disposition.DeleteFile = TRUE;
    return handle != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(
            handle, FileDispositionInfo, &disposition,
            sizeof(disposition)) != 0;
}

CommonFileStatus common_file_write_new_atomic(
    const char* path, const void* data, size_t size) {
    if (!path || !path[0] || (!data && size != 0U)) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    wchar_t* requested_path = common_windows_utf8_to_wide(path);
    if (!requested_path) return windows_conversion_error(GetLastError());
    wchar_t* wide_path = windows_absolute_path(requested_path);
    DWORD absolute_error = wide_path ? ERROR_SUCCESS : GetLastError();
    free(requested_path);
    if (!wide_path) return windows_conversion_error(absolute_error);
    const wchar_t* destination_name = NULL;
    wchar_t* parent_path = windows_destination_parent(
        wide_path, &destination_name);
    if (!parent_path) {
        DWORD error = GetLastError();
        free(wide_path);
        return error == ERROR_NOT_ENOUGH_MEMORY
            ? COMMON_FILE_ALLOCATION_FAILED
            : COMMON_FILE_INVALID_ARGUMENT;
    }
    size_t destination_name_size = wcslen(wide_path);
    if (destination_name_size > UINT32_MAX / sizeof(*wide_path)) {
        free(parent_path);
        free(wide_path);
        return COMMON_FILE_TOO_LARGE;
    }
    FILE_ID_INFO parent_identity;
    HANDLE parent_handle = windows_open_directory_anchor(
        parent_path, &parent_identity);
    if (parent_handle == INVALID_HANDLE_VALUE) {
        free(parent_path);
        free(wide_path);
        return COMMON_FILE_IO_ERROR;
    }

    size_t parent_size = wcslen(parent_path);
    if (parent_size > SIZE_MAX - 128U) {
        (void)CloseHandle(parent_handle);
        free(parent_path);
        free(wide_path);
        return COMMON_FILE_TOO_LARGE;
    }
    size_t temporary_capacity = parent_size + 128U;
    wchar_t* wide_temporary = (wchar_t*)malloc(
        temporary_capacity * sizeof(*wide_temporary));
    if (!wide_temporary) {
        (void)CloseHandle(parent_handle);
        free(parent_path);
        free(wide_path);
        return COMMON_FILE_ALLOCATION_FAILED;
    }

    static volatile LONG sequence = 0;
    LONG ticket = InterlockedIncrement(&sequence);
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        const bool separator = parent_size != 0U &&
            parent_path[parent_size - 1U] != L'\\' &&
            parent_path[parent_size - 1U] != L'/';
        int written = _snwprintf(
            wide_temporary, temporary_capacity,
            L"%ls%ls.unity_common.tmp.%lu.%lu.%ld.%u", parent_path,
            separator ? L"\\" : L"",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetCurrentThreadId(),
            (long)ticket, attempt);
        if (written < 0 || (size_t)written >= temporary_capacity) break;
        handle = CreateFileW(
            wide_temporary,
            GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
        DWORD error = handle == INVALID_HANDLE_VALUE
            ? GetLastError() : ERROR_SUCCESS;
        if (handle != INVALID_HANDLE_VALUE) break;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        (void)CloseHandle(parent_handle);
        free(wide_temporary);
        free(parent_path);
        free(wide_path);
        return COMMON_FILE_IO_ERROR;
    }

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    FILE_ID_INFO temporary_identity;
    bool written = windows_write_all(
            handle, (const uint8_t*)data, size) &&
        FlushFileBuffers(handle) != 0 &&
        windows_regular_file_info(handle, NULL) &&
        windows_file_id_info(handle, &temporary_identity);
    bool renamed = false;
    DWORD rename_error = ERROR_SUCCESS;
    if (written) {
        size_t name_bytes = destination_name_size *
            sizeof(*destination_name);
        size_t information_size = offsetof(FILE_RENAME_INFO, FileName) +
            name_bytes + sizeof(*wide_path);
        if (information_size <= UINT32_MAX) {
            FILE_RENAME_INFO* information =
                (FILE_RENAME_INFO*)calloc(1U, information_size);
            if (information) {
                information->ReplaceIfExists = FALSE;
                /* Rename the held source object.  Some supported Win32
                 * implementations ignore RootDirectory for FileRenameInfo,
                 * so use the stable absolute destination spelling and prove
                 * both parent and resulting file identity before success. */
                information->RootDirectory = NULL;
                information->FileNameLength = (DWORD)name_bytes;
                memcpy(information->FileName, wide_path,
                       name_bytes);
                information->FileName[destination_name_size] = L'\0';
                renamed = SetFileInformationByHandle(
                    handle, FileRenameInfo, information,
                    (DWORD)information_size) != 0;
                rename_error = renamed ? ERROR_SUCCESS : GetLastError();
                free(information);
            } else {
                rename_error = ERROR_NOT_ENOUGH_MEMORY;
            }
        } else {
            rename_error = ERROR_BUFFER_OVERFLOW;
        }
    }

    bool parent_stable = windows_namespace_parent_matches(
        parent_path, &parent_identity);
    if (renamed) {
        bool postcondition = FlushFileBuffers(handle) != 0 &&
            parent_stable &&
            windows_path_file_matches(wide_path, &temporary_identity);
        if (postcondition) result = COMMON_FILE_OK;
    } else if (written && parent_stable &&
               (rename_error == ERROR_ALREADY_EXISTS ||
                rename_error == ERROR_FILE_EXISTS)) {
        result = COMMON_FILE_ALREADY_EXISTS;
    }

    if (result != COMMON_FILE_OK && !windows_delete_open_file(handle)) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (!CloseHandle(handle)) result = COMMON_FILE_IO_ERROR;
    if (!CloseHandle(parent_handle)) result = COMMON_FILE_IO_ERROR;
    free(wide_temporary);
    free(parent_path);
    free(wide_path);
    return result;
}

#else

static long stat_mtime_nanoseconds(const struct stat* status) {
#if defined(__APPLE__)
    return status->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    return status->st_mtim.tv_nsec;
#else
    (void)status;
    return 0L;
#endif
}

static long stat_ctime_nanoseconds(const struct stat* status) {
#if defined(__APPLE__)
    return status->st_ctimespec.tv_nsec;
#elif defined(__linux__)
    return status->st_ctim.tv_nsec;
#else
    (void)status;
    return 0L;
#endif
}

static bool stat_same_identity(const struct stat* left,
                               const struct stat* right) {
    return S_ISREG(left->st_mode) && S_ISREG(right->st_mode) &&
        left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static bool stat_same_directory_identity(const struct stat* left,
                                         const struct stat* right) {
    return left && right && S_ISDIR(left->st_mode) &&
        S_ISDIR(right->st_mode) && left->st_dev == right->st_dev &&
        left->st_ino == right->st_ino;
}

static bool stat_file_unchanged(const struct stat* before,
                                const struct stat* after) {
    return stat_same_identity(before, after) &&
        before->st_size == after->st_size &&
        before->st_mode == after->st_mode &&
        before->st_mtime == after->st_mtime &&
        stat_mtime_nanoseconds(before) == stat_mtime_nanoseconds(after) &&
        before->st_ctime == after->st_ctime &&
        stat_ctime_nanoseconds(before) == stat_ctime_nanoseconds(after);
}

static CommonFileStatus posix_path_error(int error) {
    if (error == ENOENT || error == ENOTDIR) return COMMON_FILE_NOT_FOUND;
    if (error == ELOOP) return COMMON_FILE_NOT_REGULAR;
    return COMMON_FILE_IO_ERROR;
}

static bool posix_read_all(int descriptor, uint8_t* data, size_t size) {
    while (size != 0U) {
        size_t amount = size > (size_t)INT_MAX ? (size_t)INT_MAX : size;
        ssize_t received = read(descriptor, data, amount);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        data += (size_t)received;
        size -= (size_t)received;
    }
    return true;
}

static bool posix_write_all(int descriptor, const uint8_t* data,
                            size_t size);

static bool posix_flush_file(int descriptor) {
#if defined(__APPLE__)
    if (fcntl(descriptor, F_FULLFSYNC, 0) == 0) return true;
    if (errno != ENOTSUP && errno != EINVAL) return false;
    return fsync(descriptor) == 0;
#else
    return fsync(descriptor) == 0;
#endif
}

enum { POSIX_TEMPORARY_NAME_CAPACITY = 128 };

#if !defined(__linux__)
static bool posix_random_bytes(uint8_t* bytes, size_t size) {
    if (!bytes && size != 0U) return false;
#if defined(__APPLE__)
    while (getentropy(bytes, size) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
#elif defined(__linux__)
    size_t position = 0U;
    while (position < size) {
        ssize_t received = getrandom(bytes + position, size - position, 0U);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        position += (size_t)received;
    }
    return true;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = open("/dev/urandom", flags);
    if (descriptor < 0) return false;
    bool read_ok = posix_read_all(descriptor, bytes, size);
    if (close(descriptor) != 0) read_ok = false;
    return read_ok;
#endif
}

static CommonFileStatus posix_create_named_temporary(
    int directory_descriptor, const char* purpose,
    int* out_descriptor,
    char out_name[POSIX_TEMPORARY_NAME_CAPACITY]) {
    if (directory_descriptor < 0 || !purpose || !purpose[0] ||
        !out_descriptor || !out_name) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    *out_descriptor = -1;
    out_name[0] = '\0';
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    static const char hexadecimal[] = "0123456789abcdef";
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        uint8_t random[16];
        char suffix[sizeof(random) * 2U + 1U];
        if (!posix_random_bytes(random, sizeof(random))) {
            return COMMON_FILE_IO_ERROR;
        }
        for (size_t index = 0U; index < sizeof(random); ++index) {
            suffix[index * 2U] = hexadecimal[random[index] >> 4U];
            suffix[index * 2U + 1U] =
                hexadecimal[random[index] & 0x0fU];
        }
        suffix[sizeof(suffix) - 1U] = '\0';
        int written = snprintf(
            out_name, POSIX_TEMPORARY_NAME_CAPACITY,
            ".unity_common.%s.%s", purpose, suffix);
        if (written < 0 || written >= POSIX_TEMPORARY_NAME_CAPACITY) {
            out_name[0] = '\0';
            return COMMON_FILE_TOO_LARGE;
        }
        int descriptor = openat(
            directory_descriptor, out_name, flags, 0600);
        if (descriptor >= 0) {
#ifndef O_CLOEXEC
            int descriptor_flags = fcntl(descriptor, F_GETFD);
            if (descriptor_flags < 0 ||
                fcntl(descriptor, F_SETFD,
                      descriptor_flags | FD_CLOEXEC) != 0) {
                (void)close(descriptor);
                return COMMON_FILE_IO_ERROR;
            }
#endif
            *out_descriptor = descriptor;
            return COMMON_FILE_OK;
        }
        if (errno != EEXIST) break;
    }
    out_name[0] = '\0';
    return COMMON_FILE_IO_ERROR;
}

/* Remove only the name that still denotes the held temporary object. All
 * lookup and deletion is relative to the already-held directory, so replacing
 * the directory's external pathname cannot redirect cleanup. */
static bool posix_unlink_held_temporary(
    int directory_descriptor, const char* name, int file_descriptor,
    nlink_t expected_links_after) {
    if (directory_descriptor < 0 || !name || !name[0] ||
        file_descriptor < 0) {
        return false;
    }
    struct stat held;
    struct stat named;
    if (fstat(file_descriptor, &held) != 0 ||
        !S_ISREG(held.st_mode)) {
        return false;
    }
    if (fstatat(directory_descriptor, name, &named,
                AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT || held.st_nlink != expected_links_after) {
            return false;
        }
        return true;
    }
    if (!stat_same_identity(&held, &named) ||
        unlinkat(directory_descriptor, name, 0) != 0) {
        return false;
    }
    struct stat after;
    return fstat(file_descriptor, &after) == 0 &&
        stat_same_identity(&held, &after) &&
        after.st_nlink == expected_links_after;
}
#endif

static bool posix_copy_to_memory_exact(
    int source_descriptor, uint8_t* snapshot, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!snapshot && size != 0U) return false;
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        size_t amount = remaining < 64U * 1024U ? remaining
                                                : 64U * 1024U;
        ssize_t received = pread(
            source_descriptor, snapshot + position, amount,
            (off_t)position);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        common_sha256_update(
            &hash, snapshot + position, (size_t)received);
        position += (size_t)received;
    }
    common_sha256_final(&hash, digest);
    return true;
}

static bool posix_digest_exact(
    int descriptor, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    uint8_t buffer[64U * 1024U];
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        size_t remaining = size - position;
        size_t amount = remaining < sizeof(buffer) ? remaining
                                                    : sizeof(buffer);
        ssize_t received = pread(
            descriptor, buffer, amount, (off_t)position);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        common_sha256_update(&hash, buffer, (size_t)received);
        position += (size_t)received;
    }
    common_sha256_final(&hash, digest);
    return true;
}

typedef struct {
    int source_descriptor;
    int namespace_descriptor;
    struct stat before;
    uint8_t snapshot_digest[COMMON_SHA256_DIGEST_SIZE];
    char path[];
} PosixFileView;

CommonFileStatus common_file_view_open_regular(
    const char* path, size_t max_size, CommonFileView* out_view) {
    if (!out_view) return COMMON_FILE_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    if (!path || !path[0]) return COMMON_FILE_INVALID_ARGUMENT;

    int namespace_flags = O_RDONLY;
#ifdef O_CLOEXEC
    namespace_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    namespace_flags |= O_DIRECTORY;
#endif
    /* Relative names are permanently rooted at the caller's cwd as it stood
     * at open. Absolute names remain rooted at '/'. This preserves the full
     * original namespace path across chdir and detects parent replacement. */
    int namespace_descriptor = open(path[0] == '/' ? "/" : ".",
                                    namespace_flags);
    if (namespace_descriptor < 0) return posix_path_error(errno);

    struct stat path_before;
    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    if (fstatat(namespace_descriptor, path, &path_before,
                AT_SYMLINK_NOFOLLOW) != 0) {
        result = posix_path_error(errno);
        (void)close(namespace_descriptor);
        return result;
    }
    if (!S_ISREG(path_before.st_mode)) {
        (void)close(namespace_descriptor);
        return COMMON_FILE_NOT_REGULAR;
    }

    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = openat(namespace_descriptor, path, flags);
    if (descriptor < 0) {
        result = posix_path_error(errno);
        (void)close(namespace_descriptor);
        return result;
    }

    result = COMMON_FILE_IO_ERROR;
    struct stat before;
    const uint8_t* data = NULL;
    size_t size = 0U;
    PosixFileView* implementation = NULL;
    uint8_t snapshot_digest[COMMON_SHA256_DIGEST_SIZE];
    if (fstat(descriptor, &before) != 0 ||
        !stat_file_unchanged(&path_before, &before)) {
        goto fail;
    }
    if (before.st_size < 0 || (uintmax_t)before.st_size > SIZE_MAX ||
        (uintmax_t)before.st_size > (uintmax_t)max_size) {
        result = COMMON_FILE_TOO_LARGE;
        goto fail;
    }
    size = (size_t)before.st_size;
    size_t path_size = strlen(path);
    if (path_size > SIZE_MAX - sizeof(*implementation) - 1U) {
        result = COMMON_FILE_TOO_LARGE;
        goto fail;
    }
    implementation = (PosixFileView*)malloc(
        sizeof(*implementation) + path_size + 1U);
    if (!implementation) {
        result = COMMON_FILE_ALLOCATION_FAILED;
        goto fail;
    }
    if (size != 0U) {
        /* A direct regular-file mapping may SIGBUS after another process
         * truncates the pathname. Copy through the held source descriptor,
         * independently sample the held bytes and pathname after the copy,
         * and expose only anonymous process-private memory. */
        void* mapping = mmap(
            NULL, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mapping == NULL) {
            if (mapping == NULL) (void)munmap(mapping, size);
            result = errno == ENOMEM ? COMMON_FILE_ALLOCATION_FAILED
                                     : COMMON_FILE_IO_ERROR;
            goto fail;
        }
        data = (const uint8_t*)mapping;
        if (!posix_copy_to_memory_exact(
                descriptor, (uint8_t*)mapping, size,
                snapshot_digest)) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
        uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
        struct stat source_after;
        struct stat path_after;
        if (fstat(descriptor, &source_after) != 0 ||
            fstatat(namespace_descriptor, path, &path_after,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !posix_digest_exact(descriptor, size, source_digest) ||
            memcmp(snapshot_digest, source_digest,
                   sizeof(snapshot_digest)) != 0 ||
            !stat_file_unchanged(&before, &source_after) ||
            !stat_file_unchanged(&source_after, &path_after)) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
        if (mprotect(mapping, size, PROT_READ) != 0) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
#ifdef POSIX_MADV_SEQUENTIAL
        (void)posix_madvise(mapping, size, POSIX_MADV_SEQUENTIAL);
#endif
    } else {
        common_sha256(NULL, 0U, snapshot_digest);
        struct stat source_after;
        struct stat path_after;
        uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
        if (fstat(descriptor, &source_after) != 0 ||
            fstatat(namespace_descriptor, path, &path_after,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !posix_digest_exact(descriptor, 0U, source_digest) ||
            memcmp(snapshot_digest, source_digest,
                   sizeof(snapshot_digest)) != 0 ||
            !stat_file_unchanged(&before, &source_after) ||
            !stat_file_unchanged(&source_after, &path_after)) {
            result = COMMON_FILE_IO_ERROR;
            goto fail;
        }
    }

    implementation->source_descriptor = descriptor;
    implementation->namespace_descriptor = namespace_descriptor;
    implementation->before = before;
    memcpy(implementation->snapshot_digest, snapshot_digest,
           sizeof(snapshot_digest));
    memcpy(implementation->path, path, path_size + 1U);
    out_view->data = data;
    out_view->size = size;
    out_view->implementation = implementation;
    memcpy(out_view->captured_sha256, snapshot_digest,
           sizeof(out_view->captured_sha256));
    out_view->captured_sha256_recorded = true;
    return COMMON_FILE_OK;

fail:
    if (data) (void)munmap((void*)data, size);
    free(implementation);
    (void)close(descriptor);
    (void)close(namespace_descriptor);
    return result;
}

CommonFileStatus common_file_view_close(CommonFileView* view) {
    if (!view || !view->implementation) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    PosixFileView* implementation = (PosixFileView*)view->implementation;
    CommonFileStatus result = COMMON_FILE_OK;
    struct stat after;
    struct stat path_after;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    if (fstat(implementation->source_descriptor, &after) != 0 ||
        fstatat(implementation->namespace_descriptor,
                implementation->path, &path_after,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !posix_digest_exact(implementation->source_descriptor,
                            view->size, source_digest) ||
        memcmp(implementation->snapshot_digest, source_digest,
               sizeof(source_digest)) != 0 ||
        !stat_file_unchanged(&implementation->before, &after) ||
        !stat_file_unchanged(&after, &path_after)) {
        result = COMMON_FILE_IO_ERROR;
    }
#ifdef POSIX_MADV_DONTNEED
    if (view->data && view->size != 0U) {
        (void)posix_madvise((void*)view->data, view->size,
                            POSIX_MADV_DONTNEED);
    }
#endif
    if (view->data && munmap((void*)view->data, view->size) != 0) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (close(implementation->source_descriptor) != 0) {
        result = COMMON_FILE_IO_ERROR;
    }
    if (close(implementation->namespace_descriptor) != 0) {
        result = COMMON_FILE_IO_ERROR;
    }
    free(implementation);
    memset(view, 0, sizeof(*view));
    return result;
}

CommonFileStatus common_file_read_prefix_regular(
    const char* path, void* buffer, size_t capacity,
    size_t* out_prefix_size, uint64_t* out_file_size) {
    if (out_prefix_size) *out_prefix_size = 0U;
    if (out_file_size) *out_file_size = 0U;
    if (!path || !path[0] || (!buffer && capacity != 0U) ||
        !out_prefix_size || !out_file_size) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    struct stat path_before;
    if (lstat(path, &path_before) != 0) return posix_path_error(errno);
    if (!S_ISREG(path_before.st_mode)) return COMMON_FILE_NOT_REGULAR;

    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags);
    if (descriptor < 0) return posix_path_error(errno);

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    struct stat before;
    if (fstat(descriptor, &before) != 0 ||
        !stat_same_identity(&path_before, &before)) {
        goto done;
    }
    if (before.st_size < 0 || (uintmax_t)before.st_size > UINT64_MAX) {
        result = COMMON_FILE_TOO_LARGE;
        goto done;
    }
    size_t amount = capacity;
    if ((uintmax_t)before.st_size < (uintmax_t)amount) {
        amount = (size_t)before.st_size;
    }
    if (!posix_read_all(descriptor, (uint8_t*)buffer, amount)) goto done;

    struct stat after;
    struct stat path_after;
    if (fstat(descriptor, &after) != 0 ||
        lstat(path, &path_after) != 0 ||
        !stat_file_unchanged(&before, &after) ||
        !stat_same_identity(&after, &path_after)) {
        goto done;
    }
    *out_prefix_size = amount;
    *out_file_size = (uint64_t)before.st_size;
    result = COMMON_FILE_OK;

done:
    if (close(descriptor) != 0 && result == COMMON_FILE_OK) {
        *out_prefix_size = 0U;
        *out_file_size = 0U;
        result = COMMON_FILE_IO_ERROR;
    }
    return result;
}

CommonFileStatus common_file_read_regular(
    const char* path, size_t max_size, CommonFileBytes* out_file) {
    if (!out_file) return COMMON_FILE_INVALID_ARGUMENT;
    out_file->data = NULL;
    out_file->size = 0U;
    if (!path || !path[0]) return COMMON_FILE_INVALID_ARGUMENT;

    struct stat path_before;
    if (lstat(path, &path_before) != 0) return posix_path_error(errno);
    if (!S_ISREG(path_before.st_mode)) return COMMON_FILE_NOT_REGULAR;

    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags);
    if (descriptor < 0) return posix_path_error(errno);

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    struct stat before;
    uint8_t* data = NULL;
    size_t size = 0U;
    if (fstat(descriptor, &before) != 0 ||
        !stat_same_identity(&path_before, &before)) {
        goto done;
    }
    if (before.st_size < 0 || (uintmax_t)before.st_size > SIZE_MAX ||
        (uintmax_t)before.st_size > (uintmax_t)max_size) {
        result = COMMON_FILE_TOO_LARGE;
        goto done;
    }
    size = (size_t)before.st_size;
    if (size != 0U) {
        data = (uint8_t*)malloc(size);
        if (!data) {
            result = COMMON_FILE_ALLOCATION_FAILED;
            goto done;
        }
    }
    if (!posix_read_all(descriptor, data, size)) goto done;

    uint8_t extra = 0U;
    ssize_t received;
    do {
        received = read(descriptor, &extra, 1U);
    } while (received < 0 && errno == EINTR);
    struct stat after;
    struct stat path_after;
    if (received != 0 || fstat(descriptor, &after) != 0 ||
        lstat(path, &path_after) != 0 ||
        !stat_file_unchanged(&before, &after) ||
        !stat_same_identity(&after, &path_after)) {
        goto done;
    }

    out_file->data = data;
    out_file->size = size;
    data = NULL;
    result = COMMON_FILE_OK;

done:
    free(data);
    if (close(descriptor) != 0 && result == COMMON_FILE_OK) {
        common_file_bytes_dispose(out_file);
        result = COMMON_FILE_IO_ERROR;
    }
    return result;
}

static bool posix_write_all(int descriptor, const uint8_t* data,
                            size_t size) {
    while (size != 0U) {
        size_t amount = size > (size_t)INT_MAX ? (size_t)INT_MAX : size;
        ssize_t written = write(descriptor, data, amount);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static int posix_open_directory_at(int root_descriptor,
                                   const char* relative_path) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = openat(root_descriptor, relative_path, flags);
    if (descriptor >= 0) {
        struct stat status;
        if (fstat(descriptor, &status) != 0 ||
            !S_ISDIR(status.st_mode)) {
            (void)close(descriptor);
            descriptor = -1;
        }
    }
    return descriptor;
}

static CommonFileStatus posix_destination_parts(
    const char* path, bool* out_absolute, char** out_parent,
    char** out_name) {
    if (!path || !path[0] || !out_absolute || !out_parent || !out_name) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    *out_parent = NULL;
    *out_name = NULL;
    *out_absolute = path[0] == '/';
    const char* separator = strrchr(path, '/');
    const char* name = separator ? separator + 1 : path;
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    size_t name_size = strlen(name);
    if (name_size == SIZE_MAX) return COMMON_FILE_TOO_LARGE;
    char* name_copy = (char*)malloc(name_size + 1U);
    if (!name_copy) return COMMON_FILE_ALLOCATION_FAILED;
    memcpy(name_copy, name, name_size + 1U);

    const char* parent_start = path;
    size_t parent_size = 0U;
    if (separator) {
        if (*out_absolute) {
            while (*parent_start == '/') ++parent_start;
            if (separator >= parent_start) {
                parent_size = (size_t)(separator - parent_start);
            }
        } else {
            parent_size = (size_t)(separator - path);
        }
    }
    static const char current[] = ".";
    if (!separator || parent_size == 0U) {
        parent_start = current;
        parent_size = sizeof(current) - 1U;
    }
    char* parent_copy = (char*)malloc(parent_size + 1U);
    if (!parent_copy) {
        free(name_copy);
        return COMMON_FILE_ALLOCATION_FAILED;
    }
    memcpy(parent_copy, parent_start, parent_size);
    parent_copy[parent_size] = '\0';
    *out_parent = parent_copy;
    *out_name = name_copy;
    return COMMON_FILE_OK;
}

static bool posix_namespace_parent_matches(
    int root_descriptor, const char* parent_path,
    const struct stat* expected_parent) {
    int reopened = posix_open_directory_at(root_descriptor, parent_path);
    if (reopened < 0) return false;
    struct stat observed;
    bool matches = fstat(reopened, &observed) == 0 &&
        stat_same_directory_identity(expected_parent, &observed);
    if (close(reopened) != 0) matches = false;
    return matches;
}

static bool posix_namespace_destination_matches(
    int root_descriptor, const char* parent_path,
    const struct stat* expected_parent, const char* destination_name,
    const struct stat* expected_file) {
    int reopened = posix_open_directory_at(root_descriptor, parent_path);
    if (reopened < 0) return false;
    struct stat observed_parent;
    struct stat observed_file;
    bool matches = fstat(reopened, &observed_parent) == 0 &&
        stat_same_directory_identity(expected_parent, &observed_parent) &&
        fstatat(reopened, destination_name, &observed_file,
                AT_SYMLINK_NOFOLLOW) == 0 &&
        stat_same_identity(expected_file, &observed_file);
    if (close(reopened) != 0) matches = false;
    return matches;
}

CommonFileStatus common_file_write_new_atomic(
    const char* path, const void* data, size_t size) {
    if (!path || !path[0] || (!data && size != 0U)) {
        return COMMON_FILE_INVALID_ARGUMENT;
    }
    bool absolute = false;
    char* parent_path = NULL;
    char* destination_name = NULL;
    CommonFileStatus parts_status = posix_destination_parts(
        path, &absolute, &parent_path, &destination_name);
    if (parts_status != COMMON_FILE_OK) return parts_status;
    int root_flags = O_RDONLY;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    root_flags |= O_DIRECTORY;
#endif
    int root_descriptor = open(absolute ? "/" : ".", root_flags);
    if (root_descriptor < 0) {
        free(destination_name);
        free(parent_path);
        return COMMON_FILE_IO_ERROR;
    }
    int parent_descriptor = posix_open_directory_at(
        root_descriptor, parent_path);
    struct stat parent_identity;
    if (parent_descriptor < 0 ||
        fstat(parent_descriptor, &parent_identity) != 0 ||
        !S_ISDIR(parent_identity.st_mode)) {
        if (parent_descriptor >= 0) (void)close(parent_descriptor);
        (void)close(root_descriptor);
        free(destination_name);
        free(parent_path);
        return COMMON_FILE_IO_ERROR;
    }

    struct stat existing_destination;
    if (fstatat(parent_descriptor, destination_name,
                &existing_destination, AT_SYMLINK_NOFOLLOW) == 0) {
        CommonFileStatus existing_result =
            posix_namespace_parent_matches(
                root_descriptor, parent_path, &parent_identity)
                ? COMMON_FILE_ALREADY_EXISTS
                : COMMON_FILE_IO_ERROR;
        bool parent_closed = close(parent_descriptor) == 0;
        bool root_closed = close(root_descriptor) == 0;
        if (!parent_closed || !root_closed) {
            existing_result = COMMON_FILE_IO_ERROR;
        }
        free(destination_name);
        free(parent_path);
        return existing_result;
    }
    if (errno != ENOENT) {
        (void)close(parent_descriptor);
        (void)close(root_descriptor);
        free(destination_name);
        free(parent_path);
        return COMMON_FILE_IO_ERROR;
    }

    int descriptor = -1;
#if defined(__linux__)
#ifdef O_TMPFILE
    int temporary_flags = O_RDWR | O_TMPFILE;
#ifdef O_CLOEXEC
    temporary_flags |= O_CLOEXEC;
#endif
    descriptor = openat(
        parent_descriptor, ".", temporary_flags, 0600);
    CommonFileStatus creation_status = descriptor >= 0
        ? COMMON_FILE_OK : COMMON_FILE_IO_ERROR;
#else
    CommonFileStatus creation_status = COMMON_FILE_IO_ERROR;
#endif
#else
    char temporary_name[POSIX_TEMPORARY_NAME_CAPACITY];
    CommonFileStatus creation_status = posix_create_named_temporary(
        parent_descriptor, "publish", &descriptor, temporary_name);
#endif
    if (creation_status != COMMON_FILE_OK) {
        (void)close(parent_descriptor);
        (void)close(root_descriptor);
        free(destination_name);
        free(parent_path);
        return creation_status;
    }
    bool content_stable = posix_write_all(
            descriptor, (const uint8_t*)data, size) &&
        posix_flush_file(descriptor);
    struct stat temporary_identity;
#if !defined(__linux__)
    struct stat temporary_name_identity;
#endif
    bool temporary_bound = content_stable &&
        fstat(descriptor, &temporary_identity) == 0 &&
#if defined(__linux__)
        S_ISREG(temporary_identity.st_mode) &&
        temporary_identity.st_size >= 0 &&
        (uintmax_t)temporary_identity.st_size == (uintmax_t)size &&
        temporary_identity.st_nlink == 0U;
#else
        fstatat(parent_descriptor, temporary_name,
                &temporary_name_identity, AT_SYMLINK_NOFOLLOW) == 0 &&
        stat_same_identity(&temporary_identity, &temporary_name_identity);
#endif

    bool published = false;
    int publish_error = 0;
    if (temporary_bound) {
#if defined(__APPLE__)
        do {
            published = renameatx_np(
                parent_descriptor, temporary_name,
                parent_descriptor, destination_name,
                RENAME_EXCL) == 0;
        } while (!published && errno == EINTR);
#elif defined(__linux__)
#ifdef AT_EMPTY_PATH
        do {
            published = linkat(
                descriptor, "", parent_descriptor,
                destination_name, AT_EMPTY_PATH) == 0;
        } while (!published && errno == EINTR);
#else
        errno = ENOTSUP;
#endif
        if (!published && errno != EEXIST) {
            char descriptor_path[64];
            int path_size = snprintf(
                descriptor_path, sizeof(descriptor_path),
                "/proc/self/fd/%d", descriptor);
            if (path_size > 0 &&
                (size_t)path_size < sizeof(descriptor_path)) {
                do {
                    published = linkat(
                        AT_FDCWD, descriptor_path,
                        parent_descriptor, destination_name,
                        AT_SYMLINK_FOLLOW) == 0;
                } while (!published && errno == EINTR);
            } else {
                errno = ENAMETOOLONG;
            }
        }
#else
        do {
            published = linkat(
                parent_descriptor, temporary_name,
                parent_descriptor, destination_name, 0) == 0;
        } while (!published && errno == EINTR);
#endif
        if (!published) publish_error = errno;
    }
    bool destination_bound = false;
    if (published) {
        struct stat destination_identity;
        destination_bound =
            fstatat(parent_descriptor, destination_name,
                    &destination_identity, AT_SYMLINK_NOFOLLOW) == 0 &&
            stat_same_identity(&temporary_identity,
                               &destination_identity);
    }
    bool temporary_released = false;
#if defined(__APPLE__)
    if (published) {
        struct stat renamed_identity;
        temporary_released =
            fstat(descriptor, &renamed_identity) == 0 &&
            stat_same_identity(&temporary_identity, &renamed_identity) &&
            renamed_identity.st_nlink == 1U;
    } else {
        temporary_released = posix_unlink_held_temporary(
            parent_descriptor, temporary_name, descriptor, 0U);
    }
#elif defined(__linux__)
    if (!published) {
        /* The inode never had a namespace entry; closing the held descriptor
         * is its complete cleanup, including short writes and flush errors. */
        temporary_released = true;
    } else {
        struct stat released_identity;
        temporary_released =
            fstat(descriptor, &released_identity) == 0 &&
            stat_same_identity(&temporary_identity, &released_identity) &&
            released_identity.st_nlink == 1U;
    }
#else
    temporary_released = posix_unlink_held_temporary(
        parent_descriptor, temporary_name, descriptor,
        published ? 1U : 0U);
#endif
    bool directory_synced = temporary_released &&
        fsync(parent_descriptor) == 0;

    CommonFileStatus result = COMMON_FILE_IO_ERROR;
    if (published && destination_bound && directory_synced &&
        posix_namespace_destination_matches(
            root_descriptor, parent_path, &parent_identity,
            destination_name, &temporary_identity)) {
        result = COMMON_FILE_OK;
    } else if (!published && temporary_bound && publish_error == EEXIST &&
               directory_synced &&
               posix_namespace_parent_matches(
                   root_descriptor, parent_path, &parent_identity)) {
        result = COMMON_FILE_ALREADY_EXISTS;
    }
    if (close(descriptor) != 0) result = COMMON_FILE_IO_ERROR;
    if (close(parent_descriptor) != 0) result = COMMON_FILE_IO_ERROR;
    if (close(root_descriptor) != 0) result = COMMON_FILE_IO_ERROR;
    free(destination_name);
    free(parent_path);
    return result;
}

#endif
