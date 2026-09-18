#ifndef SERIALIZED_SCHEMA_CONTEXT_FIXTURE_H
#define SERIALIZED_SCHEMA_CONTEXT_FIXTURE_H

#include "io/serialized_file_schema_context.h"

#include "common/file_io.h"

enum {
    SCHEMA_CONTEXT_FIXTURE_MAX_BYTES = 8192
};

typedef struct SchemaContextFixture {
    CommonFileBytes file;
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
} SchemaContextFixture;

/* Setup only. Context expectations come from independent pinned observations,
 * never from these product owners. Call with a zero-initialized fixture. */
bool schema_context_fixture_load(const char* path,
    size_t expected_size,
    const char* expected_sha256,
    SchemaContextFixture* fixture);
bool schema_context_fixture_hash_matches(
    const SchemaContextFixture* fixture, const char* expected_sha256);
bool schema_context_fixture_prepare(SchemaContextFixture* fixture);
bool schema_context_fixture_schema(const SchemaContextFixture* fixture,
    bool reference,
    size_t ordinal,
    SerializedFileSchema* out_schema);
void schema_context_fixture_dispose_parents(SchemaContextFixture* fixture);
void schema_context_fixture_dispose(SchemaContextFixture* fixture);

#endif
