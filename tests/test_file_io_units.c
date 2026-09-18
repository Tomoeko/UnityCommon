#ifndef _WIN32
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/file_io.h"
#include "common/output_publish.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#else
#include <dirent.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_PROCESS_ID() ((unsigned long)getpid())
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#ifdef _WIN32
static bool delete_utf8_file(const char* path) {
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    bool deleted = DeleteFileW(wide_path) != 0;
    free(wide_path);
    return deleted;
}

static int test_windows_directory_roots(void) {
    static const char unicode[] = "caf\xc3\xa9_\xe9\x9b\xaa";
    wchar_t* wide_unicode = common_windows_utf8_to_wide(unicode);
    CHECK(wide_unicode != NULL);
    char* roundtrip = common_windows_wide_to_utf8(wide_unicode);
    free(wide_unicode);
    CHECK(roundtrip != NULL);
    CHECK(strcmp(roundtrip, unicode) == 0);
    free(roundtrip);

    static const wchar_t unpaired_surrogate[] = {(wchar_t)0xd800U, L'\0'};
    CHECK(common_windows_wide_to_utf8(unpaired_surrogate) == NULL);
    CHECK(GetLastError() == ERROR_NO_UNICODE_TRANSLATION);

    size_t root = SIZE_MAX;
    const char* drive = "C:\\child";
    CHECK(common_windows_directory_root_length(
        drive, strlen(drive), &root));
    CHECK(root == strlen("C:\\"));

    const char* unc = "\\\\server\\share\\child";
    CHECK(common_windows_directory_root_length(unc, strlen(unc), &root));
    CHECK(root == strlen("\\\\server\\share\\"));

    const char* extended_drive = "\\\\?\\C:\\child";
    CHECK(common_windows_directory_root_length(
        extended_drive, strlen(extended_drive), &root));
    CHECK(root == strlen("\\\\?\\C:\\"));

    const char* extended_unc = "\\\\?\\UNC\\server\\share\\child";
    CHECK(common_windows_directory_root_length(
        extended_unc, strlen(extended_unc), &root));
    CHECK(root == strlen("\\\\?\\UNC\\server\\share\\"));

    const char* incomplete_unc = "\\\\server";
    CHECK(!common_windows_directory_root_length(
        incomplete_unc, strlen(incomplete_unc), &root));
    const char* incomplete_extended_unc = "\\\\?\\UNC\\server\\";
    CHECK(!common_windows_directory_root_length(
        incomplete_extended_unc, strlen(incomplete_extended_unc), &root));
    CHECK(!common_windows_directory_root_length(NULL, 0U, &root));
    CHECK(!common_windows_directory_root_length("path", 4U, NULL));
    return 0;
}
#endif

#ifndef _WIN32
/* Test-local interposition gives the parent-replacement regression an exact
 * synchronization point after the temporary is complete but before it is
 * published as the destination.  The production API exposes no test hook;
 * this executable alone interposes the platform's no-replace primitive. */
static bool publish_commit_barrier_enabled = false;
static bool publish_commit_force_error = false;
static int publish_commit_barrier_arrived = -1;
static int publish_commit_barrier_release = -1;

static bool transfer_barrier_byte(int descriptor, bool write_byte) {
    uint8_t byte = 1U;
    ssize_t transferred;
    do {
        transferred = write_byte
            ? write(descriptor, &byte, 1U)
            : read(descriptor, &byte, 1U);
    } while (transferred < 0 && errno == EINTR);
    return transferred == 1;
}

static bool await_publish_commit_barrier(void) {
    if (publish_commit_barrier_enabled) {
        publish_commit_barrier_enabled = false;
        if (!transfer_barrier_byte(publish_commit_barrier_arrived, true) ||
            !transfer_barrier_byte(publish_commit_barrier_release, false)) {
            errno = EIO;
            return false;
        }
    }
    return true;
}

#ifdef __APPLE__
int renameatx_np(int source_directory, const char* source_name,
                 int destination_directory, const char* destination_name,
    unsigned int flags) {
    if (!await_publish_commit_barrier()) return -1;
    if (publish_commit_force_error) {
        errno = EIO;
        return -1;
    }
    typedef int (*RenameExclusiveFunction)(
        int, const char*, int, const char*, unsigned int);
    static RenameExclusiveFunction system_renameatx_np = NULL;
    if (!system_renameatx_np) {
        system_renameatx_np = (RenameExclusiveFunction)dlsym(
            RTLD_NEXT, "renameatx_np");
        if (!system_renameatx_np) {
            errno = ENOSYS;
            return -1;
        }
    }
    return system_renameatx_np(
        source_directory, source_name,
        destination_directory, destination_name, flags);
}
#else
int linkat(int source_directory, const char* source_name,
           int destination_directory, const char* destination_name,
           int flags) {
    if (!await_publish_commit_barrier()) return -1;
    return (int)syscall(SYS_linkat, source_directory, source_name,
                        destination_directory, destination_name, flags);
}
#endif

static int test_publish_parent_replacement(bool force_commit_error) {
    char directory[256];
    char moved_directory[256];
    char destination[512];
    char moved_destination[512];
    CHECK(snprintf(directory, sizeof(directory),
                   "unity_common_publish_parent_%lu_%s", TEST_PROCESS_ID(),
                   force_commit_error ? "cleanup" : "commit") > 0);
    CHECK(snprintf(moved_directory, sizeof(moved_directory),
                   "unity_common_publish_parent_%lu_%s_moved",
                   TEST_PROCESS_ID(),
                   force_commit_error ? "cleanup" : "commit") > 0);
    CHECK(snprintf(destination, sizeof(destination), "%s/output.bin",
                   directory) > 0);
    CHECK(snprintf(moved_destination, sizeof(moved_destination),
                   "%s/output.bin", moved_directory) > 0);
    (void)remove(destination);
    (void)remove(moved_destination);
    (void)rmdir(directory);
    (void)rmdir(moved_directory);
    CHECK(mkdir(directory, 0700) == 0);

    const size_t payload_size = 4096U;
    uint8_t* payload = (uint8_t*)malloc(payload_size);
    CHECK(payload != NULL);
    memset(payload, 0x5a, payload_size);

    int arrived_pipe[2];
    int release_pipe[2];
    CHECK(pipe(arrived_pipe) == 0);
    CHECK(pipe(release_pipe) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(arrived_pipe[1]);
        (void)close(release_pipe[0]);
        int child_exit = 0;
        if (!transfer_barrier_byte(arrived_pipe[0], false)) {
            child_exit = 10;
            goto child_release;
        }
        char replacement_entry_name[256] = {0};
#ifdef __APPLE__
        /* macOS uses a named temporary. Reusing that exact spelling in the
         * replacement directory proves cleanup stayed descriptor-relative. */
        DIR* stream = opendir(directory);
        if (!stream) {
            child_exit = 11;
            goto child_release;
        }
        size_t temporary_count = 0U;
        struct dirent* entry;
        while ((entry = readdir(stream)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            ++temporary_count;
            size_t name_size = strlen(entry->d_name);
            if (name_size >= sizeof(replacement_entry_name)) {
                child_exit = 12;
                continue;
            }
            if (temporary_count == 1U) {
                memcpy(replacement_entry_name, entry->d_name, name_size + 1U);
            }
        }
        if (closedir(stream) != 0 && child_exit == 0) child_exit = 13;
        if (child_exit != 0 || temporary_count != 1U ||
            !replacement_entry_name[0]) {
            if (child_exit == 0) child_exit = 14;
            goto child_release;
        }
#else
        /* Linux O_TMPFILE has no directory entry to enumerate. An unrelated
         * sentinel still proves publication/cleanup cannot reach the newly
         * created replacement namespace. */
        static const char sentinel_name[] = "replacement.sentinel";
        memcpy(replacement_entry_name, sentinel_name, sizeof(sentinel_name));
#endif
        if (rename(directory, moved_directory) != 0 ||
            mkdir(directory, 0700) != 0) {
            child_exit = 15;
            goto child_release;
        }
        char replacement_temporary[768];
        int count = snprintf(
            replacement_temporary, sizeof(replacement_temporary),
            "%s/%s", directory, replacement_entry_name);
        if (count <= 0 ||
            (size_t)count >= sizeof(replacement_temporary)) {
            child_exit = 16;
            goto child_release;
        }
        static const uint8_t sentinel[] = {'u', 'n', 'r', 'e', 'l', 'a',
                                           't', 'e', 'd'};
        int descriptor = open(
            replacement_temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) {
            child_exit = 17;
            goto child_release;
        }
        if (write(descriptor, sentinel, sizeof(sentinel)) !=
                (ssize_t)sizeof(sentinel) ||
            fsync(descriptor) != 0 || close(descriptor) != 0) {
            child_exit = 18;
            goto child_release;
        }

child_release:
        if (!transfer_barrier_byte(release_pipe[1], true) &&
            child_exit == 0) {
            child_exit = 19;
        }
        (void)close(arrived_pipe[0]);
        (void)close(release_pipe[1]);
        _exit(child_exit);
    }
    CHECK(close(arrived_pipe[0]) == 0);
    CHECK(close(release_pipe[1]) == 0);
    publish_commit_barrier_arrived = arrived_pipe[1];
    publish_commit_barrier_release = release_pipe[0];
    publish_commit_barrier_enabled = true;
    publish_commit_force_error = force_commit_error;
    CommonFileStatus publish_status = common_file_write_new_atomic(
        destination, payload, payload_size);
    CHECK(!publish_commit_barrier_enabled);
    publish_commit_force_error = false;
    publish_commit_barrier_arrived = -1;
    publish_commit_barrier_release = -1;
    CHECK(close(arrived_pipe[1]) == 0);
    CHECK(close(release_pipe[0]) == 0);
    free(payload);
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(publish_status == COMMON_FILE_IO_ERROR);
    CHECK(access(destination, F_OK) != 0);

    DIR* replacement = opendir(directory);
    CHECK(replacement != NULL);
    size_t replacement_files = 0U;
    struct dirent* entry;
    char replacement_path[768] = {0};
    while ((entry = readdir(replacement)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ++replacement_files;
        int count = snprintf(replacement_path, sizeof(replacement_path),
                             "%s/%s", directory, entry->d_name);
        CHECK(count > 0 && (size_t)count < sizeof(replacement_path));
    }
    CHECK(closedir(replacement) == 0);
    CHECK(replacement_files == 1U);
    CommonFileBytes sentinel_bytes;
    CHECK(common_file_read_regular(
              replacement_path, 64U, &sentinel_bytes) == COMMON_FILE_OK);
    static const uint8_t expected_sentinel[] = {
        'u', 'n', 'r', 'e', 'l', 'a', 't', 'e', 'd'};
    CHECK(sentinel_bytes.size == sizeof(expected_sentinel));
    CHECK(memcmp(sentinel_bytes.data, expected_sentinel,
                 sizeof(expected_sentinel)) == 0);
    common_file_bytes_dispose(&sentinel_bytes);

    CHECK(remove(replacement_path) == 0);
    CHECK(rmdir(directory) == 0);
    if (force_commit_error) {
        CHECK(access(moved_destination, F_OK) != 0);
    } else {
        CHECK(remove(moved_destination) == 0);
    }
    CHECK(rmdir(moved_directory) == 0);
    return 0;
}
#endif

int main(void) {
#ifdef _WIN32
    CHECK(test_windows_directory_roots() == 0);
#else
    CHECK(test_publish_parent_replacement(false) == 0);
#ifdef __APPLE__
    CHECK(test_publish_parent_replacement(true) == 0);
#endif
#endif
    char path[256];
    char link_path[256];
    int path_count = snprintf(path, sizeof(path),
                              "unity_common_file_io_%lu.bin", TEST_PROCESS_ID());
    int link_count = snprintf(link_path, sizeof(link_path),
                              "unity_common_file_io_%lu.link", TEST_PROCESS_ID());
    CHECK(path_count > 0 && (size_t)path_count < sizeof(path));
    CHECK(link_count > 0 && (size_t)link_count < sizeof(link_path));
    (void)remove(link_path);
    (void)remove(path);

    CommonFileBytes bytes = {(uint8_t*)1, 1U};
    CHECK(common_file_read_regular(NULL, SIZE_MAX, &bytes) ==
          COMMON_FILE_INVALID_ARGUMENT);
    CHECK(bytes.data == NULL && bytes.size == 0U);
    CHECK(common_file_read_regular(path, SIZE_MAX, &bytes) ==
          COMMON_FILE_NOT_FOUND);
    CommonFileView view = {
        .data = (const uint8_t*)1,
        .size = 1U,
        .implementation = (void*)1,
    };
    CHECK(common_file_view_open_regular(NULL, SIZE_MAX, &view) ==
          COMMON_FILE_INVALID_ARGUMENT);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation == NULL);
    uint8_t captured_digest[COMMON_SHA256_DIGEST_SIZE];
    CHECK(!common_file_view_sha256(&view, captured_digest));

    static const uint8_t payload[] = {0x00, 0x7f, 0x80, 0xff, 'D', 'X'};
    CHECK(common_file_write_new_atomic(path, payload, sizeof(payload)) ==
          COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(path, payload, sizeof(payload)) ==
          COMMON_FILE_ALREADY_EXISTS);
    CHECK(common_output_publish_exact_residue_possible(
        COMMON_OUTPUT_PREFLIGHT_MISSING, COMMON_OUTPUT_PUBLISH_IO_ERROR,
        path, payload, sizeof(payload)));
    CHECK(!common_output_publish_exact_residue_possible(
        COMMON_OUTPUT_PREFLIGHT_UNCHANGED, COMMON_OUTPUT_PUBLISH_IO_ERROR,
        path, payload, sizeof(payload)));
    CHECK(!common_output_publish_exact_residue_possible(
        COMMON_OUTPUT_PREFLIGHT_MISSING, COMMON_OUTPUT_PUBLISH_COLLISION,
        path, payload, sizeof(payload)));
    static const uint8_t different[] = {
        0x01, 0x7f, 0x80, 0xff, 'D', 'X'};
    CHECK(!common_output_publish_exact_residue_possible(
        COMMON_OUTPUT_PREFLIGHT_MISSING, COMMON_OUTPUT_PUBLISH_IO_ERROR,
        path, different, sizeof(different)));
    uint8_t prefix[3] = {0xffU, 0xffU, 0xffU};
    size_t prefix_size = SIZE_MAX;
    uint64_t probed_size = UINT64_MAX;
    CHECK(common_file_read_prefix_regular(
              path, prefix, sizeof(prefix), &prefix_size, &probed_size) ==
          COMMON_FILE_OK);
    CHECK(prefix_size == sizeof(prefix));
    CHECK(probed_size == sizeof(payload));
    CHECK(memcmp(prefix, payload, sizeof(prefix)) == 0);
    CHECK(common_file_read_prefix_regular(
              path, NULL, 0U, &prefix_size, &probed_size) == COMMON_FILE_OK);
    CHECK(prefix_size == 0U && probed_size == sizeof(payload));
    CHECK(common_file_view_open_regular(
              path, sizeof(payload) - 1U, &view) == COMMON_FILE_TOO_LARGE);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation == NULL);
    CHECK(common_file_view_open_regular(
              path, sizeof(payload), &view) == COMMON_FILE_OK);
    CHECK(view.data != NULL && view.size == sizeof(payload));
    CHECK(memcmp(view.data, payload, sizeof(payload)) == 0);
    uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(payload, sizeof(payload), expected_digest);
    CHECK(common_file_view_sha256(&view, captured_digest));
    CHECK(memcmp(captured_digest, expected_digest,
                 sizeof(expected_digest)) == 0);
    CHECK(common_file_view_close(&view) == COMMON_FILE_OK);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation == NULL);
    CHECK(!common_file_view_sha256(&view, captured_digest));
    CHECK(common_file_view_close(&view) == COMMON_FILE_INVALID_ARGUMENT);
    CHECK(common_file_read_regular(path, sizeof(payload), &bytes) ==
          COMMON_FILE_OK);
    CHECK(bytes.size == sizeof(payload));
    CHECK(memcmp(bytes.data, payload, sizeof(payload)) == 0);
    common_file_bytes_dispose(&bytes);
    CHECK(bytes.data == NULL && bytes.size == 0U);
    CHECK(common_file_read_regular(path, sizeof(payload) - 1U, &bytes) ==
          COMMON_FILE_TOO_LARGE);
    CHECK(common_file_read_regular_terminated(
              path, sizeof(payload), &bytes) == COMMON_FILE_OK);
    CHECK(bytes.size == sizeof(payload));
    CHECK(bytes.data[bytes.size] == 0U);
    CHECK(memcmp(bytes.data, payload, sizeof(payload)) == 0);
    common_file_bytes_dispose(&bytes);

#ifndef _WIN32
    CHECK(symlink(path, link_path) == 0);
    CHECK(common_file_read_regular(link_path, SIZE_MAX, &bytes) ==
          COMMON_FILE_NOT_REGULAR);
    CHECK(common_file_read_prefix_regular(
              link_path, prefix, sizeof(prefix), &prefix_size,
              &probed_size) == COMMON_FILE_NOT_REGULAR);
    CHECK(common_file_view_open_regular(
              link_path, SIZE_MAX, &view) == COMMON_FILE_NOT_REGULAR);
    CHECK(remove(link_path) == 0);

    CHECK(common_file_view_open_regular(
              path, SIZE_MAX, &view) == COMMON_FILE_OK);
    FILE* mutator = fopen(path, "r+b");
    CHECK(mutator != NULL);
    uint8_t changed = (uint8_t)(payload[0] ^ 0xffU);
    CHECK(fwrite(&changed, 1U, 1U, mutator) == 1U);
    CHECK(fflush(mutator) == 0);
    CHECK(fclose(mutator) == 0);
    CHECK(memcmp(view.data, payload, sizeof(payload)) == 0);
    CHECK(common_file_view_close(&view) == COMMON_FILE_IO_ERROR);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation == NULL);

    CHECK(remove(path) == 0);
    CHECK(common_file_write_new_atomic(path, payload, sizeof(payload)) ==
          COMMON_FILE_OK);
    CHECK(common_file_view_open_regular(
              path, sizeof(payload), &view) == COMMON_FILE_OK);
    uint8_t expected_snapshot[sizeof(payload)];
    memcpy(expected_snapshot, view.data, sizeof(expected_snapshot));
    FILE* truncator = fopen(path, "r+b");
    CHECK(truncator != NULL);
    CHECK(ftruncate(fileno(truncator), 0) == 0);
    CHECK(fclose(truncator) == 0);
    CHECK(view.size == sizeof(expected_snapshot));
    CHECK(memcmp(view.data, expected_snapshot,
                 sizeof(expected_snapshot)) == 0);
    CHECK(common_file_view_close(&view) == COMMON_FILE_IO_ERROR);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation == NULL);

    /* A retained MAP_SHARED writer can alter bytes without changing mtime or
     * ctime until it unmaps. The private captured image must stay readable,
     * and close must compare held-source bytes rather than trust metadata. */
    CHECK(remove(path) == 0);
    CHECK(common_file_write_new_atomic(path, payload, sizeof(payload)) ==
          COMMON_FILE_OK);
    int mapped_writer_descriptor = open(path, O_RDWR);
    CHECK(mapped_writer_descriptor >= 0);
    uint8_t* mapped_writer = (uint8_t*)mmap(
        NULL, sizeof(payload), PROT_READ | PROT_WRITE, MAP_SHARED,
        mapped_writer_descriptor, 0);
    CHECK(mapped_writer != MAP_FAILED && mapped_writer != NULL);
    CHECK(common_file_view_open_regular(
              path, sizeof(payload), &view) == COMMON_FILE_OK);
    mapped_writer[0] = (uint8_t)(payload[0] ^ 0x5aU);
    CHECK(memcmp(view.data, payload, sizeof(payload)) == 0);
    CHECK(common_file_view_close(&view) == COMMON_FILE_IO_ERROR);
    CHECK(munmap(mapped_writer, sizeof(payload)) == 0);
    CHECK(close(mapped_writer_descriptor) == 0);

    /* Closing a relative-path view is independent of later process-wide cwd
     * changes because the opening cwd remains the full-path namespace root. */
    char original_directory[4096];
    CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    char directory_a[256];
    char directory_b[256];
    char file_a[512];
    char file_b[512];
    CHECK(snprintf(directory_a, sizeof(directory_a),
                   "unity_common_file_io_%lu_a", TEST_PROCESS_ID()) > 0);
    CHECK(snprintf(directory_b, sizeof(directory_b),
                   "unity_common_file_io_%lu_b", TEST_PROCESS_ID()) > 0);
    CHECK(snprintf(file_a, sizeof(file_a), "%s/relative.bin",
                   directory_a) > 0);
    CHECK(snprintf(file_b, sizeof(file_b), "%s/relative.bin",
                   directory_b) > 0);
    (void)remove(file_a);
    (void)remove(file_b);
    (void)rmdir(directory_a);
    (void)rmdir(directory_b);
    CHECK(mkdir(directory_a, 0700) == 0);
    CHECK(mkdir(directory_b, 0700) == 0);
    CHECK(common_file_write_new_atomic(
              file_a, payload, sizeof(payload)) == COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(
              file_b, different, sizeof(different)) == COMMON_FILE_OK);
    CHECK(chdir(directory_a) == 0);
    CHECK(common_file_view_open_regular(
              "relative.bin", sizeof(payload), &view) == COMMON_FILE_OK);
    CHECK(chdir(original_directory) == 0);
    CHECK(chdir(directory_b) == 0);
    CHECK(common_file_view_close(&view) == COMMON_FILE_OK);
    CHECK(chdir(original_directory) == 0);

    char moved_directory[256];
    char moved_file[512];
    CHECK(snprintf(moved_directory, sizeof(moved_directory),
                   "unity_common_file_io_%lu_moved", TEST_PROCESS_ID()) > 0);
    CHECK(snprintf(moved_file, sizeof(moved_file), "%s/relative.bin",
                   moved_directory) > 0);
    (void)remove(moved_file);
    (void)rmdir(moved_directory);
    CHECK(common_file_view_open_regular(
              file_a, sizeof(payload), &view) == COMMON_FILE_OK);
    CHECK(rename(directory_a, moved_directory) == 0);
    CHECK(mkdir(directory_a, 0700) == 0);
    CHECK(common_file_write_new_atomic(
              file_a, payload, sizeof(payload)) == COMMON_FILE_OK);
    CHECK(common_file_view_close(&view) == COMMON_FILE_IO_ERROR);
    CHECK(remove(file_a) == 0);
    CHECK(rmdir(directory_a) == 0);
    CHECK(remove(moved_file) == 0);
    CHECK(rmdir(moved_directory) == 0);
    CHECK(remove(file_b) == 0);
    CHECK(rmdir(directory_b) == 0);
#endif

    CHECK(remove(path) == 0);
    CHECK(common_file_write_new_atomic(path, NULL, 0U) == COMMON_FILE_OK);
    CHECK(common_file_read_regular(path, 0U, &bytes) == COMMON_FILE_OK);
    CHECK(bytes.data == NULL && bytes.size == 0U);
    common_file_bytes_dispose(&bytes);
    CHECK(common_file_read_regular_terminated(path, 0U, &bytes) ==
          COMMON_FILE_OK);
    CHECK(bytes.data != NULL && bytes.size == 0U && bytes.data[0] == 0U);
    common_file_bytes_dispose(&bytes);
    CHECK(common_file_view_open_regular(path, 0U, &view) == COMMON_FILE_OK);
    CHECK(view.data == NULL && view.size == 0U &&
          view.implementation != NULL);
    CHECK(common_file_view_close(&view) == COMMON_FILE_OK);
    CHECK(remove(path) == 0);

#ifdef _WIN32
    char unicode_path[256];
    int unicode_count = snprintf(
        unicode_path, sizeof(unicode_path),
        "unity_common_file_io_%lu_caf\xc3\xa9_\xe9\x9b\xaa.bin",
        TEST_PROCESS_ID());
    CHECK(unicode_count > 0 &&
          (size_t)unicode_count < sizeof(unicode_path));
    wchar_t* stale_unicode = common_windows_utf8_to_wide(unicode_path);
    CHECK(stale_unicode != NULL);
    (void)DeleteFileW(stale_unicode);
    free(stale_unicode);
    CHECK(common_file_write_new_atomic(
              unicode_path, payload, sizeof(payload)) == COMMON_FILE_OK);
    CHECK(common_file_read_regular(
              unicode_path, sizeof(payload), &bytes) == COMMON_FILE_OK);
    CHECK(bytes.size == sizeof(payload));
    CHECK(memcmp(bytes.data, payload, sizeof(payload)) == 0);
    common_file_bytes_dispose(&bytes);
    CHECK(common_file_view_open_regular(
              unicode_path, sizeof(payload), &view) == COMMON_FILE_OK);
    CHECK(common_file_view_close(&view) == COMMON_FILE_OK);
    CHECK(delete_utf8_file(unicode_path));

    static const char invalid_utf8[] = {(char)0xc3, '(', '\0'};
    CHECK(common_file_read_regular(invalid_utf8, SIZE_MAX, &bytes) ==
          COMMON_FILE_INVALID_ARGUMENT);
    CHECK(bytes.data == NULL && bytes.size == 0U);
#endif

    CHECK(strcmp(common_file_status_name(COMMON_FILE_OK), "ok") == 0);
    CHECK(strcmp(common_file_status_name(COMMON_FILE_ALREADY_EXISTS),
                 "already-exists") == 0);
    printf("file I/O unit tests passed\n");
    return 0;
}
