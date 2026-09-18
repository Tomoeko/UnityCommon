#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/path_discovery.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

_Static_assert(COMMON_PATH_DISCOVERY_OK == 0, "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_INVALID_ARGUMENT == 1,
               "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_NOT_FOUND == 2, "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_LINK_REJECTED == 3,
               "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE == 4,
               "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_ALLOCATION_FAILED == 5,
               "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_IO_ERROR == 6, "status ABI changed");
_Static_assert(COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED == 7,
               "new status ordering changed");
_Static_assert(COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED == 8,
               "new status ordering changed");
_Static_assert(COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED == 9,
               "new status ordering changed");
_Static_assert(COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED == 10,
               "new status ordering changed");

enum { TEST_PATH_CAPACITY = 4096 };

static bool test_join_path(char* output, size_t capacity,
                           const char* directory, const char* name) {
    if (!output || capacity == 0U || !directory || !name) return false;
    size_t length = strlen(directory);
#ifdef _WIN32
    bool separator = length != 0U && directory[length - 1U] != '/' &&
                     directory[length - 1U] != '\\';
    const char* format = separator ? "%s\\%s" : "%s%s";
#else
    bool separator = length != 0U && directory[length - 1U] != '/';
    const char* format = separator ? "%s/%s" : "%s%s";
#endif
    int written = snprintf(output, capacity, format, directory, name);
    return written >= 0 && (size_t)written < capacity;
}

static bool test_make_directory(const char* path) {
#ifdef _WIN32
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    bool made = _wmkdir(wide_path) == 0;
    free(wide_path);
    return made;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static bool test_make_root(char root[TEST_PATH_CAPACITY]) {
#ifdef _WIN32
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        int written = snprintf(root, TEST_PATH_CAPACITY,
                               "unity_common_path_discovery_%lu_%u_"
                               "caf\xc3\xa9_\xe9\x9b\xaa",
                               (unsigned long)GetCurrentProcessId(), attempt);
        if (written <= 0 || written >= TEST_PATH_CAPACITY) return false;
        wchar_t* wide_root = common_windows_utf8_to_wide(root);
        if (!wide_root) return false;
        bool made = CreateDirectoryW(wide_root, NULL) != 0;
        DWORD error = made ? ERROR_SUCCESS : GetLastError();
        free(wide_root);
        if (made) return true;
        if (error != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
#else
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        int written = snprintf(root, TEST_PATH_CAPACITY,
                               "/tmp/unity_common_path_discovery_%lu_%u",
                               (unsigned long)getpid(), attempt);
        if (written <= 0 || written >= TEST_PATH_CAPACITY) return false;
        if (mkdir(root, 0700) == 0) return true;
        if (errno != EEXIST) return false;
    }
    return false;
#endif
}

static bool test_write_file(const char* path, const char* contents) {
#ifdef _WIN32
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    HANDLE file = CreateFileW(
        wide_path, GENERIC_WRITE, 0U, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide_path);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t size = strlen(contents);
    DWORD written = 0U;
    bool ok = size <= UINT32_MAX &&
        WriteFile(file, contents, (DWORD)size, &written, NULL) != 0 &&
        written == (DWORD)size;
    if (!CloseHandle(file)) ok = false;
    return ok;
#else
    FILE* stream = fopen(path, "wb");
    if (!stream) return false;
    size_t size = strlen(contents);
    bool ok = fwrite(contents, 1U, size, stream) == size;
    if (fclose(stream) != 0) ok = false;
    return ok;
#endif
}

#ifdef _WIN32
static bool test_remove_file(const char* path) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    bool removed = DeleteFileW(wide_path) != 0;
    free(wide_path);
    return removed;
}

static bool test_remove_directory(const char* path) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    bool removed = RemoveDirectoryW(wide_path) != 0;
    free(wide_path);
    return removed;
}
#endif

static const CommonDiscoveredPath* find_discovered_path(
    const CommonPathDiscoveryResult* result, const char* path) {
    if (!result || !path) return NULL;
    for (size_t index = 0U; index < result->count; ++index) {
        if (strcmp(result->paths[index].path, path) == 0) {
            return &result->paths[index];
        }
    }
    return NULL;
}

static bool result_is_sorted(const CommonPathDiscoveryResult* result) {
    if (!result) return false;
    for (size_t index = 1U; index < result->count; ++index) {
        if (strcmp(result->paths[index - 1U].path,
                   result->paths[index].path) >= 0) {
            return false;
        }
    }
    return true;
}

static bool results_are_equal(const CommonPathDiscoveryResult* left,
                              const CommonPathDiscoveryResult* right) {
    if (!left || !right || left->count != right->count) return false;
    for (size_t index = 0U; index < left->count; ++index) {
        if (strcmp(left->paths[index].path, right->paths[index].path) != 0 ||
            left->paths[index].explicit_file !=
                right->paths[index].explicit_file) {
            return false;
        }
    }
    return true;
}

int main(void) {
    char root[TEST_PATH_CAPACITY];
    char file_a[TEST_PATH_CAPACITY];
    char file_z[TEST_PATH_CAPACITY];
    char subdirectory[TEST_PATH_CAPACITY];
    char file_m[TEST_PATH_CAPACITY];
    char deep_directory[TEST_PATH_CAPACITY];
    char file_b[TEST_PATH_CAPACITY];
    char file_link[TEST_PATH_CAPACITY];
    char directory_link[TEST_PATH_CAPACITY];
#ifndef _WIN32
    char special_path[TEST_PATH_CAPACITY];
    char backslash_directory[TEST_PATH_CAPACITY];
    char backslash_file[TEST_PATH_CAPACITY];
#endif

    CHECK(test_make_root(root));
    CHECK(test_join_path(file_a, sizeof(file_a), root, "a.bin"));
#ifdef _WIN32
    CHECK(test_join_path(file_z, sizeof(file_z), root,
                         "z_caf\xc3\xa9_\xe9\x9b\xaa.bin"));
#else
    CHECK(test_join_path(file_z, sizeof(file_z), root, "z.bin"));
#endif
    CHECK(test_join_path(subdirectory, sizeof(subdirectory), root, "sub"));
    CHECK(test_join_path(file_m, sizeof(file_m), subdirectory, "m.bin"));
    CHECK(test_join_path(deep_directory, sizeof(deep_directory),
                         subdirectory, "deep"));
    CHECK(test_join_path(file_b, sizeof(file_b), deep_directory, "b.bin"));
    CHECK(test_join_path(file_link, sizeof(file_link), root, "file-link"));
    CHECK(test_join_path(directory_link, sizeof(directory_link), root,
                         "directory-link"));
#ifndef _WIN32
    CHECK(test_join_path(special_path, sizeof(special_path), root,
                         "unsupported-fifo"));
#endif

    CHECK(test_write_file(file_z, "z"));
    CHECK(test_write_file(file_a, "a"));
    CHECK(test_make_directory(subdirectory));
    CHECK(test_write_file(file_m, "m"));
    CHECK(test_make_directory(deep_directory));
    CHECK(test_write_file(file_b, "b"));

#ifndef _WIN32
    CHECK(symlink(file_a, file_link) == 0);
    CHECK(symlink(root, directory_link) == 0);
#endif

    CommonPathDiscoveryOptions options;
    common_path_discovery_options_default(&options);
    CHECK(options.recursive);
    CHECK(!options.reject_descendant_links);
    CHECK(!options.reject_descendant_unsupported_nodes);
    CHECK(options.max_files == 0U);
    CHECK(options.max_directories == 0U);
    CHECK(options.max_depth == 0U);
    CHECK(options.max_path_bytes == 0U);
    CommonPathDiscoveryOptions zero_options = {0};
    CHECK(!zero_options.reject_descendant_links);
    CHECK(!zero_options.reject_descendant_unsupported_nodes);
    CHECK(zero_options.max_files == 0U);
    CHECK(zero_options.max_directories == 0U);
    CHECK(zero_options.max_depth == 0U);
    CHECK(zero_options.max_path_bytes == 0U);

    CommonPathDiscoveryResult result;
    common_path_discovery_result_init(&result);

    /* A directory scans only its regular direct children when recursion is
     * disabled. A zero-initialized options value applies no strict policy or
     * resource limit, and descendant links are not returned as files. */
    options.recursive = false;
    const char* nonrecursive_inputs[] = {root};
    CHECK(common_path_discover(
              nonrecursive_inputs, 1U, &zero_options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U);
    CHECK(result_is_sorted(&result));
    CHECK(strcmp(result.paths[0].path, file_a) == 0);
    CHECK(strcmp(result.paths[1].path, file_z) == 0);
    CHECK(!result.paths[0].explicit_file &&
          !result.paths[1].explicit_file);

    /* NULL options exercise the recursive default. Repeated directory inputs
     * and a file also found beneath one directory collapse to one exact path;
     * the explicit-file bit is retained. */
    const char* recursive_inputs[] = {root, root, file_m};
    CHECK(common_path_discover(recursive_inputs, 3U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);
    CHECK(result_is_sorted(&result));
    const CommonDiscoveredPath* discovered_a =
        find_discovered_path(&result, file_a);
    const CommonDiscoveredPath* discovered_z =
        find_discovered_path(&result, file_z);
    const CommonDiscoveredPath* discovered_m =
        find_discovered_path(&result, file_m);
    const CommonDiscoveredPath* discovered_b =
        find_discovered_path(&result, file_b);
    CHECK(discovered_a && discovered_z && discovered_m && discovered_b);
    CHECK(!discovered_a->explicit_file && !discovered_z->explicit_file &&
          !discovered_b->explicit_file);
    CHECK(discovered_m->explicit_file);
    CHECK(find_discovered_path(&result, file_link) == NULL);
    CHECK(find_discovered_path(&result, directory_link) == NULL);

    /* Explicit regular files remain sortable and deduplicate with the origin
     * marker set. */
    const char* explicit_inputs[] = {file_z, file_a, file_a};
    CHECK(common_path_discover(explicit_inputs, 3U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U && result_is_sorted(&result));
    CHECK(strcmp(result.paths[0].path, file_a) == 0 &&
          result.paths[0].explicit_file);
    CHECK(strcmp(result.paths[1].path, file_z) == 0 &&
          result.paths[1].explicit_file);

    /* Incremental merging is equivalent to discovering the same union in one
     * call. Duplicate paths keep the destination allocation and OR the
     * explicit-file origin bit without changing the addition. */
    CHECK(common_path_discover(nonrecursive_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U && !result.paths[0].explicit_file &&
          !result.paths[1].explicit_file);

    CommonPathDiscoveryResult expected;
    common_path_discovery_result_init(&expected);
    const char* expected_inputs[] = {root, file_m, file_a, file_a};
    CHECK(common_path_discover(expected_inputs, 4U, &options, &expected) ==
          COMMON_PATH_DISCOVERY_OK);

    CommonPathDiscoveryResult addition;
    common_path_discovery_result_init(&addition);
    const char* addition_inputs[] = {file_m, file_a, file_a};
    CHECK(common_path_discover(addition_inputs, 3U, NULL, &addition) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(addition.count == 2U && result_is_sorted(&addition));
    CommonDiscoveredPath* addition_array = addition.paths;
    char* addition_a_path = addition.paths[0].path;
    char* addition_m_path = addition.paths[1].path;
    char* destination_a_path = result.paths[0].path;
    char* destination_z_path = result.paths[1].path;
    CHECK(common_path_discovery_result_merge(&result, &addition) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 3U && result_is_sorted(&result));
    CHECK(results_are_equal(&result, &expected));
    CHECK(result.paths[0].path == destination_a_path);
    CHECK(result.paths[2].path == destination_z_path);
    CHECK(result.paths[1].path != addition_m_path);
    CHECK(strcmp(result.paths[1].path, addition_m_path) == 0);
    CHECK(result.paths[0].explicit_file &&
          result.paths[1].explicit_file &&
          !result.paths[2].explicit_file);
    CHECK(addition.paths == addition_array && addition.count == 2U);
    CHECK(addition.paths[0].path == addition_a_path &&
          addition.paths[1].path == addition_m_path);

    /* Self-merge and an empty addition are stable no-ops. */
    CommonDiscoveredPath* stable_array = result.paths;
    CHECK(common_path_discovery_result_merge(&result, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.paths == stable_array && results_are_equal(&result, &expected));
    CommonPathDiscoveryResult empty_addition;
    common_path_discovery_result_init(&empty_addition);
    CHECK(common_path_discovery_result_merge(&result, &empty_addition) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.paths == stable_array && results_are_equal(&result, &expected));

    /* The linear merge contract fails closed for inputs that are not sorted
     * and unique, while preserving both live operands. */
    CommonDiscoveredPath unsorted_paths[] = {
        {file_z, false}, {file_a, true}
    };
    CommonPathDiscoveryResult unsorted_addition = {unsorted_paths, 2U};
    CHECK(common_path_discovery_result_merge(&result, &unsorted_addition) ==
          COMMON_PATH_DISCOVERY_INVALID_ARGUMENT);
    CHECK(result.paths == stable_array && results_are_equal(&result, &expected));
    CommonDiscoveredPath duplicate_paths[] = {
        {file_a, false}, {file_a, true}
    };
    CommonPathDiscoveryResult duplicate_addition = {duplicate_paths, 2U};
    CHECK(common_path_discovery_result_merge(&result, &duplicate_addition) ==
          COMMON_PATH_DISCOVERY_INVALID_ARGUMENT);
    CHECK(result.paths == stable_array && results_are_equal(&result, &expected));
    CommonPathDiscoveryResult invalid_addition = {NULL, 1U};
    CHECK(common_path_discovery_result_merge(
              &result, &invalid_addition) ==
          COMMON_PATH_DISCOVERY_INVALID_ARGUMENT);
    CHECK(result.paths == stable_array && results_are_equal(&result, &expected));
    CHECK(addition.paths == addition_array && addition.count == 2U);
    common_path_discovery_result_dispose(&empty_addition);
    common_path_discovery_result_dispose(&addition);
    common_path_discovery_result_dispose(&expected);

    /* File limits count unique exact path spellings rather than input or
     * discovery occurrences. Exact duplicates therefore remain within the
     * boundary, and a duplicate discovered beneath a directory preserves its
     * explicit-file bit without consuming another slot. */
    common_path_discovery_options_default(&options);
    options.max_files = 2U;
    CHECK(common_path_discover(explicit_inputs, 3U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U && result_is_sorted(&result));
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        char* preserved_first = result.paths[0].path;
        options.max_files = 1U;
        CHECK(common_path_discover(explicit_inputs, 3U, &options, &result) ==
              COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths && result.count == 2U);
        CHECK(result.paths[0].path == preserved_first);
    }
    options.max_files = 4U;
    CHECK(common_path_discover(recursive_inputs, 3U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);
    CHECK(find_discovered_path(&result, file_m) != NULL &&
          find_discovered_path(&result, file_m)->explicit_file);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        char* preserved_first = result.paths[0].path;
        options.max_files = 3U;
        CHECK(common_path_discover(recursive_inputs, 3U, &options, &result) ==
              COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
        CHECK(result.paths[0].path == preserved_first);
    }

    /* Directory limits count unique queued spellings, including explicit
     * roots. Repeated roots do not consume another slot. */
    const char* root_inputs[] = {root};
    const char* duplicate_root_inputs[] = {root, root};
    const char* two_root_inputs[] = {root, subdirectory};
    common_path_discovery_options_default(&options);
    options.recursive = false;
    options.max_directories = 1U;
    CHECK(common_path_discover(
              duplicate_root_inputs, 2U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_directories = 1U;
        CHECK(common_path_discover(two_root_inputs, 2U, &options, &result) ==
              COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }
    common_path_discovery_options_default(&options);
    options.max_directories = 3U;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_directories = 2U;
        CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
              COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }

    /* Explicit directory inputs have depth zero. Root/sub/deep therefore
     * needs depth two from root alone, but only depth one when both root and
     * sub are explicit roots. Zero remains the unlimited compatibility value. */
    common_path_discovery_options_default(&options);
    options.max_depth = 2U;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_depth = 1U;
        CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
              COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }
    options.max_depth = 1U;
    CHECK(common_path_discover(two_root_inputs, 2U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);
    options.max_depth = 0U;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 4U);

    /* Path limits count bytes excluding NUL. They apply both before an
     * explicit input is queried and to every child, including a directory or
     * link that permissive nonrecursive discovery would otherwise ignore. */
    const char* one_file_input[] = {file_a};
    common_path_discovery_options_default(&options);
    options.max_path_bytes = strlen(file_a);
    CHECK(common_path_discover(one_file_input, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 1U && strcmp(result.paths[0].path, file_a) == 0);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        char* preserved_first = result.paths[0].path;
        options.max_path_bytes = strlen(file_a) - 1U;
        CHECK(common_path_discover(one_file_input, 1U, &options, &result) ==
              COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths && result.count == 1U);
        CHECK(result.paths[0].path == preserved_first);
    }
    size_t direct_child_limit = strlen(file_a);
    if (strlen(file_z) > direct_child_limit) {
        direct_child_limit = strlen(file_z);
    }
    if (strlen(subdirectory) > direct_child_limit) {
        direct_child_limit = strlen(subdirectory);
    }
#ifndef _WIN32
    if (strlen(file_link) > direct_child_limit) {
        direct_child_limit = strlen(file_link);
    }
    if (strlen(directory_link) > direct_child_limit) {
        direct_child_limit = strlen(directory_link);
    }
#endif
    common_path_discovery_options_default(&options);
    options.recursive = false;
    options.max_path_bytes = direct_child_limit;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U);
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_path_bytes = direct_child_limit - 1U;
        CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
              COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }

#ifndef _WIN32
    /* An explicit symbolic link is rejected, and failure has a strong output
     * guarantee rather than replacing the previous successful result. */
    const char* link_inputs[] = {file_link};
    CommonDiscoveredPath* before_link_failure = result.paths;
    size_t before_link_failure_count = result.count;
    char* before_link_failure_first = result.paths[0].path;
    CHECK(common_path_discover(link_inputs, 1U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_LINK_REJECTED);
    CHECK(result.paths == before_link_failure &&
          result.count == before_link_failure_count);
    CHECK(result.paths[0].path == before_link_failure_first);

    /* Descendant links remain ignored by default, while strict mode rejects
     * them without following either the file or directory target. */
    common_path_discovery_options_default(&options);
    options.reject_descendant_links = true;
    CommonDiscoveredPath* before_strict_link = result.paths;
    size_t before_strict_link_count = result.count;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_LINK_REJECTED);
    CHECK(result.paths == before_strict_link &&
          result.count == before_strict_link_count);

    /* Other descendants are likewise optional strict failures. A FIFO is
     * never returned as a regular file under either policy. */
    CHECK(mkfifo(special_path, 0600) == 0);
    common_path_discovery_options_default(&options);
    options.reject_descendant_unsupported_nodes = true;
    CommonDiscoveredPath* before_strict_special = result.paths;
    size_t before_strict_special_count = result.count;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE);
    CHECK(result.paths == before_strict_special &&
          result.count == before_strict_special_count);
    const char* special_inputs[] = {special_path};
    CHECK(common_path_discover(special_inputs, 1U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_UNSUPPORTED_NODE);
    CHECK(result.paths == before_strict_special &&
          result.count == before_strict_special_count);
    common_path_discovery_options_default(&options);
    options.recursive = false;
    CHECK(common_path_discover(root_inputs, 1U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 2U);
    CHECK(find_discovered_path(&result, special_path) == NULL);
    CHECK(remove(special_path) == 0);

    /* Backslash is a legal POSIX filename byte, not a path separator.  A
     * directory whose name ends with one must still receive a slash when its
     * children are joined. */
    CHECK(test_join_path(backslash_directory, sizeof(backslash_directory),
                         root, "literal-backslash\\"));
    CHECK(test_join_path(backslash_file, sizeof(backslash_file),
                         backslash_directory, "child.bin"));
    CHECK(test_make_directory(backslash_directory));
    CHECK(test_write_file(backslash_file, "child"));
    const char* backslash_inputs[] = {backslash_directory};
    CHECK(common_path_discover(backslash_inputs, 1U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == 1U);
    CHECK(strcmp(result.paths[0].path, backslash_file) == 0);
#endif

    const char* empty_input[] = {""};
    CommonDiscoveredPath* before_invalid_input = result.paths;
    size_t before_invalid_input_count = result.count;
    char* before_invalid_input_first = result.paths[0].path;
    CHECK(common_path_discover(empty_input, 1U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_INVALID_ARGUMENT);
    CHECK(result.paths == before_invalid_input &&
          result.count == before_invalid_input_count);
    CHECK(result.paths[0].path == before_invalid_input_first);
#ifdef _WIN32
    static const char invalid_utf8[] = "\xc3(";
    const char* invalid_utf8_input[] = {invalid_utf8};
    CHECK(common_path_discover(invalid_utf8_input, 1U, NULL, &result) ==
          COMMON_PATH_DISCOVERY_INVALID_ARGUMENT);
    CHECK(result.paths == before_invalid_input &&
          result.count == before_invalid_input_count);
    CHECK(result.paths[0].path == before_invalid_input_first);
#else
    CHECK(result.count == 1U);
    CHECK(strcmp(result.paths[0].path, backslash_file) == 0);
#endif
    CHECK(strcmp(common_path_discovery_status_name(
                     COMMON_PATH_DISCOVERY_LINK_REJECTED),
                 "link-rejected") == 0);
    CHECK(strcmp(common_path_discovery_status_name(
                     COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED),
                 "file-limit-exceeded") == 0);
    CHECK(strcmp(common_path_discovery_status_name(
                     COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED),
                 "directory-limit-exceeded") == 0);
    CHECK(strcmp(common_path_discovery_status_name(
                     COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED),
                 "depth-limit-exceeded") == 0);
    CHECK(strcmp(common_path_discovery_status_name(
                     COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED),
                 "path-limit-exceeded") == 0);

    /* Cross several exact-path-index growth boundaries with distinct files
     * and directories. This guards the expected-linear duplicate lookup used
     * for large trees while rechecking exact limit boundaries after rehash. */
    enum { INDEX_STRESS_COUNT = 96 };
    char index_root[TEST_PATH_CAPACITY];
    char index_directory[TEST_PATH_CAPACITY];
    char index_file[TEST_PATH_CAPACITY];
    char index_name[32];
    CHECK(test_join_path(index_root, sizeof(index_root),
                         root, "index-stress"));
    CHECK(test_make_directory(index_root));
    for (unsigned index = 0U; index < INDEX_STRESS_COUNT; ++index) {
        int name_size = snprintf(index_name, sizeof(index_name),
                                 "directory-%03u", index);
        CHECK(name_size > 0 && (size_t)name_size < sizeof(index_name));
        CHECK(test_join_path(index_directory, sizeof(index_directory),
                             index_root, index_name));
        CHECK(test_make_directory(index_directory));
        CHECK(test_join_path(index_file, sizeof(index_file),
                             index_directory, "entry.bin"));
        CHECK(test_write_file(index_file, "x"));
    }
    const char* index_inputs[] = {index_root, index_root};
    common_path_discovery_options_default(&options);
    options.max_files = INDEX_STRESS_COUNT;
    options.max_directories = INDEX_STRESS_COUNT + 1U;
    options.max_depth = 1U;
    CHECK(common_path_discover(index_inputs, 2U, &options, &result) ==
          COMMON_PATH_DISCOVERY_OK);
    CHECK(result.count == INDEX_STRESS_COUNT && result_is_sorted(&result));
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_files = INDEX_STRESS_COUNT - 1U;
        CHECK(common_path_discover(index_inputs, 2U, &options, &result) ==
              COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }
    {
        CommonDiscoveredPath* preserved_paths = result.paths;
        size_t preserved_count = result.count;
        options.max_files = INDEX_STRESS_COUNT;
        options.max_directories = INDEX_STRESS_COUNT;
        CHECK(common_path_discover(index_inputs, 2U, &options, &result) ==
              COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED);
        CHECK(result.paths == preserved_paths &&
              result.count == preserved_count);
    }

    common_path_discovery_result_dispose(&result);
    CHECK(result.paths == NULL && result.count == 0U);

    for (unsigned index = 0U; index < INDEX_STRESS_COUNT; ++index) {
        int name_size = snprintf(index_name, sizeof(index_name),
                                 "directory-%03u", index);
        CHECK(name_size > 0 && (size_t)name_size < sizeof(index_name));
        CHECK(test_join_path(index_directory, sizeof(index_directory),
                             index_root, index_name));
        CHECK(test_join_path(index_file, sizeof(index_file),
                             index_directory, "entry.bin"));
#ifdef _WIN32
        CHECK(test_remove_file(index_file));
        CHECK(test_remove_directory(index_directory));
#else
        CHECK(remove(index_file) == 0);
        CHECK(rmdir(index_directory) == 0);
#endif
    }
#ifdef _WIN32
    CHECK(test_remove_directory(index_root));
#else
    CHECK(rmdir(index_root) == 0);
#endif

#ifndef _WIN32
    CHECK(remove(backslash_file) == 0);
    CHECK(rmdir(backslash_directory) == 0);
    CHECK(unlink(directory_link) == 0);
    CHECK(unlink(file_link) == 0);
#endif
#ifdef _WIN32
    CHECK(test_remove_file(file_b));
    CHECK(test_remove_file(file_m));
    CHECK(test_remove_file(file_z));
    CHECK(test_remove_file(file_a));
    CHECK(test_remove_directory(deep_directory));
    CHECK(test_remove_directory(subdirectory));
    CHECK(test_remove_directory(root));
#else
    CHECK(remove(file_b) == 0);
    CHECK(remove(file_m) == 0);
    CHECK(remove(file_z) == 0);
    CHECK(remove(file_a) == 0);
    CHECK(rmdir(deep_directory) == 0);
    CHECK(rmdir(subdirectory) == 0);
    CHECK(rmdir(root) == 0);
#endif

    printf("path discovery unit tests passed\n");
    return 0;
}
