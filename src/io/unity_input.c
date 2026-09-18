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

#include "io/unity_input.h"

#include "common/file_io.h"
#include "common/sha256.h"
#include "io/bundle_archive.h"
#include "io/serialized_file_prefix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum { UNITY_INPUT_PREFIX_SIZE = 96 };

static uint32_t load_be32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static bool has_archive_signature(const uint8_t* prefix, size_t prefix_size,
                                  const char* signature) {
    size_t size = strlen(signature) + 1U;
    return prefix_size >= size && memcmp(prefix, signature, size) == 0;
}

static bool has_serialized_header_marker(
    const uint8_t* prefix, size_t prefix_size, uint32_t* out_version) {
    if (prefix == NULL || prefix_size < 20U) {
        return false;
    }
    uint32_t version = load_be32(prefix + 8U);
    if (version == 0U || version > 100U) {
        return false;
    }
    /* Only v22 has an evidenced physical query. Preserve the prior marker
     * policy for other versions without interpreting a future header layout.
     * In v22 bytes16-19 are metadata high bits, not endian/reserved markers. */
    if (version != 22U &&
        (prefix[16U] > 1U || prefix[17U] != 0U || prefix[18U] != 0U || prefix[19U] != 0U)) {
        return false;
    }
    *out_version = version;
    return true;
}

UnityInputStatus unity_input_probe_bytes(
    const uint8_t* prefix, size_t prefix_size, uint64_t file_size, UnityInputProbe* out_probe) {
    if (!out_probe || (!prefix && prefix_size != 0U) || prefix_size > file_size) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->file_size = file_size;
    if (has_archive_signature(prefix, prefix_size, "UnityFS")) {
        out_probe->kind = UNITY_INPUT_KIND_UNITYFS;
        return UNITY_INPUT_OK;
    }
    if (has_archive_signature(prefix, prefix_size, "UnityRaw") ||
        has_archive_signature(prefix, prefix_size, "UnityWeb") ||
        has_archive_signature(prefix, prefix_size, "UnityArchive")) {
        out_probe->kind = UNITY_INPUT_KIND_UNSUPPORTED_UNITY_ARCHIVE;
        return UNITY_INPUT_UNSUPPORTED;
    }
    uint32_t version = 0U;
    if (!has_serialized_header_marker(prefix, prefix_size, &version)) {
        out_probe->kind = UNITY_INPUT_KIND_UNRELATED;
        return UNITY_INPUT_UNRELATED;
    }
    out_probe->serialized_file_version = version;
    out_probe->kind = UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE;
    if (version != 22U) {
        return UNITY_INPUT_UNSUPPORTED;
    }

    /* A recognizable v22 header with unsupported selector, missing mapped
     * header bytes or contradictory extents remains unsupported Unity data.
     * It must not become an ignored auxiliary resource during discovery. */
    SerializedFileHeaderView header;
    SerializedFilePrefixResult result = serialized_file_header_query(
        prefix, prefix_size, file_size, SERIALIZED_FILE_V22_HEADER_SIZE + 1U, &header);
    if (result.status != SERIALIZED_FILE_PREFIX_OK) {
        return UNITY_INPUT_UNSUPPORTED;
    }
    out_probe->kind = UNITY_INPUT_KIND_SERIALIZED_FILE_V22;
    return UNITY_INPUT_OK;
}

UnityInputStatus unity_input_probe_path(const char* path, UnityInputProbe* out_probe) {
    if (!path || !path[0] || !out_probe) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    uint8_t prefix[UNITY_INPUT_PREFIX_SIZE];
    size_t prefix_size = 0U;
    uint64_t file_size = 0U;
    CommonFileStatus status = common_file_read_prefix_regular(
        path, prefix, sizeof(prefix), &prefix_size, &file_size);
    if (status != COMMON_FILE_OK) return UNITY_INPUT_FILE_ERROR;
    return unity_input_probe_bytes(
        prefix, prefix_size, file_size, out_probe);
}

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION before;
    FILE_BASIC_INFO before_basic;
    FILE_ID_INFO before_id;
#else
    int descriptor;
    struct stat before;
#endif
    uint8_t snapshot_digest[COMMON_SHA256_DIGEST_SIZE];
    size_t snapshot_size;
    bool snapshot_bound;
} UnityInputIdentityAnchor;

typedef struct {
    CommonFileView view;
    bool view_open;
    UnityInputIdentityAnchor anchor;
    bool anchor_open;
    char* path;
    UnityInputProbe probe;
    BundleArchive archive;
    bool archive_open;
} UnityInputSnapshotImplementation;

static bool identity_anchor_bind_snapshot(
    UnityInputIdentityAnchor* anchor, const uint8_t* data, size_t size) {
    if (!anchor || (!data && size != 0U) || anchor->snapshot_bound)
        return false;
    common_sha256(data, size, anchor->snapshot_digest);
    anchor->snapshot_size = size;
    anchor->snapshot_bound = true;
    return true;
}

static bool identity_anchor_matches_snapshot(
    const UnityInputIdentityAnchor* anchor,
    const uint8_t* data,
    size_t size) {
    if (!anchor || !anchor->snapshot_bound ||
        anchor->snapshot_size != size || (!data && size != 0U)) {
        return false;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(data, size, digest);
    return memcmp(anchor->snapshot_digest, digest, sizeof(digest)) == 0;
}

#ifdef _WIN32
static void identity_anchor_init(UnityInputIdentityAnchor* anchor) {
    memset(anchor, 0, sizeof(*anchor));
    anchor->handle = INVALID_HANDLE_VALUE;
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

static bool windows_basic_file_info(
    HANDLE handle, FILE_BASIC_INFO* out_information) {
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

static bool windows_file_id_unchanged(
    const FILE_ID_INFO* before, const FILE_ID_INFO* after) {
    return before && after &&
        before->VolumeSerialNumber == after->VolumeSerialNumber &&
        memcmp(before->FileId.Identifier, after->FileId.Identifier,
               sizeof(before->FileId.Identifier)) == 0;
}

static bool windows_digest_exact(
    HANDLE handle, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    LARGE_INTEGER start;
    start.QuadPart = 0;
    if (handle == INVALID_HANDLE_VALUE ||
        !SetFilePointerEx(handle, start, NULL, FILE_BEGIN)) {
        return false;
    }
    uint8_t buffer[64U * 1024U];
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        const size_t remaining = size - position;
        const DWORD amount = remaining < sizeof(buffer)
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

static bool windows_basic_file_unchanged(
    const FILE_BASIC_INFO* before, const FILE_BASIC_INFO* after) {
    return before->CreationTime.QuadPart == after->CreationTime.QuadPart &&
        before->LastWriteTime.QuadPart == after->LastWriteTime.QuadPart &&
        before->ChangeTime.QuadPart == after->ChangeTime.QuadPart &&
        before->FileAttributes == after->FileAttributes;
}

static bool identity_anchor_open(
    UnityInputIdentityAnchor* anchor, const char* path) {
    identity_anchor_init(anchor);
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    HANDLE handle = CreateFileW(
        wide_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_RANDOM_ACCESS, NULL);
    free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) return false;
    if (!windows_regular_file_info(handle, &anchor->before) ||
        !windows_basic_file_info(handle, &anchor->before_basic) ||
        !windows_file_id_info(handle, &anchor->before_id)) {
        (void)CloseHandle(handle);
        return false;
    }
    anchor->handle = handle;
    return true;
}

static bool identity_anchor_validate(
    const UnityInputIdentityAnchor* anchor, const char* path) {
    if (!anchor || anchor->handle == INVALID_HANDLE_VALUE || !path) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION held_after;
    FILE_BASIC_INFO held_basic_after;
    FILE_ID_INFO held_id_after;
    uint8_t held_digest[COMMON_SHA256_DIGEST_SIZE];
    if (!windows_regular_file_info(anchor->handle, &held_after) ||
        !windows_basic_file_info(anchor->handle, &held_basic_after) ||
        !windows_file_id_info(anchor->handle, &held_id_after) ||
        !anchor->snapshot_bound ||
        !windows_digest_exact(
            anchor->handle, anchor->snapshot_size, held_digest) ||
        memcmp(anchor->snapshot_digest, held_digest,
               sizeof(held_digest)) != 0 ||
        !windows_file_unchanged(&anchor->before, &held_after) ||
        !windows_basic_file_unchanged(
            &anchor->before_basic, &held_basic_after) ||
        !windows_file_id_unchanged(
            &anchor->before_id, &held_id_after)) {
        return false;
    }
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    HANDLE path_handle = CreateFileW(
        wide_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide_path);
    BY_HANDLE_FILE_INFORMATION path_after;
    FILE_BASIC_INFO path_basic_after;
    FILE_ID_INFO path_id_after;
    bool unchanged = path_handle != INVALID_HANDLE_VALUE &&
        windows_regular_file_info(path_handle, &path_after) &&
        windows_basic_file_info(path_handle, &path_basic_after) &&
        windows_file_id_info(path_handle, &path_id_after) &&
        windows_file_unchanged(&held_after, &path_after) &&
        windows_basic_file_unchanged(
            &held_basic_after, &path_basic_after) &&
        windows_file_id_unchanged(&held_id_after, &path_id_after);
    if (path_handle != INVALID_HANDLE_VALUE &&
        !CloseHandle(path_handle)) {
        unchanged = false;
    }
    return unchanged;
}

static bool identity_anchor_close(
    UnityInputIdentityAnchor* anchor, const char* path) {
    bool unchanged = identity_anchor_validate(anchor, path);
    if (anchor->handle != INVALID_HANDLE_VALUE &&
        !CloseHandle(anchor->handle)) {
        unchanged = false;
    }
    identity_anchor_init(anchor);
    return unchanged;
}
#else
static void identity_anchor_init(UnityInputIdentityAnchor* anchor) {
    memset(anchor, 0, sizeof(*anchor));
    anchor->descriptor = -1;
}

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

static bool stat_file_unchanged(const struct stat* before,
                                const struct stat* after) {
    return S_ISREG(before->st_mode) && S_ISREG(after->st_mode) &&
        before->st_dev == after->st_dev &&
        before->st_ino == after->st_ino &&
        before->st_size == after->st_size &&
        before->st_mode == after->st_mode &&
        before->st_mtime == after->st_mtime &&
        stat_mtime_nanoseconds(before) == stat_mtime_nanoseconds(after) &&
        before->st_ctime == after->st_ctime &&
        stat_ctime_nanoseconds(before) == stat_ctime_nanoseconds(after);
}

static bool posix_digest_exact(
    int descriptor, size_t size,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    uint8_t buffer[64U * 1024U];
    CommonSha256Context hash;
    common_sha256_init(&hash);
    size_t position = 0U;
    while (position < size) {
        const size_t remaining = size - position;
        const size_t amount = remaining < sizeof(buffer)
            ? remaining : sizeof(buffer);
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

static bool identity_anchor_open(
    UnityInputIdentityAnchor* anchor, const char* path) {
    identity_anchor_init(anchor);
    struct stat path_before;
    if (lstat(path, &path_before) != 0 ||
        !S_ISREG(path_before.st_mode)) {
        return false;
    }
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags);
    if (descriptor < 0) return false;
    struct stat held_before;
    if (fstat(descriptor, &held_before) != 0 ||
        !stat_file_unchanged(&path_before, &held_before)) {
        (void)close(descriptor);
        return false;
    }
    anchor->descriptor = descriptor;
    anchor->before = held_before;
    return true;
}

static bool identity_anchor_validate(
    const UnityInputIdentityAnchor* anchor, const char* path) {
    if (!anchor || anchor->descriptor < 0 || !path) return false;
    struct stat held_after;
    struct stat path_after;
    uint8_t held_digest[COMMON_SHA256_DIGEST_SIZE];
    return fstat(anchor->descriptor, &held_after) == 0 &&
        lstat(path, &path_after) == 0 &&
        anchor->snapshot_bound &&
        posix_digest_exact(
            anchor->descriptor, anchor->snapshot_size, held_digest) &&
        memcmp(anchor->snapshot_digest, held_digest,
               sizeof(held_digest)) == 0 &&
        stat_file_unchanged(&anchor->before, &held_after) &&
        stat_file_unchanged(&held_after, &path_after);
}

static bool identity_anchor_close(
    UnityInputIdentityAnchor* anchor, const char* path) {
    bool unchanged = identity_anchor_validate(anchor, path);
    if (anchor->descriptor >= 0 && close(anchor->descriptor) != 0) {
        unchanged = false;
    }
    identity_anchor_init(anchor);
    return unchanged;
}
#endif

#ifndef _WIN32
static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}
#endif

static char* stable_absolute_path(const char* path) {
    if (!path || !path[0]) return NULL;
#ifdef _WIN32
    wchar_t* requested = common_windows_utf8_to_wide(path);
    if (!requested) return NULL;
    DWORD required = GetFullPathNameW(requested, 0U, NULL, NULL);
    if (required == 0U) {
        free(requested);
        return NULL;
    }
    wchar_t* absolute = (wchar_t*)malloc(
        (size_t)required * sizeof(*absolute));
    if (!absolute) {
        free(requested);
        return NULL;
    }
    DWORD written = GetFullPathNameW(
        requested, required, absolute, NULL);
    free(requested);
    if (written == 0U || written >= required) {
        free(absolute);
        return NULL;
    }
    char* result = common_windows_wide_to_utf8(absolute);
    free(absolute);
    return result;
#else
    if (path[0] == '/') return duplicate_string(path);
    size_t capacity = 256U;
    char* current = NULL;
    for (;;) {
        current = (char*)malloc(capacity);
        if (!current) return NULL;
        errno = 0;
        if (getcwd(current, capacity)) break;
        int error = errno;
        free(current);
        current = NULL;
        if (error != ERANGE || capacity > SIZE_MAX / 2U) return NULL;
        capacity *= 2U;
    }
    const size_t current_size = strlen(current);
    const size_t path_size = strlen(path);
    const bool separator = current_size == 0U ||
        current[current_size - 1U] != '/';
    if (current_size > SIZE_MAX - path_size -
            (separator ? 2U : 1U)) {
        free(current);
        return NULL;
    }
    char* absolute = (char*)malloc(
        current_size + (separator ? 1U : 0U) + path_size + 1U);
    if (!absolute) {
        free(current);
        return NULL;
    }
    memcpy(absolute, current, current_size);
    size_t at = current_size;
    if (separator) absolute[at++] = '/';
    memcpy(absolute + at, path, path_size + 1U);
    free(current);
    return absolute;
#endif
}

static UnityInputStatus dispose_snapshot_implementation(
    UnityInputSnapshotImplementation* implementation,
    UnityInputStatus status) {
    if (!implementation) return status;
    bool stable = true;
    if (implementation->archive_open) {
        bundle_close(&implementation->archive);
        implementation->archive_open = false;
    }
    if (implementation->view_open) {
        if (common_file_view_close(&implementation->view) !=
            COMMON_FILE_OK) {
            stable = false;
        }
        implementation->view_open = false;
    }
    if (implementation->anchor_open) {
        if (!identity_anchor_close(
                &implementation->anchor, implementation->path)) {
            stable = false;
        }
        implementation->anchor_open = false;
    }
    free(implementation->path);
    free(implementation);
    return stable ? status : UNITY_INPUT_FILE_ERROR;
}

static UnityInputStatus ensure_snapshot_mapping(
    UnityInputSnapshotImplementation* implementation) {
    if (!implementation || !implementation->anchor_open) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    if (!implementation->view_open) {
        if (common_file_view_open_regular(
                implementation->path, SIZE_MAX,
                &implementation->view) != COMMON_FILE_OK) {
            return UNITY_INPUT_FILE_ERROR;
        }
        implementation->view_open = true;
    }
    if (!identity_anchor_validate(
            &implementation->anchor, implementation->path) ||
        !identity_anchor_matches_snapshot(
            &implementation->anchor, implementation->view.data,
            implementation->view.size)) {
        (void)common_file_view_close(&implementation->view);
        implementation->view_open = false;
        return UNITY_INPUT_FILE_ERROR;
    }
    UnityInputProbe probe;
    UnityInputStatus status = unity_input_probe_bytes(
        implementation->view.data,
        implementation->view.size < UNITY_INPUT_PREFIX_SIZE
            ? implementation->view.size : UNITY_INPUT_PREFIX_SIZE,
        implementation->view.size, &probe);
    if (status != UNITY_INPUT_OK ||
        probe.kind != implementation->probe.kind ||
        probe.file_size != implementation->probe.file_size) {
        (void)common_file_view_close(&implementation->view);
        implementation->view_open = false;
        return UNITY_INPUT_FILE_ERROR;
    }
    return UNITY_INPUT_OK;
}

static UnityInputStatus visit_bundle(
    const UnityInputSnapshotImplementation* implementation,
    UnitySerializedSourceVisitor visitor, void* context,
    UnityInputVisitStats* stats) {
    const BundleArchive* archive = &implementation->archive;
    UnityInputStatus result = UNITY_INPUT_OK;
    for (int i = 0; i < archive->directory_count; ++i) {
        const BundleDirectoryInfo* member = &archive->directories[i];
        BundleMemberKind kind = bundle_member_classify(member);
        const uint8_t* member_data = NULL;
        size_t member_size = 0U;
        if (!bundle_get_member_view(
                archive, (size_t)i, &member_data, &member_size)) {
            result = UNITY_INPUT_MEMBER_INVALID;
            break;
        }
        if (kind == BUNDLE_MEMBER_RESOURCE) {
            ++stats->resource_members;
            continue;
        }
        if (kind == BUNDLE_MEMBER_DIRECTORY) {
            ++stats->directory_members;
            if (member_size != 0U) {
                result = UNITY_INPUT_MEMBER_INVALID;
                break;
            }
            continue;
        }
        if (kind == BUNDLE_MEMBER_DELETED) {
            ++stats->deleted_members;
            if (member_size != 0U) {
                result = UNITY_INPUT_MEMBER_INVALID;
                break;
            }
            continue;
        }
        if (kind != BUNDLE_MEMBER_SERIALIZED_FILE || !member_data ||
            member_size == 0U) {
            result = UNITY_INPUT_MEMBER_INVALID;
            break;
        }
        UnityInputProbe probe;
        UnityInputStatus probe_status = unity_input_probe_bytes(
            member_data,
            member_size < UNITY_INPUT_PREFIX_SIZE
                ? member_size : UNITY_INPUT_PREFIX_SIZE,
            member_size, &probe);
        if (probe_status != UNITY_INPUT_OK ||
            probe.kind != UNITY_INPUT_KIND_SERIALIZED_FILE_V22) {
            result = probe_status == UNITY_INPUT_UNSUPPORTED
                ? UNITY_INPUT_UNSUPPORTED : UNITY_INPUT_MEMBER_INVALID;
            break;
        }
        UnitySerializedSource source = {
            implementation->path, member->name, (size_t)i, true,
            member_data, member_size
        };
        if (!visitor(&source, context)) {
            result = UNITY_INPUT_VISITOR_FAILED;
            break;
        }
        ++stats->serialized_files;
    }
    return result;
}

void unity_input_snapshot_init(UnityInputSnapshot* snapshot) {
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
}

UnityInputStatus unity_input_snapshot_open(
    const char* path, UnityInputSnapshot* snapshot) {
    if (!path || !path[0] || !snapshot || snapshot->implementation) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    UnityInputSnapshotImplementation* implementation =
        (UnityInputSnapshotImplementation*)calloc(
            1U, sizeof(*implementation));
    if (!implementation) return UNITY_INPUT_FILE_ERROR;
    identity_anchor_init(&implementation->anchor);
    implementation->path = stable_absolute_path(path);
    if (!implementation->path) {
        free(implementation);
        return UNITY_INPUT_FILE_ERROR;
    }
    if (!identity_anchor_open(
            &implementation->anchor, implementation->path)) {
        free(implementation->path);
        free(implementation);
        return UNITY_INPUT_FILE_ERROR;
    }
    implementation->anchor_open = true;
    if (common_file_view_open_regular(
            implementation->path, SIZE_MAX,
            &implementation->view) != COMMON_FILE_OK) {
        return dispose_snapshot_implementation(
            implementation, UNITY_INPUT_FILE_ERROR);
    }
    implementation->view_open = true;
    if (!identity_anchor_bind_snapshot(
            &implementation->anchor, implementation->view.data,
            implementation->view.size) ||
        !identity_anchor_validate(
            &implementation->anchor, implementation->path)) {
        return dispose_snapshot_implementation(
            implementation, UNITY_INPUT_FILE_ERROR);
    }
    UnityInputProbe probe;
    UnityInputStatus status = unity_input_probe_bytes(
        implementation->view.data,
        implementation->view.size < UNITY_INPUT_PREFIX_SIZE
            ? implementation->view.size : UNITY_INPUT_PREFIX_SIZE,
        implementation->view.size, &probe);
    if (status != UNITY_INPUT_OK ||
        (probe.kind != UNITY_INPUT_KIND_SERIALIZED_FILE_V22 &&
         probe.kind != UNITY_INPUT_KIND_UNITYFS)) {
        return dispose_snapshot_implementation(implementation, status);
    }
    implementation->probe = probe;
    if (probe.kind == UNITY_INPUT_KIND_UNITYFS) {
        if (!bundle_open(&implementation->archive,
                         implementation->view.data,
                         implementation->view.size)) {
            bundle_close(&implementation->archive);
            return dispose_snapshot_implementation(
                implementation, UNITY_INPUT_CONTAINER_INVALID);
        }
        implementation->archive_open = true;
    }
    snapshot->implementation = implementation;
    return UNITY_INPUT_OK;
}

bool unity_input_snapshot_is_open(const UnityInputSnapshot* snapshot) {
    return snapshot && snapshot->implementation;
}

const char* unity_input_snapshot_path(const UnityInputSnapshot* snapshot) {
    if (!snapshot || !snapshot->implementation) return NULL;
    const UnityInputSnapshotImplementation* implementation =
        (const UnityInputSnapshotImplementation*)snapshot->implementation;
    return implementation->path;
}

UnityInputStatus unity_input_snapshot_visit(
    UnityInputSnapshot* snapshot, UnitySerializedSourceVisitor visitor,
    void* context, UnityInputVisitStats* out_stats) {
    if (!snapshot || !snapshot->implementation || !visitor || !out_stats) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    UnityInputSnapshotImplementation* implementation =
        (UnityInputSnapshotImplementation*)snapshot->implementation;
    if (!identity_anchor_validate(
            &implementation->anchor, implementation->path)) {
        return UNITY_INPUT_FILE_ERROR;
    }
    if (implementation->probe.kind == UNITY_INPUT_KIND_UNITYFS) {
        return visit_bundle(implementation, visitor, context, out_stats);
    }
    if (implementation->probe.kind !=
        UNITY_INPUT_KIND_SERIALIZED_FILE_V22) {
        return UNITY_INPUT_UNRELATED;
    }
    UnityInputStatus mapping_status = ensure_snapshot_mapping(implementation);
    if (mapping_status != UNITY_INPUT_OK) return mapping_status;
    UnitySerializedSource source = {
        implementation->path, NULL, 0U, false,
        implementation->view.data, implementation->view.size
    };
    if (!visitor(&source, context)) return UNITY_INPUT_VISITOR_FAILED;
    out_stats->serialized_files = 1U;
    return UNITY_INPUT_OK;
}

UnityInputStatus unity_input_snapshot_suspend_mapping(
    UnityInputSnapshot* snapshot) {
    if (!snapshot || !snapshot->implementation) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    UnityInputSnapshotImplementation* implementation =
        (UnityInputSnapshotImplementation*)snapshot->implementation;
    if (implementation->view_open) {
        CommonFileStatus close_status = common_file_view_close(
            &implementation->view);
        implementation->view_open = false;
        if (close_status != COMMON_FILE_OK) {
            return UNITY_INPUT_FILE_ERROR;
        }
    }
    return identity_anchor_validate(
        &implementation->anchor, implementation->path)
        ? UNITY_INPUT_OK : UNITY_INPUT_FILE_ERROR;
}

UnityInputStatus unity_input_snapshot_close(UnityInputSnapshot* snapshot) {
    if (!snapshot || !snapshot->implementation) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    UnityInputSnapshotImplementation* implementation =
        (UnityInputSnapshotImplementation*)snapshot->implementation;
    snapshot->implementation = NULL;
    return dispose_snapshot_implementation(
        implementation, UNITY_INPUT_OK);
}

UnityInputStatus unity_input_visit_serialized(
    const char* path, UnitySerializedSourceVisitor visitor, void* context,
    UnityInputVisitStats* out_stats) {
    if (!path || !path[0] || !visitor || !out_stats) {
        return UNITY_INPUT_INVALID_ARGUMENT;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    UnityInputSnapshot snapshot;
    unity_input_snapshot_init(&snapshot);
    UnityInputStatus status = unity_input_snapshot_open(path, &snapshot);
    if (status != UNITY_INPUT_OK) return status;
    UnityInputVisitStats pending_stats;
    status = unity_input_snapshot_visit(
        &snapshot, visitor, context, &pending_stats);
    UnityInputStatus close_status = unity_input_snapshot_close(&snapshot);
    if (close_status != UNITY_INPUT_OK) return close_status;
    *out_stats = pending_stats;
    return status;
}

const char* unity_input_kind_name(UnityInputKind kind) {
    switch (kind) {
        case UNITY_INPUT_KIND_UNRELATED: return "unrelated";
        case UNITY_INPUT_KIND_SERIALIZED_FILE_V22:
            return "serialized-file-v22";
        case UNITY_INPUT_KIND_UNITYFS: return "unityfs";
        case UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE:
            return "unsupported-serialized-file";
        case UNITY_INPUT_KIND_UNSUPPORTED_UNITY_ARCHIVE:
            return "unsupported-unity-archive";
        default: return "unknown";
    }
}

const char* unity_input_status_name(UnityInputStatus status) {
    switch (status) {
        case UNITY_INPUT_OK: return "ok";
        case UNITY_INPUT_INVALID_ARGUMENT: return "invalid-argument";
        case UNITY_INPUT_FILE_ERROR: return "file-error";
        case UNITY_INPUT_UNRELATED: return "unrelated";
        case UNITY_INPUT_UNSUPPORTED: return "unsupported";
        case UNITY_INPUT_CONTAINER_INVALID: return "container-invalid";
        case UNITY_INPUT_MEMBER_INVALID: return "member-invalid";
        case UNITY_INPUT_VISITOR_FAILED: return "visitor-failed";
        default: return "unknown";
    }
}
