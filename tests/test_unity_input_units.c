#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/file_io.h"
#include "io/unity_input.h"
#include "test_support/file_mutation.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

static void store_be32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void store_be64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> ((7U - i) * 8U));
    }
}

static void make_v22_header(uint8_t bytes[64]) {
    memset(bytes, 0, 64U);
    store_be32(bytes + 8U, 22U);
    store_be64(bytes + 16U, 16U);
    store_be64(bytes + 24U, 64U);
    store_be64(bytes + 32U, 64U);
}

static int test_serialized_header_layout(void) {
    uint8_t bytes[64];
    make_v22_header(bytes);
    UnityInputProbe probe;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_OK);
    CHECK(probe.kind == UNITY_INPUT_KIND_SERIALIZED_FILE_V22);

    bytes[40] = 1;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_OK);
    bytes[40] = 2;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);
    bytes[40] = 255;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);

    make_v22_header(bytes);
    const size_t raw_offsets[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 41, 42, 43, 44, 45, 46, 47};
    for (size_t index = 0; index < sizeof(raw_offsets) / sizeof(raw_offsets[0]); ++index) {
        bytes[raw_offsets[index]] = 0xd3;
    }
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_OK);

    /* Contradictory v22 metadata high bits are a damaged Unity header, not
     * an unrelated resource. No metadata bytes need to be mapped here. */
    for (size_t offset = 16; offset < 20; ++offset) {
        make_v22_header(bytes);
        bytes[offset] = 2;
        CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
        CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);
        CHECK(probe.serialized_file_version == 22);
    }
    make_v22_header(bytes);
    const uint64_t logical_size = UINT64_C(0x10000000000);
    store_be64(bytes + 16, logical_size - 48);
    store_be64(bytes + 24, logical_size);
    store_be64(bytes + 32, logical_size);
    CHECK(unity_input_probe_bytes(bytes, 48, logical_size, &probe) == UNITY_INPUT_OK);
    CHECK(probe.kind == UNITY_INPUT_KIND_SERIALIZED_FILE_V22);
    CHECK(probe.file_size == logical_size);

    make_v22_header(bytes);
    store_be64(bytes + 16, 17);
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);
    make_v22_header(bytes);
    CHECK(unity_input_probe_bytes(bytes, 20, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(unity_input_probe_bytes(bytes, 47, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(unity_input_probe_bytes(bytes, 19, 64, &probe) == UNITY_INPUT_UNRELATED);

    /* Other versions retain the previous marker-only recognition policy;
     * v22's selector/high-word interpretation does not extend to them. */
    store_be32(bytes + 8, 21);
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.serialized_file_version == 21);
    bytes[17] = 1;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNRELATED);
    make_v22_header(bytes);
    store_be32(bytes + 8, 23);
    bytes[40] = 255;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.serialized_file_version == 23);
    bytes[16] = 2;
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNRELATED);
    store_be32(bytes + 8, 101);
    CHECK(unity_input_probe_bytes(bytes, 48, 64, &probe) == UNITY_INPUT_UNRELATED);
    return 0;
}

static bool count_visitor(const UnitySerializedSource* source, void* context) {
    size_t* count = (size_t*)context;
    if (!source || !source->outer_path || source->member_name ||
        source->is_bundle_member || source->member_index != 0U ||
        !source->data || source->size != 64U) {
        return false;
    }
    ++*count;
    return true;
}

static bool reject_visitor(const UnitySerializedSource* source,
                           void* context) {
    (void)context;
    return source == NULL;
}

#ifndef _WIN32
static bool mutate_visitor(const UnitySerializedSource* source,
                           void* context) {
    (void)context;
    if (!source || !source->outer_path || !source->data ||
        source->size != 64U) {
        return false;
    }
    FILE* file = fopen(source->outer_path, "r+b");
    if (!file || fseek(file, 63L, SEEK_SET) != 0) {
        if (file) (void)fclose(file);
        return false;
    }
    const uint8_t changed = 0x5aU;
    bool ok = fwrite(&changed, 1U, 1U, file) == 1U;
    if (fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}
#endif

int main(void) {
    CHECK(test_serialized_header_layout() == 0);
    UnityInputProbe probe;
    static const uint8_t unrelated[] = "ordinary file";
    CHECK(unity_input_probe_bytes(
              unrelated, sizeof(unrelated), sizeof(unrelated), &probe) ==
          UNITY_INPUT_UNRELATED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNRELATED);

    static const uint8_t unityfs[] = "UnityFS\0rest";
    CHECK(unity_input_probe_bytes(
              unityfs, sizeof(unityfs), sizeof(unityfs), &probe) ==
          UNITY_INPUT_OK);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNITYFS);

    static const uint8_t unityraw[] = "UnityRaw\0rest";
    CHECK(unity_input_probe_bytes(
              unityraw, sizeof(unityraw), sizeof(unityraw), &probe) ==
          UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_UNITY_ARCHIVE);

    uint8_t serialized[64];
    make_v22_header(serialized);
    CHECK(unity_input_probe_bytes(
              serialized, sizeof(serialized), sizeof(serialized), &probe) ==
          UNITY_INPUT_OK);
    CHECK(probe.kind == UNITY_INPUT_KIND_SERIALIZED_FILE_V22);
    CHECK(probe.serialized_file_version == 22U);
    CHECK(probe.file_size == sizeof(serialized));
    CHECK(unity_input_probe_bytes(
              serialized, sizeof(serialized), sizeof(serialized) + 1U,
              &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);
    CHECK(probe.serialized_file_version == 22U);

    uint8_t corrupt_v22[64];
    make_v22_header(corrupt_v22);
    store_be64(corrupt_v22 + 32U, 65U);
    CHECK(unity_input_probe_bytes(
              corrupt_v22, sizeof(corrupt_v22), sizeof(corrupt_v22),
              &probe) == UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);

    uint8_t legacy[64] = {0};
    store_be32(legacy + 4U, sizeof(legacy));
    store_be32(legacy + 8U, 21U);
    store_be32(legacy + 12U, 32U);
    CHECK(unity_input_probe_bytes(
              legacy, sizeof(legacy), sizeof(legacy), &probe) ==
          UNITY_INPUT_UNSUPPORTED);
    CHECK(probe.kind == UNITY_INPUT_KIND_UNSUPPORTED_SERIALIZED_FILE);

    char path[256];
    int written = snprintf(path, sizeof(path), "unity_common_input_%lu.bin",
                           TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(path));
    (void)remove(path);
    CHECK(common_file_write_new_atomic(
              path, serialized, sizeof(serialized)) == COMMON_FILE_OK);
    CHECK(unity_input_probe_path(path, &probe) == UNITY_INPUT_OK);
    CHECK(probe.kind == UNITY_INPUT_KIND_SERIALIZED_FILE_V22);
    size_t visited = 0U;
    UnityInputVisitStats stats;
    CHECK(unity_input_visit_serialized(
              path, count_visitor, &visited, &stats) == UNITY_INPUT_OK);
    CHECK(visited == 1U && stats.serialized_files == 1U);
    CHECK(stats.resource_members == 0U && stats.directory_members == 0U &&
          stats.deleted_members == 0U);
    memset(&stats, 0xff, sizeof(stats));
    CHECK(unity_input_visit_serialized(
              path, reject_visitor, NULL, &stats) ==
          UNITY_INPUT_VISITOR_FAILED);
    CHECK(stats.serialized_files == 0U && stats.resource_members == 0U &&
          stats.directory_members == 0U && stats.deleted_members == 0U);
#ifndef _WIN32
    memset(&stats, 0xff, sizeof(stats));
    CHECK(unity_input_visit_serialized(
              path, mutate_visitor, NULL, &stats) ==
          UNITY_INPUT_FILE_ERROR);
    CHECK(stats.serialized_files == 0U && stats.resource_members == 0U &&
          stats.directory_members == 0U && stats.deleted_members == 0U);
#endif

    /* A retained snapshot may be visited repeatedly, but closing it must
     * reject even a byte-identical pathname replacement. Content hashing
     * alone cannot prove this stable-source identity contract. */
    CHECK(remove(path) == 0);
    CHECK(common_file_write_new_atomic(
              path, serialized, sizeof(serialized)) == COMMON_FILE_OK);
    UnityInputSnapshot snapshot;
    unity_input_snapshot_init(&snapshot);
    uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t captured_digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    uint8_t zero_digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    common_sha256(serialized, sizeof(serialized), expected_digest);
    CHECK(unity_input_snapshot_digest(NULL, captured_digest) ==
          UNITY_INPUT_INVALID_ARGUMENT);
    CHECK(unity_input_snapshot_digest(&snapshot, captured_digest) ==
          UNITY_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(captured_digest, zero_digest, sizeof(captured_digest)) == 0);
    CHECK(unity_input_snapshot_open(path, &snapshot) == UNITY_INPUT_OK);
    CHECK(unity_input_snapshot_digest(&snapshot, NULL) ==
          UNITY_INPUT_INVALID_ARGUMENT);
    CHECK(unity_input_snapshot_digest(&snapshot, captured_digest) == UNITY_INPUT_OK);
    CHECK(memcmp(captured_digest, expected_digest, sizeof(captured_digest)) == 0);
    CHECK(unity_input_snapshot_is_open(&snapshot));
    CHECK(unity_input_snapshot_path(&snapshot) != NULL);
    CHECK(strstr(unity_input_snapshot_path(&snapshot), path) != NULL);
    visited = 0U;
    CHECK(unity_input_snapshot_visit(
              &snapshot, count_visitor, &visited, &stats) ==
          UNITY_INPUT_OK);
    CHECK(visited == 1U && stats.serialized_files == 1U);
    CHECK(unity_input_snapshot_suspend_mapping(&snapshot) ==
          UNITY_INPUT_OK);
    CHECK(unity_input_snapshot_is_open(&snapshot));
    memset(captured_digest, 0, sizeof(captured_digest));
    CHECK(unity_input_snapshot_digest(&snapshot, captured_digest) == UNITY_INPUT_OK);
    CHECK(memcmp(captured_digest, expected_digest, sizeof(captured_digest)) == 0);
    CHECK(unity_input_snapshot_visit(
              &snapshot, count_visitor, &visited, &stats) ==
          UNITY_INPUT_OK);
    CHECK(visited == 2U && stats.serialized_files == 1U);
    CHECK(test_replace_regular_file(path, serialized, sizeof(serialized)));
    memset(captured_digest, 0, sizeof(captured_digest));
    CHECK(unity_input_snapshot_digest(&snapshot, captured_digest) == UNITY_INPUT_FILE_ERROR);
    CHECK(memcmp(captured_digest, zero_digest, sizeof(captured_digest)) == 0);
    CHECK(unity_input_snapshot_close(&snapshot) ==
          UNITY_INPUT_FILE_ERROR);
    CHECK(!unity_input_snapshot_is_open(&snapshot));
    CHECK(unity_input_snapshot_path(&snapshot) == NULL);
    CHECK(remove(path) == 0);

#ifndef _WIN32
    /* A retained writable mapping can change bytes while filesystem times
     * remain stale. The content digest bound at snapshot-open must reject a
     * changed source before a suspended mapping can be recreated. */
    CHECK(common_file_write_new_atomic(
              path, serialized, sizeof(serialized)) == COMMON_FILE_OK);
    int writer_descriptor = open(path, O_RDWR);
    CHECK(writer_descriptor >= 0);
    uint8_t* writer_mapping = (uint8_t*)mmap(
        NULL, sizeof(serialized), PROT_READ | PROT_WRITE, MAP_SHARED,
        writer_descriptor, 0);
    CHECK(writer_mapping != MAP_FAILED && writer_mapping != NULL);
    unity_input_snapshot_init(&snapshot);
    CHECK(unity_input_snapshot_open(path, &snapshot) == UNITY_INPUT_OK);
    CHECK(unity_input_snapshot_suspend_mapping(&snapshot) == UNITY_INPUT_OK);
    writer_mapping[63] ^= 0x5aU;
    CHECK(unity_input_snapshot_digest(&snapshot, captured_digest) == UNITY_INPUT_FILE_ERROR);
    CHECK(memcmp(captured_digest, zero_digest, sizeof(captured_digest)) == 0);
    visited = 0U;
    CHECK(unity_input_snapshot_visit(
              &snapshot, count_visitor, &visited, &stats) ==
          UNITY_INPUT_FILE_ERROR);
    CHECK(visited == 0U);
    CHECK(unity_input_snapshot_close(&snapshot) == UNITY_INPUT_FILE_ERROR);
    CHECK(munmap(writer_mapping, sizeof(serialized)) == 0);
    CHECK(close(writer_descriptor) == 0);
    CHECK(remove(path) == 0);

    /* The user-visible and remap path is fixed at open, so another thread's
     * process-wide cwd change cannot redirect a retained snapshot. */
    char original_directory[4096];
    CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    char directory_a[256];
    char directory_b[256];
    char file_a[512];
    char file_b[512];
    CHECK(snprintf(directory_a, sizeof(directory_a),
                   "unity_common_input_%lu_a", TEST_PROCESS_ID()) > 0);
    CHECK(snprintf(directory_b, sizeof(directory_b),
                   "unity_common_input_%lu_b", TEST_PROCESS_ID()) > 0);
    CHECK(snprintf(file_a, sizeof(file_a), "%s/source.assets",
                   directory_a) > 0);
    CHECK(snprintf(file_b, sizeof(file_b), "%s/source.assets",
                   directory_b) > 0);
    (void)remove(file_a);
    (void)remove(file_b);
    (void)rmdir(directory_a);
    (void)rmdir(directory_b);
    CHECK(mkdir(directory_a, 0700) == 0);
    CHECK(mkdir(directory_b, 0700) == 0);
    CHECK(common_file_write_new_atomic(
              file_a, serialized, sizeof(serialized)) == COMMON_FILE_OK);
    uint8_t other_serialized[sizeof(serialized)];
    memcpy(other_serialized, serialized, sizeof(other_serialized));
    other_serialized[63] ^= 0xa5U;
    CHECK(common_file_write_new_atomic(
              file_b, other_serialized, sizeof(other_serialized)) ==
          COMMON_FILE_OK);
    CHECK(chdir(directory_a) == 0);
    unity_input_snapshot_init(&snapshot);
    CHECK(unity_input_snapshot_open("source.assets", &snapshot) ==
          UNITY_INPUT_OK);
    CHECK(chdir(original_directory) == 0);
    CHECK(chdir(directory_b) == 0);
    visited = 0U;
    CHECK(unity_input_snapshot_visit(
              &snapshot, count_visitor, &visited, &stats) == UNITY_INPUT_OK);
    CHECK(visited == 1U);
    CHECK(unity_input_snapshot_close(&snapshot) == UNITY_INPUT_OK);
    CHECK(chdir(original_directory) == 0);
    CHECK(remove(file_a) == 0);
    CHECK(remove(file_b) == 0);
    CHECK(rmdir(directory_a) == 0);
    CHECK(rmdir(directory_b) == 0);
#endif

    CHECK(strcmp(unity_input_kind_name(UNITY_INPUT_KIND_UNITYFS),
                 "unityfs") == 0);
    CHECK(strcmp(unity_input_status_name(UNITY_INPUT_MEMBER_INVALID),
                 "member-invalid") == 0);
    puts("Unity input unit tests passed.");
    return 0;
}
