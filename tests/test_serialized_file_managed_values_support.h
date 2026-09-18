#ifndef TEST_SERIALIZED_FILE_MANAGED_VALUES_SUPPORT_H
#define TEST_SERIALIZED_FILE_MANAGED_VALUES_SUPPORT_H

#include "io/serialized_file_managed_values.h"

#include "common/file_io.h"

#include <stdbool.h>

enum {
    MANAGED_WRITER_HOST_COUNT = 9,
    MANAGED_WRITER_FILE_SIZE = 6528
};

typedef struct ManagedExpectedValue {
    size_t node;
    size_t type; /* SIZE_MAX is the ordinary host schema. */
    size_t row;
    SerializedFileManagedValueKind kind;
    size_t offset;
    size_t size;
    size_t padding; /* SIZE_MAX when alignment is absent. */
    uint64_t bits;
    const char* string;
} ManagedExpectedValue;

typedef struct ManagedExpectedRow {
    size_t offset;
    size_t size;
    size_t payload_offset;
    size_t payload_size;
    uint64_t rid;
    size_t type;
    size_t selected;
    size_t first_value;
    size_t value_count;
} ManagedExpectedRow;

typedef struct ManagedExpectedSlot {
    size_t value;
    size_t row;
    uint64_t rid;
    size_t first_match;
    size_t match_count;
    bool is_null;
} ManagedExpectedSlot;

typedef struct ManagedExpectedHost {
    const char* name;
    uint64_t path_id;
    size_t offset;
    size_t size;
    size_t registry_offset;
    size_t registry_size;
    size_t array_count;
    size_t first_value;
    size_t value_count;
    size_t first_row;
    size_t row_count;
    size_t first_slot;
    size_t slot_count;
    size_t selected_count;
} ManagedExpectedHost;

extern const ManagedExpectedHost managed_expected_hosts[MANAGED_WRITER_HOST_COUNT];
extern const ManagedExpectedValue managed_expected_values[];
extern const ManagedExpectedRow managed_expected_rows[];
extern const ManagedExpectedSlot managed_expected_slots[];

typedef struct ManagedTestParents {
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
} ManagedTestParents;

bool managed_test_read_fixture(
    const char* common_path, const char* file_path, CommonFileBytes* file);
bool managed_test_parents_create(const uint8_t* bytes, size_t size, ManagedTestParents* parents);
void managed_test_parents_dispose(ManagedTestParents* parents);
SerializedFileManagedValuesLimits managed_test_limits(void);
uint64_t managed_test_read_le(const uint8_t* bytes, size_t width);
void managed_test_write_le(uint8_t* bytes, size_t width, uint64_t value);
void managed_test_write_be(uint8_t* bytes, size_t width, uint64_t value);
bool managed_test_span(
    SerializedFilePrefixSpan span, const uint8_t* bytes, size_t offset, size_t size);
bool managed_test_absent(SerializedFilePrefixSpan span);

#endif
