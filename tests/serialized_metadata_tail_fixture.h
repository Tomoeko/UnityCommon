#ifndef SERIALIZED_METADATA_TAIL_FIXTURE_H
#define SERIALIZED_METADATA_TAIL_FIXTURE_H

#include "io/serialized_file_metadata_tail.h"

#include <stdalign.h>

enum {
    TAIL_FIXTURE_CAPACITY = 2048,
    TAIL_FIXTURE_START = 73,
    TAIL_FIXTURE_EVENTS = 192,
    TAIL_FIXTURE_SCRIPTS = 2,
    TAIL_FIXTURE_EXTERNALS = 2,
    TAIL_FIXTURE_REFERENCES = 3
};

/* Construction records the physical field/charge schedule. Tests never decode
 * the product's rows to manufacture expected boundaries or work diagnostics. */
typedef struct TailFixtureEvent {
    size_t offset;
    size_t size;
    uint64_t work;
    SerializedFileMetadataTailField field;
    size_t row;
    bool terminated_byte;
} TailFixtureEvent;

/* Align forged overlap arguments for the concrete types used by the tests. */
typedef union TailFixtureAlignment {
    SerializedFileMetadataTail output;
    SerializedFileMetadataTailLimits limits;
} TailFixtureAlignment;

typedef struct TailFixture {
    alignas(TailFixtureAlignment) uint8_t bytes[TAIL_FIXTURE_CAPACITY];
    size_t end;
    uint64_t logical_size;
    bool big_endian;
    bool tree;
    SerializedFileDirectoryEngineVersion version;
    SerializedFilePrefixSpan counts[3];
    SerializedFilePrefixSpan tables[3];
    SerializedFilePrefixSpan information;
    SerializedFileMetadataTailScriptRow scripts[TAIL_FIXTURE_SCRIPTS];
    SerializedFileMetadataTailExternalRow externals[TAIL_FIXTURE_EXTERNALS];
    SerializedFileMetadataTailReferenceTypeRow references[TAIL_FIXTURE_REFERENCES];
    size_t script_count;
    size_t external_count;
    size_t reference_count;
    uint64_t nodes;
    uint64_t tree_strings;
    uint64_t terminated_strings;
    TailFixtureEvent events[TAIL_FIXTURE_EVENTS];
    size_t event_count;
} TailFixture;

void tail_fixture_store(uint8_t* bytes, size_t width, uint64_t value, bool big_endian);
void tail_fixture_header(TailFixture* fixture, uint64_t metadata_end, uint64_t logical_size);
void tail_fixture_build(TailFixture* fixture,
    bool big_endian,
    bool tree,
    SerializedFileDirectoryEngineVersion version,
    bool empty);
bool tail_fixture_parent_at(
    const TailFixture* fixture, const uint8_t* bytes, SerializedFileDirectory* out_parent);
bool tail_fixture_parent(const TailFixture* fixture, SerializedFileDirectory* out_parent);

#endif
