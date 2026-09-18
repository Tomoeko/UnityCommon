#include "io/serialized_file_managed_values.h"

#define MANAGED_VALUES_ALLOCATION_DECLARATIONS_ONLY
#include "test_serialized_file_managed_values_allocation.h"
#include "test_serialized_file_managed_values_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct TestAllocation {
    void* pointer;
    size_t size;
} TestAllocation;

static TestAllocation allocations[32];
static size_t allocation_calls;
static size_t fail_allocation;
static size_t live_allocations;
static size_t live_bytes;
static size_t peak_live_bytes;
static size_t requested_sizes[32];

void* managed_values_test_allocate(size_t size) {
    if (allocation_calls < sizeof(requested_sizes) / sizeof(requested_sizes[0])) {
        requested_sizes[allocation_calls] = size;
    }
    ++allocation_calls;
    if (allocation_calls == fail_allocation) {
        return NULL;
    }
    void* allocation = malloc(size);
    if (!allocation) {
        return NULL;
    }
    for (size_t index = 0U; index < sizeof(allocations) / sizeof(allocations[0]); ++index) {
        if (!allocations[index].pointer) {
            allocations[index] = (TestAllocation){allocation, size};
            ++live_allocations;
            live_bytes += size;
            if (live_bytes > peak_live_bytes) {
                peak_live_bytes = live_bytes;
            }
            return allocation;
        }
    }
    fprintf(stderr, "Managed-value test allocation tracker capacity exceeded\n");
    abort();
}

void managed_values_test_release(void* allocation) {
    if (!allocation) {
        return;
    }
    for (size_t index = 0U; index < sizeof(allocations) / sizeof(allocations[0]); ++index) {
        if (allocations[index].pointer == allocation) {
            live_bytes -= allocations[index].size;
            --live_allocations;
            allocations[index] = (TestAllocation){0};
            free(allocation);
            return;
        }
    }
    fprintf(stderr, "Managed-value test released an unowned allocation\n");
    abort();
}

static void reset_allocation_observer(void) {
    if (live_allocations != 0U || live_bytes != 0U) {
        fprintf(stderr, "Managed-value test reset with live allocations\n");
        abort();
    }
    allocation_calls = 0U;
    fail_allocation = 0U;
    peak_live_bytes = 0U;
    memset(requested_sizes, 0, sizeof(requested_sizes));
}

static size_t field_offset(size_t host, size_t node, size_t type, size_t row) {
    const ManagedExpectedHost* expected = &managed_expected_hosts[host];
    for (size_t index = 0U; index < expected->value_count; ++index) {
        const ManagedExpectedValue* value = &managed_expected_values[expected->first_value + index];
        if (value->node == node && value->type == type && value->row == row) {
            return value->offset;
        }
    }
    fprintf(stderr, "Independent fixture field was not found\n");
    abort();
}

static void set_object_size(uint8_t* bytes, size_t host, size_t size) {
    /* Original object directory cells are independent writer observations. */
    managed_test_write_le(bytes + 1824U + host * 24U, 4U, size);
}

static SerializedFileManagedValuesResult create_values(const uint8_t* bytes,
    const ManagedTestParents* parents,
    size_t host,
    const SerializedFileManagedValuesLimits* limits,
    SerializedFileManagedValues* owner) {
    return serialized_file_managed_values_create(
        &parents->directory, &parents->tail, host, bytes, MANAGED_WRITER_FILE_SIZE, limits, owner);
}

static bool expect_failure(uint8_t* bytes, size_t host, SerializedFileManagedValuesStatus status) {
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, MANAGED_WRITER_FILE_SIZE, &parents));
    uint8_t before[MANAGED_WRITER_FILE_SIZE];
    memcpy(before, bytes, sizeof(before));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    const SerializedFileManagedValuesResult result =
        create_values(bytes, &parents, host, &limits, &owner);
    managed_test_parents_dispose(&parents);
    if (result.status != status) {
        fprintf(stderr,
            "Expected status %d, received %d, field %d, offset %llu\n",
            status,
            result.status,
            result.field,
            (unsigned long long)result.error_offset);
        serialized_file_managed_values_dispose(&owner);
        return false;
    }
    CHECK(!owner.implementation && live_allocations == 0U && live_bytes == 0U);
    if (status == SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT) {
        const uint64_t declared_size = managed_test_read_le(bytes + 1824U + host * 24U, 4U);
        CHECK(result.error_offset == managed_expected_hosts[host].offset + declared_size);
    }
    if (status == SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_TERMINATOR) {
        CHECK(!result.query_attempted);
    } else if (status == SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_AMBIGUOUS) {
        CHECK(result.query_attempted && result.query_registry_row_ordinal == 1U &&
            result.type_match.kind == SERIALIZED_FILE_REFERENCE_AMBIGUOUS &&
            result.type_match.match_count == 2U && result.type_match.first_ordinal == 0U &&
            result.type_match.second_ordinal == 1U);
    } else if (status == SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_MISSING) {
        CHECK(result.query_attempted && result.query_registry_row_ordinal == 1U &&
            result.type_match.kind == SERIALIZED_FILE_REFERENCE_MISSING &&
            result.type_match.match_count == 0U && result.type_match.first_ordinal == SIZE_MAX &&
            result.type_match.second_ordinal == SIZE_MAX);
    }
    CHECK(memcmp(before, bytes, sizeof(before)) == 0);
    return true;
}

static bool raw_domains_and_exhaustion(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];

    const struct Mutation {
        size_t offset;
        size_t width;
        uint64_t bits;
        SerializedFileManagedValuesStatus status;
    } mutations[] = {{5064U, 4U, 1U, SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_REGISTRY_VERSION},
        {5068U, 4U, UINT32_C(0x80000000), SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_COUNT},
        {5068U, 4U, UINT32_MAX, SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_COUNT},
        {4996U, 4U, UINT32_C(0x80000000), SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_STRING_LENGTH},
        {5068U, 4U, 2U, SERIALIZED_FILE_MANAGED_VALUES_TRAILING_OBJECT_BYTES},
        {5068U, 4U, 4U, SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT},
        {5092U, 8U, UINT64_MAX - 1U, SERIALIZED_FILE_MANAGED_VALUES_OK}};

    for (size_t index = 0U; index < sizeof(mutations) / sizeof(mutations[0]); ++index) {
        memcpy(bytes, original->data, sizeof(bytes));
        managed_test_write_le(
            bytes + mutations[index].offset, mutations[index].width, mutations[index].bits);
        if (mutations[index].status == SERIALIZED_FILE_MANAGED_VALUES_OK) {
            ManagedTestParents parents;
            CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
            SerializedFileManagedValues owner;
            serialized_file_managed_values_init(&owner);
            const SerializedFileManagedValuesLimits limits = managed_test_limits();
            const SerializedFileManagedValuesResult result =
                create_values(bytes, &parents, 3U, &limits, &owner);
            CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
            const SerializedFileManagedRegistryRow* row =
                serialized_file_managed_values_registry_row(&owner, 1U);
            CHECK(row && row->rid_bits == UINT64_MAX - 1U &&
                row->kind == SERIALIZED_FILE_MANAGED_REGISTRY_SELECTED_PAYLOAD &&
                row->payload_source.size == 32U);
            serialized_file_managed_values_dispose(&owner);
            managed_test_parents_dispose(&parents);
        } else {
            CHECK(expect_failure(bytes, 3U, mutations[index].status));
        }
    }
    memcpy(bytes, original->data, sizeof(bytes));
    const size_t items = field_offset(3U, 22U, SIZE_MAX, SIZE_MAX);
    managed_test_write_le(bytes + items, 4U, UINT32_C(0x80000000));
    CHECK(expect_failure(bytes, 3U, SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_COUNT));
    /* Shorten only the declared extent. No file copy or physical truncation is
     * needed to exercise every object-end boundary through both selected types.
     */
    for (size_t extent = 0U; extent < 324U; ++extent) {
        memcpy(bytes, original->data, sizeof(bytes));
        set_object_size(bytes, 3U, extent);
        CHECK(expect_failure(bytes, 3U, SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT));
    }
    memcpy(bytes, original->data, sizeof(bytes));
    set_object_size(bytes, 3U, 325U);
    CHECK(expect_failure(bytes, 3U, SERIALIZED_FILE_MANAGED_VALUES_TRAILING_OBJECT_BYTES));
    return true;
}

static bool null_index_and_type_policies(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    memcpy(bytes, original->data, sizeof(bytes));
    managed_test_write_le(bytes + 4504U, 8U, 17U);
    CHECK(expect_failure(bytes, 0U, SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_NULL_ROW));
    memcpy(bytes, original->data, sizeof(bytes));
    bytes[4872U] = 'p';
    CHECK(expect_failure(bytes, 2U, SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_MISSING));

    /* The catalogue remains genuine and nonempty when the wire registry is empty.
     */
    memcpy(bytes, original->data, sizeof(bytes));
    managed_test_write_le(bytes + 4500U, 4U, 0U);
    set_object_size(bytes, 0U, 72U);
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    SerializedFileManagedValuesLimits limits = managed_test_limits();
    limits.max_registry_rows = 0U;
    limits.max_selected_types = 0U;
    limits.max_array_elements = 0U;
    const SerializedFileManagedValuesResult empty =
        create_values(bytes, &parents, 0U, &limits, &owner);
    CHECK(empty.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const SerializedFileManagedValuesView* view = serialized_file_managed_values_view(&owner);
    CHECK(view && view->registry_row_count == 0U && view->selected_type_count == 0U &&
        view->registry_count_bits == 0U && view->registry_source.size == 8U &&
        view->rid_slot_count == 2U && view->missing_rid_slots == 2U);
    CHECK(managed_test_absent(view->registry_padding_source));
    serialized_file_managed_values_dispose(&owner);
    managed_test_parents_dispose(&parents);

    /* Empty metadata index is distinct from the admitted zero wire count. */
    managed_test_write_le(bytes + 2098U, 4U, 0U);
    bytes[2102U] = 0U;
    managed_test_write_be(bytes + 16U, 8U, 2103U - 48U);
    CHECK(expect_failure(bytes, 0U, SERIALIZED_FILE_MANAGED_VALUES_EMPTY_REFERENCE_INDEX));

    /* Shrink only the first original type identity to three empty C strings.
     * The null-looking wire tuple now selects a real payload schema, so skipping
     * it as null would incorrectly hide a missing payload. */
    memcpy(bytes, original->data, sizeof(bytes));
    memmove(bytes + 3257U, bytes + 3311U, 4425U - 3311U);
    memset(bytes + 3254U, 0, 3U);
    managed_test_write_be(bytes + 16U, 8U, 4425U - 54U - 48U);
    CHECK(expect_failure(bytes, 0U, SERIALIZED_FILE_MANAGED_VALUES_TRUNCATED_OBJECT));

    /* Two original reference rows with identical tuple bytes remain ambiguous. */
    memcpy(bytes, original->data, sizeof(bytes));
    memmove(bytes + 4409U, bytes + 4408U, 4425U - 4408U);
    memcpy(bytes + 4376U, "UnityRecoverManagedFixture.Alpha", 33U);
    managed_test_write_be(bytes + 16U, 8U, 4426U - 48U);
    CHECK(expect_failure(bytes, 2U, SERIALIZED_FILE_MANAGED_VALUES_REFERENCE_TYPE_AMBIGUOUS));
    return true;
}

static size_t write_string(uint8_t* bytes, size_t cursor, const char* string) {
    const size_t length = strlen(string);
    managed_test_write_le(bytes + cursor, 4U, length);
    cursor += 4U;
    memcpy(bytes + cursor, string, length);
    cursor += length;
    const size_t padding = (4U - cursor % 4U) % 4U;
    memset(bytes + cursor, 0, padding);
    return cursor + padding;
}

static bool terminator_and_duplicates(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    memcpy(bytes, original->data, sizeof(bytes));
    size_t cursor = 6424U;
    cursor = write_string(bytes, cursor, "Terminus");
    cursor = write_string(bytes, cursor, "UnityEngine.DMAT");
    cursor = write_string(bytes, cursor, "FAKE_ASM");
    CHECK(cursor < sizeof(bytes));
    CHECK(expect_failure(bytes, 8U, SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_TERMINATOR));
    memcpy(bytes, original->data, sizeof(bytes));
    managed_test_write_le(bytes + 6012U, 8U, 101U);
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    const SerializedFileManagedValuesResult result =
        create_values(bytes, &parents, 6U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const SerializedFileManagedValuesView* view = serialized_file_managed_values_view(&owner);
    CHECK(view && view->registry_row_count == 2U && view->selected_type_count == 2U &&
        view->ambiguous_rid_slots == 2U && view->missing_rid_slots == 2U);
    const size_t counts[] = {2U, 0U, 0U, 2U};
    for (size_t index = 0U; index < 4U; ++index) {
        const SerializedFileManagedRidSlot* slot =
            serialized_file_managed_values_rid_slot(&owner, index);
        CHECK(slot && slot->match_count == counts[index]);
        CHECK(slot->resolution ==
            (counts[index] == 0U ? SERIALIZED_FILE_MANAGED_RID_MISSING
                                 : SERIALIZED_FILE_MANAGED_RID_AMBIGUOUS));
        CHECK(slot->first_registry_row == (counts[index] == 0U ? SIZE_MAX : 0U));
        CHECK(slot->second_registry_row == (counts[index] == 0U ? SIZE_MAX : 1U));
    }
    serialized_file_managed_values_dispose(&owner);
    managed_test_parents_dispose(&parents);
    CHECK(live_allocations == 0U);
    return true;
}

static bool float_bits_and_padding(const CommonFileBytes* original) {
    /* These are raw byte-retention mutations, not Editor roundtrip observations.
     */
    static const uint32_t raw_float_bits[] = {0U,
        UINT32_C(0x80000000),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc12345),
        UINT32_C(0x7f812345),
        UINT32_C(0xffcabcde)};
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    for (size_t mutation = 0U; mutation < sizeof(raw_float_bits) / sizeof(raw_float_bits[0]);
        ++mutation) {
        memcpy(bytes, original->data, sizeof(bytes));
        managed_test_write_le(bytes + 6516U, 4U, raw_float_bits[mutation]);
        /* Neither scalar nor string padding carries a zero-content requirement. */
        bytes[6341U] = 0xa5U;
        bytes[6342U] = 0xffU;
        bytes[6343U] = 0x80U;
        ManagedTestParents parents;
        CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
        SerializedFileManagedValues owner;
        serialized_file_managed_values_init(&owner);
        const SerializedFileManagedValuesLimits limits = managed_test_limits();
        const SerializedFileManagedValuesResult result =
            create_values(bytes, &parents, 8U, &limits, &owner);
        CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
        const SerializedFileManagedValuesView* view = serialized_file_managed_values_view(&owner);
        size_t float_count = 0U;
        for (size_t index = 0U; index < view->value_count; ++index) {
            const SerializedFileManagedValue* value =
                serialized_file_managed_values_value(&owner, index);
            if (value->kind == SERIALIZED_FILE_MANAGED_VALUE_FLOAT32) {
                ++float_count;
                CHECK(value->raw_bits == raw_float_bits[mutation]);
                CHECK(managed_test_span(value->scalar_source, bytes, 6516U, 4U));
                CHECK(managed_test_absent(value->padding_source));
            }
        }
        CHECK(float_count == 1U);
        serialized_file_managed_values_dispose(&owner);
        managed_test_parents_dispose(&parents);
    }
    return true;
}

static bool packed_unaligned_rids(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    memcpy(bytes, original->data, sizeof(bytes));
    /* A shorter ordinary name moves the remaining supported payload by four
     * bytes. RID array elements and nested slots now cross eight-byte boundaries
     * without admitting any additional array or scalar alignment operation. */
    memmove(bytes + 4772U, bytes + 4776U, 4968U - 4776U);
    CHECK(write_string(bytes, 4764U, "Z") == 4772U);
    set_object_size(bytes, 2U, 228U);
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    const SerializedFileManagedValuesResult result =
        create_values(bytes, &parents, 2U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const SerializedFileManagedValuesView* view = serialized_file_managed_values_view(&owner);
    CHECK(view && view->array_element_count == 3U && view->consumed_bytes == 228U &&
        view->registry_source.offset == 4828U &&
        managed_test_absent(view->registry_padding_source));
    const size_t offsets[] = {4784U, 4792U, 4804U, 4812U, 4820U, 4956U};
    const uint64_t bits[] = {101U, 101U, 101U, UINT64_MAX - 1U, 101U, UINT64_MAX - 1U};
    for (size_t index = 0U; index < 6U; ++index) {
        const SerializedFileManagedRidSlot* slot =
            serialized_file_managed_values_rid_slot(&owner, index);
        CHECK(slot && slot->rid_bits == bits[index]);
        const SerializedFileManagedValue* value =
            serialized_file_managed_values_value(&owner, slot->value_ordinal);
        CHECK(value && managed_test_span(value->scalar_source, bytes, offsets[index], 8U) &&
            managed_test_absent(value->padding_source));
    }
    serialized_file_managed_values_dispose(&owner);
    managed_test_parents_dispose(&parents);
    return true;
}

static bool unused_schemas_and_authored_names(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    memcpy(bytes, original->data, sizeof(bytes));
    bytes[3392U] = 8U; /* Unsupported Beta hierarchy, never selected by the
                        Alpha-only host. */
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    SerializedFileManagedValuesResult result = create_values(bytes, &parents, 2U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    CHECK(serialized_file_managed_values_view(&owner)->selected_type_count == 1U);
    serialized_file_managed_values_dispose(&owner);
    result = create_values(bytes, &parents, 8U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_SCHEMA_REJECTED && !owner.implementation);
    managed_test_parents_dispose(&parents);

    memcpy(bytes, original->data, sizeof(bytes));
    const char* authored[] = {"Payload", "marker", "label", "amount", "next", "caseTag", "items"};
    for (size_t word = 0U; word < sizeof(authored) / sizeof(authored[0]); ++word) {
        const size_t length = strlen(authored[word]);
        size_t replacements = 0U;
        for (size_t offset = 0U; offset <= sizeof(bytes) - length; ++offset) {
            if (memcmp(bytes + offset, authored[word], length) == 0) {
                bytes[offset] = (uint8_t)'Z';
                ++replacements;
            }
        }
        CHECK(replacements != 0U);
    }
    /* Unity reused common-table encodings for the authored first/second names.
     * Repoint both original name words to the genuine local caseTag start,
     * already renamed above; immutable common-table bytes remain untouched. */
    CHECK(managed_test_read_le(bytes + 636U, 4U) == UINT32_C(0x8000009b));
    CHECK(managed_test_read_le(bytes + 700U, 4U) == UINT32_C(0x8000030a));
    managed_test_write_le(bytes + 636U, 4U, 18U);
    managed_test_write_le(bytes + 700U, 4U, 18U);
    CHECK(memcmp(bytes + 1606U, "ZaseTag", 8U) == 0);
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    for (size_t host = 0U; host < MANAGED_WRITER_HOST_COUNT; ++host) {
        result = create_values(bytes, &parents, host, &limits, &owner);
        CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
        CHECK(serialized_file_managed_values_view(&owner)->consumed_bytes ==
            managed_expected_hosts[host].size);
        serialized_file_managed_values_dispose(&owner);
    }
    managed_test_parents_dispose(&parents);
    return true;
}

static size_t write_empty_alpha_row(uint8_t* bytes, size_t cursor, uint64_t rid, uint64_t next) {
    managed_test_write_le(bytes + cursor, 8U, rid);
    cursor = write_string(bytes, cursor + 8U, "Payload");
    cursor = write_string(bytes, cursor, "UnityRecoverManagedFixture.Alpha");
    cursor = write_string(bytes, cursor, "Assembly-CSharp");
    managed_test_write_le(bytes + cursor, 4U, 1011U);
    cursor = write_string(bytes, cursor + 4U, "");
    managed_test_write_le(bytes + cursor, 8U, next);
    return cursor + 8U;
}

static bool repeated_type_cache_and_host_scope(const CommonFileBytes* original) {
    uint8_t bytes[MANAGED_WRITER_FILE_SIZE];
    memcpy(bytes, original->data, sizeof(bytes));
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    SerializedFileManagedValues first;
    SerializedFileManagedValues second;
    serialized_file_managed_values_init(&first);
    serialized_file_managed_values_init(&second);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    reset_allocation_observer();
    SerializedFileManagedValuesResult result = create_values(bytes, &parents, 2U, &limits, &first);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const size_t single_type_requests = allocation_calls;
    serialized_file_managed_values_dispose(&first);
    result = create_values(bytes, &parents, 7U, &limits, &first);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    result = create_values(bytes, &parents, 8U, &limits, &second);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const SerializedFileManagedValuesView* alpha = serialized_file_managed_values_view(&first);
    const SerializedFileManagedValuesView* beta = serialized_file_managed_values_view(&second);
    CHECK(
        alpha && beta && alpha->object.path_id_bits == 1015U && beta->object.path_id_bits == 1017U);
    CHECK(alpha->object.ordinal != beta->object.ordinal &&
        alpha->payload_source.data != beta->payload_source.data);
    const SerializedFileManagedRidSlot* alpha_slot =
        serialized_file_managed_values_rid_slot(&first, 0U);
    const SerializedFileManagedRidSlot* beta_slot =
        serialized_file_managed_values_rid_slot(&second, 0U);
    CHECK(alpha_slot && beta_slot && alpha_slot->rid_bits == 101U && beta_slot->rid_bits == 101U);
    CHECK(alpha_slot->first_registry_row == 1U && beta_slot->first_registry_row == 1U);
    CHECK(alpha_slot->resolution == SERIALIZED_FILE_MANAGED_RID_UNIQUE &&
        beta_slot->resolution == SERIALIZED_FILE_MANAGED_RID_UNIQUE);
    CHECK(serialized_file_managed_values_selected_type(&first, 0U)->original.ordinal == 0U);
    CHECK(serialized_file_managed_values_selected_type(&second, 0U)->original.ordinal == 1U);
    serialized_file_managed_values_dispose(&first);
    CHECK(serialized_file_managed_values_rid_slot(&second, 0U)->rid_bits == 101U);
    serialized_file_managed_values_dispose(&second);
    managed_test_parents_dispose(&parents);

    /* Two different physical rows select the same original Alpha tree. Empty
     * labels make the controlled replacement fit inside the old host extent.
     * The unrelated Beta tree is intentionally unusable and must stay unused. */
    size_t cursor = write_empty_alpha_row(bytes, 5904U, 101U, 202U);
    cursor = write_empty_alpha_row(bytes, cursor, 202U, 101U);
    CHECK(cursor == 6088U);
    set_object_size(bytes, 6U, cursor - 5816U);
    bytes[3392U] = 8U;
    CHECK(managed_test_parents_create(bytes, sizeof(bytes), &parents));
    reset_allocation_observer();
    result = create_values(bytes, &parents, 6U, &limits, &first);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    CHECK(allocation_calls == single_type_requests);
    CHECK(serialized_file_managed_values_view(&first)->selected_type_count == 1U);
    for (size_t row = 0U; row < 2U; ++row) {
        const SerializedFileManagedRegistryRow* record =
            serialized_file_managed_values_registry_row(&first, row);
        CHECK(record && record->selected_type_ordinal == 0U &&
            record->type_match.first_ordinal == 0U);
    }
    CHECK(serialized_file_managed_values_view(&first)->missing_rid_slots == 0U);
    serialized_file_managed_values_dispose(&first);
    managed_test_parents_dispose(&parents);
    CHECK(live_allocations == 0U);
    return true;
}

static bool source_state_and_mapping(const CommonFileBytes* original) {
    uint8_t identical[MANAGED_WRITER_FILE_SIZE + 1U];
    memcpy(identical, original->data, original->size);
    identical[MANAGED_WRITER_FILE_SIZE] = 0U;
    ManagedTestParents parents;
    ManagedTestParents other;
    CHECK(managed_test_parents_create(original->data, original->size, &parents));
    CHECK(managed_test_parents_create(identical, original->size, &other));
    const SerializedFileManagedValuesLimits limits = managed_test_limits();

    struct GuardedOwner {
        uint64_t before;
        SerializedFileManagedValues owner;
        uint64_t after;
    } guarded = {UINT64_C(0x1728394050607080), {NULL}, UINT64_C(0xfedcba9876543210)};
    const struct GuardedOwner empty = guarded;
    const SerializedFileManagedValuesStorageRange absent =
        serialized_file_managed_values_storage_range(NULL);
    const SerializedFileManagedValuesStorageRange empty_range =
        serialized_file_managed_values_storage_range(&guarded.owner);
    CHECK(!absent.data && absent.size == 0U && !empty_range.data && empty_range.size == 0U);
    SerializedFileManagedValuesResult result =
        serialized_file_managed_values_create(&parents.directory,
            &other.tail,
            3U,
            original->data,
            original->size,
            &limits,
            &guarded.owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_SOURCE_MISMATCH);
    CHECK(memcmp(&guarded, &empty, sizeof(guarded)) == 0);
    result = serialized_file_managed_values_create(
        &parents.directory, &parents.tail, 3U, identical, original->size, &limits, &guarded.owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_SOURCE_MISMATCH);
    CHECK(memcmp(&guarded, &empty, sizeof(guarded)) == 0);
    result = serialized_file_managed_values_create(
        &other.directory, &other.tail, 3U, identical, sizeof(identical), &limits, &guarded.owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_SOURCE_MISMATCH);
    for (size_t size = 4968U; size < 5292U; ++size) {
        result = serialized_file_managed_values_create(
            &parents.directory, &parents.tail, 3U, original->data, size, &limits, &guarded.owner);
        CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_INCOMPLETE_MAPPING);
        CHECK(memcmp(&guarded, &empty, sizeof(guarded)) == 0 && live_allocations == 0U);
    }
    result = serialized_file_managed_values_create(
        &parents.directory, &parents.tail, 3U, original->data, 5292U, &limits, &guarded.owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const void* implementation = guarded.owner.implementation;
    unsigned char snapshot[sizeof(SerializedFileManagedValuesView)];
    memcpy(snapshot, serialized_file_managed_values_view(&guarded.owner), sizeof(snapshot));
    result = create_values(original->data, &parents, 3U, &limits, &guarded.owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_INVALID_STATE && result.work_used == 0U);
    CHECK(guarded.owner.implementation == implementation &&
        memcmp(snapshot, serialized_file_managed_values_view(&guarded.owner), sizeof(snapshot)) ==
            0);
    CHECK(guarded.before == empty.before && guarded.after == empty.after);
    serialized_file_managed_values_dispose(&guarded.owner);
    const SerializedFileManagedValuesStorageRange disposed =
        serialized_file_managed_values_storage_range(&guarded.owner);
    CHECK(!disposed.data && disposed.size == 0U);
    result = create_values(original->data, &parents, SIZE_MAX, &limits, &guarded.owner);
    CHECK(result.status != SERIALIZED_FILE_MANAGED_VALUES_OK && !guarded.owner.implementation);
    serialized_file_managed_values_dispose(NULL);
    serialized_file_managed_values_dispose(&guarded.owner);
    CHECK(!serialized_file_managed_values_view(NULL));
    CHECK(!serialized_file_managed_values_value(NULL, SIZE_MAX));
    CHECK(!serialized_file_managed_values_registry_row(NULL, SIZE_MAX));
    CHECK(!serialized_file_managed_values_selected_type(NULL, SIZE_MAX));
    CHECK(!serialized_file_managed_values_rid_slot(NULL, SIZE_MAX));
    managed_test_parents_dispose(&other);
    managed_test_parents_dispose(&parents);
    CHECK(live_allocations == 0U);
    return true;
}

static bool query_component_boundaries(const CommonFileBytes* original) {
    static const struct QueryBoundary {
        size_t component_bytes;
        size_t key_bytes;
        uint64_t work;
        SerializedFileReferenceIndexLimit limit;
        SerializedFileReferenceIdentityComponent component;
        size_t node;
        uint64_t offset;
        size_t reference_row;
        uint64_t work_used;
    } boundaries[] = {{7U,
                          768U,
                          1000000U,
                          SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_COMPONENT_BYTES,
                          SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE,
                          37U,
                          5112U,
                          SIZE_MAX,
                          1U},
        {256U,
            39U,
            1000000U,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_KEY_BYTES,
            SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY,
            41U,
            5148U,
            SIZE_MAX,
            1U},
        {256U,
            768U,
            11U,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK,
            SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE,
            37U,
            5112U,
            0U,
            11U},
        {256U,
            768U,
            44U,
            SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK,
            SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY,
            41U,
            5148U,
            0U,
            44U}};

    ManagedTestParents parents;
    CHECK(managed_test_parents_create(original->data, original->size, &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    for (size_t index = 0U; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index) {
        const struct QueryBoundary* expected = &boundaries[index];
        SerializedFileManagedValuesLimits limits = managed_test_limits();
        limits.query.max_component_bytes = expected->component_bytes;
        limits.query.max_key_bytes = expected->key_bytes;
        limits.query.max_work = expected->work;
        const SerializedFileManagedValuesResult result =
            create_values(original->data, &parents, 3U, &limits, &owner);
        CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_QUERY_REJECTED &&
            result.limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE &&
            result.field == SERIALIZED_FILE_MANAGED_VALUES_FIELD_REFERENCE_IDENTITY &&
            result.node_ordinal == expected->node && result.error_offset == expected->offset &&
            result.query_attempted && result.query_registry_row_ordinal == 1U);
        const SerializedFileReferenceQueryResult* nested = &result.query_result;
        CHECK(nested->status == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED &&
            nested->limit == expected->limit && nested->component == expected->component &&
            nested->row_ordinal == expected->reference_row &&
            nested->work_used == expected->work_used);
        CHECK(!owner.implementation && live_allocations == 0U && live_bytes == 0U);
    }
    managed_test_parents_dispose(&parents);
    return true;
}

static bool protected_mapping_end(const CommonFileBytes* original) {
#ifndef _WIN32
    const long native_page_size = sysconf(_SC_PAGESIZE);
    CHECK(native_page_size > 0);
    const size_t page_size = (size_t)native_page_size;
    const size_t readable_size = 5292U;
    const size_t page_count = (readable_size + page_size - 1U) / page_size;
    const size_t readable_pages = page_count * page_size;
    const size_t allocation_size = readable_pages + page_size;
    uint8_t* allocation =
        mmap(NULL, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(allocation != MAP_FAILED);
    uint8_t* bytes = allocation + readable_pages - readable_size;
    memcpy(bytes, original->data, readable_size);
    bool passed = false;
    ManagedTestParents parents;
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    if (!managed_test_parents_create(bytes, MANAGED_WRITER_FILE_SIZE, &parents)) {
        munmap(allocation, allocation_size);
        CHECK(false);
    }
    if (mprotect(allocation, readable_pages, PROT_READ) == 0 &&
        mprotect(allocation + readable_pages, page_size, PROT_NONE) == 0) {
        const SerializedFileManagedValuesLimits limits = managed_test_limits();
        const SerializedFileManagedValuesResult result = serialized_file_managed_values_create(
            &parents.directory, &parents.tail, 3U, bytes, readable_size, &limits, &owner);
        passed = result.status == SERIALIZED_FILE_MANAGED_VALUES_OK &&
            serialized_file_managed_values_view(&owner)->consumed_bytes == 324U;
    }
    managed_test_parents_dispose(&parents);
    /* Disposal must not touch borrowed bytes, even when every mapped source is inaccessible. */
    if (mprotect(allocation, readable_pages, PROT_NONE) != 0) {
        passed = false;
    }
    if (passed) {
        const SerializedFileManagedValuesStorageRange storage =
            serialized_file_managed_values_storage_range(&owner);
        passed = storage.data != NULL && storage.size != 0U;
    }
    serialized_file_managed_values_dispose(&owner);
    serialized_file_managed_values_dispose(&owner);
    const int unmapped = munmap(allocation, allocation_size);
    CHECK(passed && unmapped == 0 && live_allocations == 0U);
#else
    (void)original;
#endif
    return true;
}

static bool resource_and_allocation_boundaries(const CommonFileBytes* original) {
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(original->data, original->size, &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    SerializedFileManagedValuesLimits limits = managed_test_limits();
    reset_allocation_observer();
    SerializedFileManagedValuesResult success =
        create_values(original->data, &parents, 3U, &limits, &owner);
    CHECK(success.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    const SerializedFileManagedValuesView view = *serialized_file_managed_values_view(&owner);
    const size_t requests = allocation_calls;
    CHECK(requests == 9U && live_allocations == 1U && live_bytes == view.retained_bytes);
    const SerializedFileManagedValuesStorageRange storage =
        serialized_file_managed_values_storage_range(&owner);
    bool exact_allocation = false;
    for (size_t index = 0U; index < sizeof(allocations) / sizeof(allocations[0]); ++index) {
        if (allocations[index].pointer) {
            exact_allocation = storage.data == allocations[index].pointer &&
                storage.size == allocations[index].size;
        }
    }
    CHECK(exact_allocation && allocation_calls == requests);
    size_t original_requests[9];
    memcpy(original_requests, requested_sizes, sizeof(original_requests));
    uint64_t allocation_work = 0U;
    for (size_t request = 0U; request < requests; ++request) {
        allocation_work += original_requests[request];
    }
    CHECK(original_requests[0] == view.schema.retained_bytes &&
        original_requests[2] == success.index_result.required_retained_bytes &&
        original_requests[4] ==
            serialized_file_managed_values_selected_type(&owner, 0U)->schema.retained_bytes &&
        original_requests[6] ==
            serialized_file_managed_values_selected_type(&owner, 1U)->schema.retained_bytes &&
        original_requests[8] == success.required_retained_bytes);
    /* Observe opaque allocation widths; derive every semantic work charge from
     * original wire counts and the independently checked public contracts.
     * Schemas: 5816+4906+4751 beyond R+T. Contexts: 48+31+28.
     * Profiles: 699+405+370. Index: 7. Two passes each consume 324 bytes through
     * 57 spans and complete 32 values, 3 rows, 6 slots. Queries cost 6+70+69 per
     * pass; first component terminator mismatches cost 3. Four cache tests,
     * 6*(3+1) RID comparisons/completions, 4 layouts and 2 owner units remain. */
    const uint64_t schema_work = 5816U + 4906U + 4751U;
    const uint64_t context_work = 48U + 31U + 28U;
    const uint64_t profile_work = 699U + 405U + 370U;
    const uint64_t payload_work = 2U * (324U + 57U + 32U + 3U + 6U);
    const uint64_t query_work = 2U * (6U + 70U + 69U);
    const uint64_t fixed_work = schema_work + context_work + profile_work + 7U + payload_work +
        query_work + 6U + 4U + 24U + 4U + 2U;
    CHECK(fixed_work == 18235U && success.work_used == allocation_work + fixed_work);
    const uint64_t before_host = original_requests[0] + original_requests[1] +
        original_requests[2] + original_requests[3] + 1U + 5816U + 48U + 699U + 7U;
    /* The Alpha prefix consumes 200 bytes through 39 spans, completes 22 values,
     * 4 slots and 1 null row, then performs two queries and terminator checks.
     * The Beta transition completes Alpha's 32-byte payload and row, reads a
     * 76-byte identity header, and performs one query/terminator/cache test. */
    const uint64_t alpha_prefix_work = 200U + 39U + 22U + 4U + 1U + 6U + 70U + 2U;
    const uint64_t beta_transition_work =
        (32U + 5U + 3U + 1U + 1U) + (76U + 10U + 4U) + 69U + 1U + 1U;
    const uint64_t before_alpha = before_host + alpha_prefix_work + original_requests[4] +
        original_requests[5] + 4906U + 31U + 405U;
    const uint64_t before_beta = before_alpha + beta_transition_work + original_requests[6] +
        original_requests[7] + 4751U + 28U + 370U;
    CHECK(peak_live_bytes <= success.reserved_heap_bytes &&
        success.peak_retained_bytes == success.required_retained_bytes &&
        success.peak_prerequisite_bytes != 0U && success.peak_schema_scratch_bytes != 0U);
    CHECK(success.reserved_heap_bytes == 262144U);
    serialized_file_managed_values_dispose(&owner);
    const uint64_t phase_budgets[] = {before_host, before_alpha, before_beta};
    const uint64_t phase_offsets[] = {4968U, 5168U, 5276U};
    const size_t phase_nodes[] = {2U, 1U, 1U};
    for (size_t phase = 0U; phase < 3U; ++phase) {
        limits = managed_test_limits();
        limits.max_work = phase_budgets[phase];
        reset_allocation_observer();
        const SerializedFileManagedValuesResult result =
            create_values(original->data, &parents, 3U, &limits, &owner);
        CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED &&
            result.limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK &&
            result.field == SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCALAR &&
            result.node_ordinal == phase_nodes[phase] &&
            result.error_offset == phase_offsets[phase] &&
            result.work_used == phase_budgets[phase] && !owner.implementation &&
            live_allocations == 0U);
    }
    limits = managed_test_limits();

    /* Each original prerequisite request and final publication allocation is
     * rejected in turn. Parent owners remain live across the whole sweep. */
    for (size_t request = 1U; request <= requests; ++request) {
        reset_allocation_observer();
        fail_allocation = request;
        const SerializedFileManagedValuesResult result =
            create_values(original->data, &parents, 3U, &limits, &owner);
        fail_allocation = 0U;
        const bool allocation_failure =
            result.status == SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_FAILED ||
            (result.status == SERIALIZED_FILE_MANAGED_VALUES_SCHEMA_REJECTED &&
                result.schema_attempted &&
                result.schema_result.status == SERIALIZED_FILE_SCHEMA_ALLOCATION_FAILED) ||
            (result.status == SERIALIZED_FILE_MANAGED_VALUES_INDEX_REJECTED &&
                result.index_attempted &&
                result.index_result.status == SERIALIZED_FILE_REFERENCE_INDEX_ALLOCATION_FAILED);
        CHECK(allocation_failure);
        CHECK(allocation_calls == request && !owner.implementation && live_allocations == 0U &&
            live_bytes == 0U);
        CHECK(result.work_used <= limits.max_work && peak_live_bytes <= result.reserved_heap_bytes);
    }
    const uint64_t work_boundaries[] = {
        0U, 1U, success.work_used / 2U, success.work_used - 1U, success.work_used};
    for (size_t index = 0U; index < sizeof(work_boundaries) / sizeof(work_boundaries[0]); ++index) {
        limits = managed_test_limits();
        limits.max_work = work_boundaries[index];
        reset_allocation_observer();
        const SerializedFileManagedValuesResult result =
            create_values(original->data, &parents, 3U, &limits, &owner);
        CHECK(result.work_used <= limits.max_work);
        if (limits.max_work == success.work_used) {
            CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK &&
                result.work_used == success.work_used);
        } else {
            CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED &&
                result.limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_WORK && !owner.implementation);
        }
        serialized_file_managed_values_dispose(&owner);
        CHECK(live_allocations == 0U);
    }

    /* Tight success uses original retained counts/bytes; reduce each active
     * physical cap by one without resetting any nested work budget. */
    limits = managed_test_limits();
    limits.max_payload_bytes = view.consumed_bytes;
    limits.max_values = view.value_count;
    limits.max_registry_rows = view.registry_row_count;
    limits.max_rid_slots = view.rid_slot_count;
    limits.max_array_elements = view.array_element_count;
    limits.max_selected_types = view.selected_type_count;
    limits.max_string_bytes = 32U;
    limits.max_total_string_bytes = view.string_bytes;
    limits.max_scalar_bytes = 8U;
    limits.max_total_scalar_bytes = view.scalar_bytes;
    limits.max_padding_bytes = view.padding_bytes;
    limits.max_prerequisite_bytes = success.peak_prerequisite_bytes;
    limits.max_schema_scratch_bytes = success.peak_schema_scratch_bytes;
    limits.max_retained_bytes = success.required_retained_bytes;
    limits.max_heap_bytes = limits.max_prerequisite_bytes +
        (limits.max_retained_bytes > limits.max_schema_scratch_bytes
                ? limits.max_retained_bytes
                : limits.max_schema_scratch_bytes);
    const SerializedFileManagedValuesLimits exact = limits;
    reset_allocation_observer();
    SerializedFileManagedValuesResult result =
        create_values(original->data, &parents, 3U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    serialized_file_managed_values_dispose(&owner);
    const SerializedFileManagedValuesLimit expected_limits[] = {
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PAYLOAD_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_VALUES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_REGISTRY_ROWS,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RID_SLOTS,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_ARRAY_ELEMENTS,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SELECTED_TYPES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_STRING_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_STRING_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCALAR_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_SCALAR_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PADDING_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCHEMA_SCRATCH_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RETAINED_BYTES,
        SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES};
    for (size_t index = 0U; index < sizeof(expected_limits) / sizeof(expected_limits[0]); ++index) {
        limits = exact;
        switch (expected_limits[index]) {
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PAYLOAD_BYTES:
            --limits.max_payload_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_VALUES:
            --limits.max_values;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_REGISTRY_ROWS:
            --limits.max_registry_rows;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RID_SLOTS:
            --limits.max_rid_slots;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_ARRAY_ELEMENTS:
            --limits.max_array_elements;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SELECTED_TYPES:
            --limits.max_selected_types;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_STRING_BYTES:
            --limits.max_string_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_STRING_BYTES:
            --limits.max_total_string_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCALAR_BYTES:
            --limits.max_scalar_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_TOTAL_SCALAR_BYTES:
            --limits.max_total_scalar_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PADDING_BYTES:
            --limits.max_padding_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_PREREQUISITE_BYTES:
            --limits.max_prerequisite_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_SCHEMA_SCRATCH_BYTES:
            --limits.max_schema_scratch_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_RETAINED_BYTES:
            --limits.max_retained_bytes;
            break;
        case SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES:
            --limits.max_heap_bytes;
            break;
        default:
            abort();
        }
        reset_allocation_observer();
        result = create_values(original->data, &parents, 3U, &limits, &owner);
        if (result.status != SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED ||
            result.limit != expected_limits[index]) {
            fprintf(stderr,
                "Resource cap %d returned status %d / limit %d\n",
                expected_limits[index],
                result.status,
                result.limit);
            serialized_file_managed_values_dispose(&owner);
            managed_test_parents_dispose(&parents);
            return false;
        }
        CHECK(!owner.implementation && live_allocations == 0U && live_bytes == 0U);
    }
    limits = managed_test_limits();
    limits.max_prerequisite_bytes = SIZE_MAX;
    limits.max_heap_bytes = SIZE_MAX;
    reset_allocation_observer();
    result = create_values(original->data, &parents, 3U, &limits, &owner);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED &&
        result.limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_HEAP_BYTES && allocation_calls == 0U);
    managed_test_parents_dispose(&parents);
    CHECK(live_allocations == 0U);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s COMMON_STRINGS WITH_TREE\n", argv[0]);
        return EXIT_FAILURE;
    }
    CommonFileBytes file = {0};
    if (!managed_test_read_fixture(argv[1], argv[2], &file)) {
        return EXIT_FAILURE;
    }
    const bool passed = raw_domains_and_exhaustion(&file) && null_index_and_type_policies(&file) &&
        terminator_and_duplicates(&file) && float_bits_and_padding(&file) &&
        packed_unaligned_rids(&file) && unused_schemas_and_authored_names(&file) &&
        repeated_type_cache_and_host_scope(&file) && source_state_and_mapping(&file) &&
        query_component_boundaries(&file) && protected_mapping_end(&file) &&
        resource_and_allocation_boundaries(&file);
    common_file_bytes_dispose(&file);
    if (!passed || live_allocations != 0U) {
        return EXIT_FAILURE;
    }
    puts("Managed values: raw domains, host-local references, provenance, limits "
         "and allocation rollback pass.");
    return EXIT_SUCCESS;
}
