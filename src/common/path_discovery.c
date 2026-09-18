// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/path_discovery.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef enum {
    PATH_NODE_REGULAR = 0,
    PATH_NODE_DIRECTORY,
    PATH_NODE_LINK,
    PATH_NODE_OTHER,
} PathNodeKind;

typedef struct {
    /* Zero is empty; stored entry indices are encoded as index + 1. */
    size_t* slots;
    size_t capacity;
} ExactPathIndex;

typedef struct {
    CommonDiscoveredPath* paths;
    size_t count;
    size_t capacity;
    ExactPathIndex index;
} PathBuilder;

typedef struct {
    char* path;
    size_t depth;
} QueuedDirectory;

typedef struct {
    QueuedDirectory* entries;
    size_t count;
    size_t capacity;
    size_t next;
    ExactPathIndex index;
} DirectoryQueue;

static bool size_mul_overflows(size_t left, size_t right) {
    return left != 0U && right > SIZE_MAX / left;
}

typedef const char* (*IndexedPathGetter)(const void* owner, size_t index);

static uint64_t exact_path_hash(const char* path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*path) {
        hash ^= (uint8_t)*path++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t exact_path_index_find(
    const ExactPathIndex* index, const char* path, const void* owner,
    IndexedPathGetter get_path) {
    if (!index || !index->slots || index->capacity == 0U || !path ||
        !owner || !get_path) {
        return SIZE_MAX;
    }
    size_t slot = (size_t)exact_path_hash(path) & (index->capacity - 1U);
    for (size_t probes = 0U; probes < index->capacity; ++probes) {
        size_t encoded = index->slots[slot];
        if (encoded == 0U) return SIZE_MAX;
        size_t entry = encoded - 1U;
        if (strcmp(get_path(owner, entry), path) == 0) return entry;
        slot = (slot + 1U) & (index->capacity - 1U);
    }
    return SIZE_MAX;
}

static bool exact_path_index_insert(
    ExactPathIndex* index, const char* path, size_t entry) {
    if (!index || !index->slots || index->capacity == 0U || !path ||
        entry == SIZE_MAX) {
        return false;
    }
    size_t slot = (size_t)exact_path_hash(path) & (index->capacity - 1U);
    for (size_t probes = 0U; probes < index->capacity; ++probes) {
        if (index->slots[slot] == 0U) {
            index->slots[slot] = entry + 1U;
            return true;
        }
        slot = (slot + 1U) & (index->capacity - 1U);
    }
    return false;
}

static bool exact_path_index_reserve(
    ExactPathIndex* index, size_t needed, size_t existing_count,
    const void* owner, IndexedPathGetter get_path) {
    if (!index || !owner || !get_path || needed == SIZE_MAX ||
        existing_count > needed ||
        ((index->capacity == 0U) != (index->slots == NULL))) {
        return false;
    }
    if (index->capacity != 0U && needed <= index->capacity / 2U) {
        return true;
    }
    size_t capacity = index->capacity == 0U ? 16U : index->capacity;
    while (needed > capacity / 2U) {
        if (capacity > SIZE_MAX / 2U) return false;
        capacity *= 2U;
    }
    if (size_mul_overflows(capacity, sizeof(*index->slots))) return false;
    size_t* slots = (size_t*)calloc(capacity, sizeof(*slots));
    if (!slots) return false;
    ExactPathIndex replacement = {slots, capacity};
    for (size_t entry = 0U; entry < existing_count; ++entry) {
        if (!exact_path_index_insert(
                &replacement, get_path(owner, entry), entry)) {
            free(slots);
            return false;
        }
    }
    free(index->slots);
    *index = replacement;
    return true;
}

static const char* builder_path_at(const void* owner, size_t index) {
    const PathBuilder* builder = (const PathBuilder*)owner;
    return builder->paths[index].path;
}

static const char* directory_path_at(const void* owner, size_t index) {
    const DirectoryQueue* queue = (const DirectoryQueue*)owner;
    return queue->entries[index].path;
}

static char* duplicate_path(const char* path) {
    if (!path) return NULL;
    size_t length = strlen(path);
    if (length == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, path, length + 1U);
    return copy;
}

static bool reserve_files(PathBuilder* builder, size_t needed) {
    if (needed <= builder->capacity) return true;
    size_t capacity = builder->capacity == 0U ? 32U : builder->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < needed ||
        size_mul_overflows(capacity, sizeof(*builder->paths))) {
        return false;
    }
    CommonDiscoveredPath* grown = (CommonDiscoveredPath*)realloc(
        builder->paths, capacity * sizeof(*builder->paths));
    if (!grown) return false;
    builder->paths = grown;
    builder->capacity = capacity;
    return true;
}

static CommonPathDiscoveryStatus add_file(
    PathBuilder* builder, const char* path, bool explicit_file,
    size_t max_files) {
    if (!builder || !path) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }
    size_t existing = exact_path_index_find(
        &builder->index, path, builder, builder_path_at);
    if (existing != SIZE_MAX) {
        builder->paths[existing].explicit_file =
            builder->paths[existing].explicit_file || explicit_file;
        return COMMON_PATH_DISCOVERY_OK;
    }
    if (max_files != 0U && builder->count >= max_files) {
        return COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED;
    }
    if (builder->count == SIZE_MAX ||
        !exact_path_index_reserve(
            &builder->index, builder->count + 1U, builder->count,
            builder, builder_path_at) ||
        !reserve_files(builder, builder->count + 1U)) {
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    char* copy = duplicate_path(path);
    if (!copy) return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    builder->paths[builder->count].path = copy;
    builder->paths[builder->count].explicit_file = explicit_file;
    if (!exact_path_index_insert(
            &builder->index, copy, builder->count)) {
        free(copy);
        builder->paths[builder->count].path = NULL;
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    builder->count++;
    return COMMON_PATH_DISCOVERY_OK;
}

static void dispose_builder(PathBuilder* builder) {
    if (!builder) return;
    for (size_t index = 0U; index < builder->count; ++index) {
        free(builder->paths[index].path);
    }
    free(builder->paths);
    free(builder->index.slots);
    memset(builder, 0, sizeof(*builder));
}

static bool reserve_directories(DirectoryQueue* queue, size_t needed) {
    if (needed <= queue->capacity) return true;
    size_t capacity = queue->capacity == 0U ? 16U : queue->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < needed ||
        size_mul_overflows(capacity, sizeof(*queue->entries))) {
        return false;
    }
    QueuedDirectory* grown = (QueuedDirectory*)realloc(
        queue->entries, capacity * sizeof(*queue->entries));
    if (!grown) return false;
    queue->entries = grown;
    queue->capacity = capacity;
    return true;
}

static size_t queued_directory_index(const DirectoryQueue* queue,
                                     const char* path) {
    return exact_path_index_find(
        &queue->index, path, queue, directory_path_at);
}

static CommonPathDiscoveryStatus queue_directory(
    DirectoryQueue* queue, const char* path, size_t depth,
    size_t max_directories, size_t max_depth) {
    if (!queue || !path) return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    size_t existing = queued_directory_index(queue, path);
    if (existing != SIZE_MAX) {
        if (depth < queue->entries[existing].depth) {
            queue->entries[existing].depth = depth;
        }
        return COMMON_PATH_DISCOVERY_OK;
    }
    if (max_depth != 0U && depth > max_depth) {
        return COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED;
    }
    if (max_directories != 0U && queue->count >= max_directories) {
        return COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED;
    }
    if (queue->count == SIZE_MAX ||
        !exact_path_index_reserve(
            &queue->index, queue->count + 1U, queue->count,
            queue, directory_path_at) ||
        !reserve_directories(queue, queue->count + 1U)) {
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    char* copy = duplicate_path(path);
    if (!copy) return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    queue->entries[queue->count].path = copy;
    queue->entries[queue->count].depth = depth;
    if (!exact_path_index_insert(&queue->index, copy, queue->count)) {
        free(copy);
        queue->entries[queue->count].path = NULL;
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    queue->count++;
    return COMMON_PATH_DISCOVERY_OK;
}

static void dispose_directory_queue(DirectoryQueue* queue) {
    if (!queue) return;
    for (size_t index = 0U; index < queue->count; ++index) {
        free(queue->entries[index].path);
    }
    free(queue->entries);
    free(queue->index.slots);
    memset(queue, 0, sizeof(*queue));
}

static bool is_path_separator(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    /* A backslash is an ordinary POSIX filename byte. */
    return value == '/';
#endif
}

static CommonPathDiscoveryStatus join_path(
    const char* directory, const char* name, size_t max_path_bytes,
    char** out_path) {
    if (!directory || !name || !out_path) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }
    *out_path = NULL;
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool needs_separator = directory_length != 0U &&
        !is_path_separator(directory[directory_length - 1U]);
    size_t separator_length = needs_separator ? 1U : 0U;
    if (directory_length > SIZE_MAX - separator_length ||
        directory_length + separator_length > SIZE_MAX - name_length ||
        directory_length + separator_length + name_length == SIZE_MAX) {
        return COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED;
    }
    size_t total = directory_length + separator_length + name_length;
    if (max_path_bytes != 0U && total > max_path_bytes) {
        return COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED;
    }
    char* joined = (char*)malloc(total + 1U);
    if (!joined) return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    memcpy(joined, directory, directory_length);
    if (needs_separator) {
#ifdef _WIN32
        joined[directory_length] = '\\';
#else
        joined[directory_length] = '/';
#endif
    }
    memcpy(joined + directory_length + separator_length, name,
           name_length + 1U);
    *out_path = joined;
    return COMMON_PATH_DISCOVERY_OK;
}

#ifdef _WIN32

static CommonPathDiscoveryStatus windows_conversion_failure(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) {
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    if (error == ERROR_NO_UNICODE_TRANSLATION ||
        error == ERROR_INVALID_PARAMETER) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }
    return COMMON_PATH_DISCOVERY_IO_ERROR;
}

static CommonPathDiscoveryStatus query_path_kind(const char* path,
                                                 PathNodeKind* kind) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return windows_conversion_failure(GetLastError());
    DWORD attributes = GetFileAttributesW(wide_path);
    DWORD error = attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    free(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return COMMON_PATH_DISCOVERY_NOT_FOUND;
        }
        return COMMON_PATH_DISCOVERY_IO_ERROR;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        *kind = PATH_NODE_LINK;
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        *kind = PATH_NODE_DIRECTORY;
    } else if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0U) {
        *kind = PATH_NODE_OTHER;
    } else {
        *kind = PATH_NODE_REGULAR;
    }
    return COMMON_PATH_DISCOVERY_OK;
}

static CommonPathDiscoveryStatus scan_directory(
    const QueuedDirectory* directory,
    const CommonPathDiscoveryOptions* options,
    DirectoryQueue* queue, PathBuilder* files) {
    char* pattern = NULL;
    CommonPathDiscoveryStatus status = join_path(
        directory->path, "*", 0U, &pattern);
    if (status != COMMON_PATH_DISCOVERY_OK) return status;
    wchar_t* wide_pattern = common_windows_utf8_to_wide(pattern);
    DWORD conversion_error = wide_pattern ? ERROR_SUCCESS : GetLastError();
    free(pattern);
    if (!wide_pattern) return windows_conversion_failure(conversion_error);

    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(wide_pattern, &data);
    free(wide_pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return COMMON_PATH_DISCOVERY_OK;
        }
        return error == ERROR_PATH_NOT_FOUND
            ? COMMON_PATH_DISCOVERY_NOT_FOUND
            : COMMON_PATH_DISCOVERY_IO_ERROR;
    }

    status = COMMON_PATH_DISCOVERY_OK;
    for (;;) {
        if (wcscmp(data.cFileName, L".") != 0 &&
            wcscmp(data.cFileName, L"..") != 0) {
            char* name = common_windows_wide_to_utf8(data.cFileName);
            if (!name) {
                status = windows_conversion_failure(GetLastError());
                break;
            }
            char* child = NULL;
            status = join_path(directory->path, name,
                               options->max_path_bytes, &child);
            free(name);
            if (status != COMMON_PATH_DISCOVERY_OK) break;
            PathNodeKind kind;
            status = query_path_kind(child, &kind);
            if (status == COMMON_PATH_DISCOVERY_OK) {
                if (kind == PATH_NODE_REGULAR) {
                    status = add_file(
                        files, child, false, options->max_files);
                } else if (kind == PATH_NODE_DIRECTORY &&
                           options->recursive) {
                    if (directory->depth == SIZE_MAX) {
                        status =
                            COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED;
                    } else {
                        status = queue_directory(
                            queue, child, directory->depth + 1U,
                            options->max_directories,
                            options->max_depth);
                    }
                } else if (kind == PATH_NODE_LINK &&
                           options->reject_descendant_links) {
                    status = COMMON_PATH_DISCOVERY_LINK_REJECTED;
                } else if (kind == PATH_NODE_OTHER &&
                           options->reject_descendant_unsupported_nodes) {
                    status = COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE;
                }
                /* Links/reparse points and other special descendants are
                 * ignored in permissive mode and are never traversed. */
            }
            free(child);
            if (status != COMMON_PATH_DISCOVERY_OK) break;
        }

        if (!FindNextFileW(search, &data)) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                status = COMMON_PATH_DISCOVERY_IO_ERROR;
            }
            break;
        }
    }
    if (!FindClose(search) && status == COMMON_PATH_DISCOVERY_OK) {
        status = COMMON_PATH_DISCOVERY_IO_ERROR;
    }
    return status;
}

#else

static CommonPathDiscoveryStatus map_path_error(int error) {
    if (error == ENOENT || error == ENOTDIR) {
        return COMMON_PATH_DISCOVERY_NOT_FOUND;
    }
    return COMMON_PATH_DISCOVERY_IO_ERROR;
}

static CommonPathDiscoveryStatus query_directory_entry_kind(
    int descriptor, const char* name, PathNodeKind* kind) {
    struct stat information;
    if (fstatat(descriptor, name, &information, AT_SYMLINK_NOFOLLOW) != 0) {
        return map_path_error(errno);
    }
    if (S_ISLNK(information.st_mode)) {
        *kind = PATH_NODE_LINK;
    } else if (S_ISDIR(information.st_mode)) {
        *kind = PATH_NODE_DIRECTORY;
    } else if (S_ISREG(information.st_mode)) {
        *kind = PATH_NODE_REGULAR;
    } else {
        *kind = PATH_NODE_OTHER;
    }
    return COMMON_PATH_DISCOVERY_OK;
}

static CommonPathDiscoveryStatus query_path_kind(const char* path,
                                                 PathNodeKind* kind) {
    struct stat information;
    if (lstat(path, &information) != 0) return map_path_error(errno);
    if (S_ISLNK(information.st_mode)) {
        *kind = PATH_NODE_LINK;
    } else if (S_ISDIR(information.st_mode)) {
        *kind = PATH_NODE_DIRECTORY;
    } else if (S_ISREG(information.st_mode)) {
        *kind = PATH_NODE_REGULAR;
    } else {
        *kind = PATH_NODE_OTHER;
    }
    return COMMON_PATH_DISCOVERY_OK;
}

static CommonPathDiscoveryStatus scan_directory(
    const QueuedDirectory* directory,
    const CommonPathDiscoveryOptions* options,
    DirectoryQueue* queue, PathBuilder* files) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(directory->path, flags);
    if (descriptor < 0) {
#ifdef ELOOP
        if (errno == ELOOP) return COMMON_PATH_DISCOVERY_LINK_REJECTED;
#endif
        return map_path_error(errno);
    }
    DIR* stream = fdopendir(descriptor);
    if (!stream) {
        int saved_error = errno;
        (void)close(descriptor);
        return map_path_error(saved_error);
    }

    CommonPathDiscoveryStatus status = COMMON_PATH_DISCOVERY_OK;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(stream);
        if (!entry) {
            if (errno != 0) status = COMMON_PATH_DISCOVERY_IO_ERROR;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char* child = NULL;
        status = join_path(directory->path, entry->d_name,
                           options->max_path_bytes, &child);
        if (status != COMMON_PATH_DISCOVERY_OK) break;
        PathNodeKind kind;
        status = query_directory_entry_kind(
            dirfd(stream), entry->d_name, &kind);
        if (status == COMMON_PATH_DISCOVERY_OK) {
            if (kind == PATH_NODE_REGULAR) {
                status = add_file(
                    files, child, false, options->max_files);
            } else if (kind == PATH_NODE_DIRECTORY &&
                       options->recursive) {
                if (directory->depth == SIZE_MAX) {
                    status = COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED;
                } else {
                    status = queue_directory(
                        queue, child, directory->depth + 1U,
                        options->max_directories, options->max_depth);
                }
            } else if (kind == PATH_NODE_LINK &&
                       options->reject_descendant_links) {
                status = COMMON_PATH_DISCOVERY_LINK_REJECTED;
            } else if (kind == PATH_NODE_OTHER &&
                       options->reject_descendant_unsupported_nodes) {
                status = COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE;
            }
            /* Links and other special descendants are ignored in permissive
             * mode and are never traversed. */
        }
        free(child);
        if (status != COMMON_PATH_DISCOVERY_OK) break;
    }
    if (closedir(stream) != 0 && status == COMMON_PATH_DISCOVERY_OK) {
        status = COMMON_PATH_DISCOVERY_IO_ERROR;
    }
    return status;
}

#endif

static int compare_discovered_paths(const void* left, const void* right) {
    const CommonDiscoveredPath* a = (const CommonDiscoveredPath*)left;
    const CommonDiscoveredPath* b = (const CommonDiscoveredPath*)right;
    return strcmp(a->path, b->path);
}

static void sort_and_deduplicate(PathBuilder* builder) {
    if (!builder || builder->count < 2U) return;
    qsort(builder->paths, builder->count, sizeof(*builder->paths),
          compare_discovered_paths);

    size_t output = 1U;
    for (size_t input = 1U; input < builder->count; ++input) {
        CommonDiscoveredPath* previous = &builder->paths[output - 1U];
        CommonDiscoveredPath* current = &builder->paths[input];
        if (strcmp(previous->path, current->path) == 0) {
            previous->explicit_file =
                previous->explicit_file || current->explicit_file;
            free(current->path);
            current->path = NULL;
            continue;
        }
        if (output != input) {
            builder->paths[output] = *current;
            current->path = NULL;
        }
        output++;
    }
    builder->count = output;
}

void common_path_discovery_options_default(CommonPathDiscoveryOptions* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->recursive = true;
}

void common_path_discovery_result_init(CommonPathDiscoveryResult* result) {
    if (!result) return;
    result->paths = NULL;
    result->count = 0U;
}

void common_path_discovery_result_dispose(CommonPathDiscoveryResult* result) {
    if (!result) return;
    for (size_t index = 0U; index < result->count; ++index) {
        free(result->paths[index].path);
    }
    free(result->paths);
    common_path_discovery_result_init(result);
}

static bool discovery_result_is_sorted_unique(
    const CommonPathDiscoveryResult* result) {
    if (!result || (result->count != 0U && !result->paths)) return false;
    for (size_t index = 0U; index < result->count; ++index) {
        const char* path = result->paths[index].path;
        if (!path || path[0] == '\0') return false;
        if (index != 0U &&
            strcmp(result->paths[index - 1U].path, path) >= 0) {
            return false;
        }
    }
    return true;
}

static size_t merged_path_count(const CommonPathDiscoveryResult* destination,
                                const CommonPathDiscoveryResult* addition) {
    size_t destination_index = 0U;
    size_t addition_index = 0U;
    size_t count = 0U;
    while (destination_index < destination->count &&
           addition_index < addition->count) {
        int order = strcmp(destination->paths[destination_index].path,
                           addition->paths[addition_index].path);
        destination_index += order <= 0 ? 1U : 0U;
        addition_index += order >= 0 ? 1U : 0U;
        count++;
    }
    count += destination->count - destination_index;
    count += addition->count - addition_index;
    return count;
}

CommonPathDiscoveryStatus common_path_discovery_result_merge(
    CommonPathDiscoveryResult* destination,
    const CommonPathDiscoveryResult* addition) {
    if (!discovery_result_is_sorted_unique(destination) ||
        !discovery_result_is_sorted_unique(addition)) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }
    if (destination == addition || addition->count == 0U) {
        return COMMON_PATH_DISCOVERY_OK;
    }
    if (destination->paths && destination->paths == addition->paths) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }

    size_t output_count = merged_path_count(destination, addition);
    if (size_mul_overflows(output_count, sizeof(*destination->paths)) ||
        size_mul_overflows(output_count, sizeof(uint8_t))) {
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }
    CommonDiscoveredPath* merged = (CommonDiscoveredPath*)malloc(
        output_count * sizeof(*merged));
    uint8_t* merged_path_is_copy = (uint8_t*)malloc(
        output_count * sizeof(*merged_path_is_copy));
    if (!merged || !merged_path_is_copy) {
        free(merged_path_is_copy);
        free(merged);
        return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
    }

    size_t destination_index = 0U;
    size_t addition_index = 0U;
    size_t output_index = 0U;
    while (destination_index < destination->count ||
           addition_index < addition->count) {
        bool take_destination = addition_index == addition->count;
        bool take_addition = destination_index == destination->count;
        int order = 0;
        if (!take_destination && !take_addition) {
            order = strcmp(destination->paths[destination_index].path,
                           addition->paths[addition_index].path);
            take_destination = order <= 0;
            take_addition = order >= 0;
        }

        if (take_destination) {
            merged[output_index] = destination->paths[destination_index];
            if (take_addition) {
                merged[output_index].explicit_file =
                    merged[output_index].explicit_file ||
                    addition->paths[addition_index].explicit_file;
            }
            merged_path_is_copy[output_index] = 0U;
        } else {
            char* copy = duplicate_path(addition->paths[addition_index].path);
            if (!copy) {
                for (size_t index = 0U; index < output_index; ++index) {
                    if (merged_path_is_copy[index] != 0U) {
                        free(merged[index].path);
                    }
                }
                free(merged_path_is_copy);
                free(merged);
                return COMMON_PATH_DISCOVERY_ALLOCATION_FAILED;
            }
            merged[output_index] = addition->paths[addition_index];
            merged[output_index].path = copy;
            merged_path_is_copy[output_index] = 1U;
        }
        destination_index += take_destination ? 1U : 0U;
        addition_index += take_addition ? 1U : 0U;
        output_index++;
    }

    free(merged_path_is_copy);
    free(destination->paths);
    destination->paths = merged;
    destination->count = output_index;
    return COMMON_PATH_DISCOVERY_OK;
}

CommonPathDiscoveryStatus common_path_discover(
    const char* const* inputs, size_t input_count,
    const CommonPathDiscoveryOptions* options,
    CommonPathDiscoveryResult* result) {
    if (!inputs || input_count == 0U || !result ||
        (result->count != 0U && !result->paths)) {
        return COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
    }

    CommonPathDiscoveryOptions effective_options;
    if (options) {
        effective_options = *options;
    } else {
        common_path_discovery_options_default(&effective_options);
    }
    PathBuilder files = {0};
    DirectoryQueue directories = {0};
    CommonPathDiscoveryStatus status = COMMON_PATH_DISCOVERY_OK;

    for (size_t index = 0U; index < input_count; ++index) {
        const char* input = inputs[index];
        if (!input || input[0] == '\0') {
            status = COMMON_PATH_DISCOVERY_INVALID_ARGUMENT;
            goto done;
        }
        if (effective_options.max_path_bytes != 0U &&
            strlen(input) > effective_options.max_path_bytes) {
            status = COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED;
            goto done;
        }
        PathNodeKind kind;
        status = query_path_kind(input, &kind);
        if (status != COMMON_PATH_DISCOVERY_OK) goto done;
        if (kind == PATH_NODE_LINK) {
            status = COMMON_PATH_DISCOVERY_LINK_REJECTED;
            goto done;
        }
        if (kind == PATH_NODE_OTHER) {
            status = COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE;
            goto done;
        }
        if (kind == PATH_NODE_REGULAR) {
            status = add_file(
                &files, input, true, effective_options.max_files);
        } else {
            status = queue_directory(
                &directories, input, 0U,
                effective_options.max_directories,
                effective_options.max_depth);
        }
        if (status != COMMON_PATH_DISCOVERY_OK) goto done;
    }

    while (directories.next < directories.count) {
        QueuedDirectory current = directories.entries[directories.next++];
        status = scan_directory(
            &current, &effective_options, &directories, &files);
        if (status != COMMON_PATH_DISCOVERY_OK) goto done;
    }

    sort_and_deduplicate(&files);
    common_path_discovery_result_dispose(result);
    result->paths = files.paths;
    result->count = files.count;
    files.paths = NULL;
    files.count = 0U;
    files.capacity = 0U;

done:
    dispose_directory_queue(&directories);
    dispose_builder(&files);
    return status;
}

const char* common_path_discovery_status_name(
    CommonPathDiscoveryStatus status) {
    switch (status) {
        case COMMON_PATH_DISCOVERY_OK: return "ok";
        case COMMON_PATH_DISCOVERY_INVALID_ARGUMENT:
            return "invalid-argument";
        case COMMON_PATH_DISCOVERY_NOT_FOUND: return "not-found";
        case COMMON_PATH_DISCOVERY_LINK_REJECTED: return "link-rejected";
        case COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE:
            return "unsupported-node";
        case COMMON_PATH_DISCOVERY_ALLOCATION_FAILED:
            return "allocation-failed";
        case COMMON_PATH_DISCOVERY_IO_ERROR: return "io-error";
        case COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED:
            return "file-limit-exceeded";
        case COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED:
            return "directory-limit-exceeded";
        case COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED:
            return "depth-limit-exceeded";
        case COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED:
            return "path-limit-exceeded";
        default: return "unknown";
    }
}
