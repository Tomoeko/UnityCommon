#include "serialized_schema_context_fixture.h"

#include "common/sha256.h"

#include <stdio.h>
#include <string.h>

bool schema_context_fixture_hash_matches(
    const SchemaContextFixture* fixture, const char* expected_sha256) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hexadecimal[COMMON_SHA256_HEX_SIZE];
    common_sha256(fixture->file.data, fixture->file.size, digest);
    common_sha256_digest_to_hex(digest, hexadecimal);
    if (strcmp(hexadecimal, expected_sha256) != 0) {
        fprintf(stderr, "Context fixture hash mismatch: %s\n", hexadecimal);
        return false;
    }
    return true;
}

bool schema_context_fixture_load(const char* path,
    size_t expected_size,
    const char* expected_sha256,
    SchemaContextFixture* fixture) {
    if (common_file_read_regular(path, SCHEMA_CONTEXT_FIXTURE_MAX_BYTES, &fixture->file) !=
        COMMON_FILE_OK) {
        fprintf(stderr, "Could not read bounded context fixture: %s\n", path);
        return false;
    }
    if (fixture->file.size != expected_size ||
        !schema_context_fixture_hash_matches(fixture, expected_sha256)) {
        common_file_bytes_dispose(&fixture->file);
        return false;
    }
    return true;
}

bool schema_context_fixture_prepare(SchemaContextFixture* fixture) {
    const SerializedFileDirectoryLimits directory_limits = {12U,
        8U,
        32U,
        1024U,
        SCHEMA_CONTEXT_FIXTURE_MAX_BYTES,
        128U,
        SCHEMA_CONTEXT_FIXTURE_MAX_BYTES,
        65536U,
        0U,
        1048576U};
    const SerializedFileMetadataTailLimits tail_limits = {32U,
        32U,
        8U,
        1024U,
        SCHEMA_CONTEXT_FIXTURE_MAX_BYTES,
        SCHEMA_CONTEXT_FIXTURE_MAX_BYTES,
        SCHEMA_CONTEXT_FIXTURE_MAX_BYTES,
        65536U,
        0U,
        1048576U};
    const SerializedFileDirectoryResult directory_result =
        serialized_file_directory_create(fixture->file.data,
            fixture->file.size,
            fixture->file.size,
            SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
            &directory_limits,
            &fixture->directory);
    if (directory_result.status != SERIALIZED_FILE_DIRECTORY_OK) {
        fprintf(stderr, "Expected-valid context directory rejected: %d\n", directory_result.status);
        return false;
    }
    const SerializedFileMetadataTailResult tail_result =
        serialized_file_metadata_tail_create(&fixture->directory,
            fixture->file.data,
            fixture->file.size,
            fixture->file.size,
            &tail_limits,
            &fixture->tail);
    if (tail_result.status != SERIALIZED_FILE_METADATA_TAIL_OK) {
        fprintf(stderr, "Expected-valid context metadata tail rejected: %d\n", tail_result.status);
        return false;
    }
    return true;
}

bool schema_context_fixture_schema(const SchemaContextFixture* fixture,
    bool reference,
    size_t ordinal,
    SerializedFileSchema* out_schema) {
    const SerializedFileSchemaLimits limits = {
        256U, SCHEMA_CONTEXT_FIXTURE_MAX_BYTES, 255U, 1048576U, 1048576U, UINT64_C(16777216)};
    const SerializedFileSchemaResult result = reference
        ? serialized_file_schema_create_reference(&fixture->tail, ordinal, &limits, out_schema)
        : serialized_file_schema_create_ordinary(&fixture->directory, ordinal, &limits, out_schema);
    if (result.status != SERIALIZED_FILE_SCHEMA_OK) {
        fprintf(stderr, "Expected-valid context schema rejected: %d\n", result.status);
        return false;
    }
    return true;
}

void schema_context_fixture_dispose_parents(SchemaContextFixture* fixture) {
    serialized_file_metadata_tail_dispose(&fixture->tail);
    serialized_file_directory_dispose(&fixture->directory);
}

void schema_context_fixture_dispose(SchemaContextFixture* fixture) {
    schema_context_fixture_dispose_parents(fixture);
    common_file_bytes_dispose(&fixture->file);
}
