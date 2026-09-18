#include "io/serialized_file_reference_index.h"

#include "common/common.h"

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

enum {
    FIXTURE_BYTES = 4096,
    FIXTURE_ROWS = 6,
    KEY_PARTS = 3
};

typedef struct ReferenceFixture {
    uint8_t bytes[FIXTURE_BYTES];
    size_t rows[FIXTURE_ROWS];
    size_t names[FIXTURE_ROWS][KEY_PARTS];
    size_t lengths[FIXTURE_ROWS][KEY_PARTS];
    size_t ends[FIXTURE_ROWS];
    size_t count;
    size_t end;
    bool tree;
} ReferenceFixture;

typedef struct ReferenceParents {
    SerializedFileDirectory directory;
    SerializedFileMetadataTail tail;
} ReferenceParents;

typedef struct ExpectedCharge {
    uint64_t amount;
    size_t row;
    SerializedFileReferenceIdentityComponent component;
} ExpectedCharge;

static bool expected_charge_failure(const ExpectedCharge* charges,
    size_t count,
    uint64_t limit,
    const ExpectedCharge** out_charge,
    uint64_t* out_used) {
    uint64_t used = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (charges[index].amount > limit - used) {
            *out_charge = &charges[index];
            *out_used = used;
            return true;
        }
        used += charges[index].amount;
    }
    return false;
}

static const SerializedFileReferenceIndexLimits generous_limits = {
    FIXTURE_ROWS, FIXTURE_BYTES, 1048576U, UINT64_C(1048576)};
static const SerializedFileReferenceQueryLimits query_limits = {1024U, 3072U, UINT64_C(1048576)};
static size_t allocation_calls;
static size_t live_allocations;
static bool fail_allocation;
static size_t comparison_calls;
static size_t compared_bytes;

void* reference_index_test_allocate(size_t size) {
    ++allocation_calls;
    if (fail_allocation) {
        return NULL;
    }
    void* allocation = malloc(size);
    if (allocation) {
        ++live_allocations;
    }
    return allocation;
}

void reference_index_test_release(void* allocation) {
    if (allocation) {
        --live_allocations;
        free(allocation);
    }
}

int reference_index_test_compare(const void* left, const void* right, size_t size) {
    ++comparison_calls;
    compared_bytes += size;
    return memcmp(left, right, size);
}

static void store_integer(uint8_t* bytes, size_t width, uint64_t value, bool big_endian) {
    for (size_t index = 0U; index < width; ++index) {
        bytes[big_endian ? width - index - 1U : index] = (uint8_t)value;
        value >>= 8U;
    }
}

static void make_fixture(ReferenceFixture* fixture, size_t count, bool tree, bool big_endian) {
    static const char* const names[FIXTURE_ROWS][KEY_PARTS] = {{"Payload", "Alpha", "Assembly"},
        {"Payload", "Beta", "Assembly"},
        {"Payload", "Alpha", "Assembly"},
        {"Payload", "Alpha", "Assembly"},
        {"", "", ""},
        {"\xff", "", "Other"}};
    memset(fixture, 0, sizeof(*fixture));
    fixture->count = count;
    fixture->tree = tree;
    uint8_t* bytes = fixture->bytes;
    memcpy(bytes + 48U, "2021.3.35f1", 12U);
    bytes[64U] = tree ? 1U : 0U;
    /* Ordinary, object, script and external counts are all zero. */
    size_t offset = 81U;
    store_integer(bytes + offset, 4U, count, big_endian);
    offset += 4U;
    for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
        fixture->rows[ordinal] = offset;
        store_integer(bytes + offset, 4U, UINT32_MAX, big_endian);
        bytes[offset + 4U] = 0xa7U;
        store_integer(bytes + offset + 5U, 2U, 0U, big_endian);
        offset += 7U;
        memset(bytes + offset, 0xab, 32U);
        offset += 32U;
        if (tree) {
            store_integer(bytes + offset, 4U, 1U, big_endian);
            store_integer(bytes + offset + 4U, 4U, 11U, big_endian);
            offset += 8U;
            store_integer(bytes + offset, 2U, 0x1234U, big_endian);
            bytes[offset + 3U] = 0xffU;
            store_integer(bytes + offset + 8U, 4U, 5U, big_endian);
            store_integer(bytes + offset + 12U, 4U, UINT32_MAX, big_endian);
            offset += 32U;
            memcpy(bytes + offset, "Node\0field\0", 11U);
            offset += 11U;
            for (size_t component = 0U; component < KEY_PARTS; ++component) {
                const size_t length = strlen(names[ordinal][component]);
                fixture->names[ordinal][component] = offset;
                fixture->lengths[ordinal][component] = length;
                memcpy(bytes + offset, names[ordinal][component], length + 1U);
                offset += length + 1U;
            }
        }
        fixture->ends[ordinal] = offset;
    }
    bytes[offset++] = 0U;
    fixture->end = offset;
    store_integer(bytes + 8U, 4U, 22U, true);
    store_integer(bytes + 16U, 8U, offset - 48U, true);
    store_integer(bytes + 24U, 8U, offset, true);
    store_integer(bytes + 32U, 8U, offset, true);
    bytes[40U] = big_endian ? 1U : 0U;
}

static bool prepare_parents(const ReferenceFixture* fixture, ReferenceParents* parents) {
    const SerializedFileDirectoryLimits directory_limits = {
        12U, 0U, 0U, 0U, 0U, 0U, FIXTURE_BYTES, 1048576U, 0U, 1048576U};
    const SerializedFileMetadataTailLimits tail_limits = {0U,
        0U,
        FIXTURE_ROWS,
        FIXTURE_ROWS,
        FIXTURE_BYTES,
        FIXTURE_BYTES,
        FIXTURE_BYTES,
        1048576U,
        0U,
        1048576U};
    const SerializedFileDirectoryEngineVersion engine = fixture->bytes[55U] == '2'
        ? SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1
        : SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1;
    const SerializedFileDirectoryResult directory = serialized_file_directory_create(
        fixture->bytes, fixture->end, fixture->end, engine, &directory_limits, &parents->directory);
    CHECK(directory.status == SERIALIZED_FILE_DIRECTORY_OK);
    const SerializedFileMetadataTailResult tail =
        serialized_file_metadata_tail_create(&parents->directory,
            fixture->bytes,
            fixture->end,
            fixture->end,
            &tail_limits,
            &parents->tail);
    CHECK(tail.status == SERIALIZED_FILE_METADATA_TAIL_OK);
    return true;
}

static void dispose_parents(ReferenceParents* parents) {
    serialized_file_metadata_tail_dispose(&parents->tail);
    serialized_file_directory_dispose(&parents->directory);
}

static SerializedFileReferenceKey key_for_row(const ReferenceFixture* fixture, size_t ordinal) {
    const SerializedFileReferenceKey key = {
        {fixture->bytes + fixture->names[ordinal][0], fixture->lengths[ordinal][0]},
        {fixture->bytes + fixture->names[ordinal][1], fixture->lengths[ordinal][1]},
        {fixture->bytes + fixture->names[ordinal][2], fixture->lengths[ordinal][2]}};
    return key;
}

static uint64_t expected_query(const ReferenceFixture* fixture,
    const SerializedFileReferenceKey* key,
    SerializedFileReferenceMatch* out_match,
    size_t* out_comparisons,
    size_t* out_compared_bytes) {
    const SerializedFileReferenceKeyPart parts[KEY_PARTS] = {
        key->class_name, key->namespace_name, key->assembly_name};
    *out_match =
        (SerializedFileReferenceMatch){SERIALIZED_FILE_REFERENCE_MISSING, 0U, SIZE_MAX, SIZE_MAX};
    *out_comparisons = 0U;
    *out_compared_bytes = 0U;
    uint64_t work = 2U;
    for (size_t ordinal = 0U; ordinal < fixture->count; ++ordinal) {
        ++work;
        bool equal = true;
        for (size_t component = 0U; equal && component < KEY_PARTS; ++component) {
            ++work;
            const size_t length = fixture->lengths[ordinal][component];
            equal = length == parts[component].size;
            if (!equal || !length) {
                continue;
            }
            work += length;
            ++*out_comparisons;
            *out_compared_bytes += length;
            for (size_t byte = 0U; byte < length; ++byte) {
                if (fixture->bytes[fixture->names[ordinal][component] + byte] !=
                    parts[component].bytes[byte]) {
                    equal = false;
                }
            }
        }
        if (equal) {
            if (!out_match->match_count) {
                out_match->first_ordinal = ordinal;
                out_match->kind = SERIALIZED_FILE_REFERENCE_UNIQUE;
            } else if (out_match->match_count == 1U) {
                out_match->second_ordinal = ordinal;
                out_match->kind = SERIALIZED_FILE_REFERENCE_AMBIGUOUS;
            }
            ++out_match->match_count;
        }
    }
    return work;
}

static bool check_query(const ReferenceFixture* fixture,
    const SerializedFileReferenceIndex* index,
    const SerializedFileReferenceKey* key) {
    SerializedFileReferenceMatch expected;
    size_t expected_comparisons;
    size_t expected_bytes;
    const uint64_t work =
        expected_query(fixture, key, &expected, &expected_comparisons, &expected_bytes);
    SerializedFileReferenceMatch actual;
    memset(&actual, 0xa5, sizeof(actual));
    comparison_calls = 0U;
    compared_bytes = 0U;
    const SerializedFileReferenceQueryResult result =
        serialized_file_reference_index_query(index, key, &query_limits, &actual);
    CHECK(result.status == SERIALIZED_FILE_REFERENCE_INDEX_OK);
    CHECK(result.limit == SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_NONE);
    CHECK(result.component == SERIALIZED_FILE_REFERENCE_IDENTITY_NONE);
    CHECK(result.row_ordinal == SIZE_MAX && result.work_used == work);
    CHECK(actual.kind == expected.kind && actual.match_count == expected.match_count &&
        actual.first_ordinal == expected.first_ordinal &&
        actual.second_ordinal == expected.second_ordinal);
    CHECK(comparison_calls == expected_comparisons && compared_bytes == expected_bytes);
    return true;
}

static bool check_domains_and_queries(void) {
    for (size_t endian = 0U; endian < 2U; ++endian) {
        ReferenceFixture fixture;
        make_fixture(&fixture, FIXTURE_ROWS, true, endian != 0U);
        if (endian != 0U) {
            /* Equal identity remains ambiguous when a duplicate's tree differs. */
            store_integer(fixture.bytes + fixture.rows[2U] + 47U, 2U, 0x4321U, true);
        }
        ReferenceParents parents = {0};
        SerializedFileReferenceIndex index = {0};
        bool passed = false;
        if (!prepare_parents(&fixture, &parents)) {
            goto cleanup_owners;
        }
        const SerializedFileMetadataTailView* tail =
            serialized_file_metadata_tail_view(&parents.tail);
        const SerializedFileReferenceIndexResult result =
            serialized_file_reference_index_create(&parents.tail, &generous_limits, &index);
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_OK) {
            goto cleanup_owners;
        }
        const SerializedFileReferenceIndexView* view = serialized_file_reference_index_view(&index);
        if (!view || view->reference_type_count != FIXTURE_ROWS ||
            result.reference_type_count != FIXTURE_ROWS ||
            result.work_used != result.required_retained_bytes + 2U * FIXTURE_ROWS + 3U ||
            result.peak_retained_bytes != result.required_retained_bytes ||
            view->retained_bytes != result.required_retained_bytes ||
            view->prefix.header.header_source.data != fixture.bytes ||
            view->tail_source.offset != tail->source.offset) {
            goto cleanup_owners;
        }
        for (size_t ordinal = 0U; ordinal < FIXTURE_ROWS; ++ordinal) {
            const SerializedFileMetadataTailReferenceTypeRow* row =
                serialized_file_reference_index_row(&index, ordinal);
            if (!row || row->ordinal != ordinal || row->source.offset != fixture.rows[ordinal] ||
                row->source.size != fixture.ends[ordinal] - fixture.rows[ordinal] ||
                row->class_name_source.data != fixture.bytes + fixture.names[ordinal][0] ||
                row->namespace_source.data != fixture.bytes + fixture.names[ordinal][1] ||
                row->assembly_name_source.data != fixture.bytes + fixture.names[ordinal][2]) {
                goto cleanup_owners;
            }
        }
        dispose_parents(&parents);
        for (size_t ordinal = 0U; ordinal < FIXTURE_ROWS; ++ordinal) {
            const SerializedFileReferenceKey key = key_for_row(&fixture, ordinal);
            if (!check_query(&fixture, &index, &key)) {
                goto cleanup_owners;
            }
        }
        SerializedFileReferenceKey key = key_for_row(&fixture, 1U);
        key.class_name = (SerializedFileReferenceKeyPart){(const uint8_t*)"payload", 7U};
        if (!check_query(&fixture, &index, &key)) {
            goto cleanup_owners;
        }
        key = key_for_row(&fixture, 1U);
        key.assembly_name = (SerializedFileReferenceKeyPart){(const uint8_t*)"Assemblz", 8U};
        if (!check_query(&fixture, &index, &key)) {
            goto cleanup_owners;
        }
        key.class_name = (SerializedFileReferenceKeyPart){(const uint8_t*)"Pay\0oad", 7U};
        if (!check_query(&fixture, &index, &key)) {
            goto cleanup_owners;
        }
        key = (SerializedFileReferenceKey){{NULL, 0U}, {NULL, 0U}, {NULL, 0U}};
        if (!check_query(&fixture, &index, &key)) {
            goto cleanup_owners;
        }
        passed = !serialized_file_reference_index_row(&index, FIXTURE_ROWS) &&
            !serialized_file_reference_index_row(&index, SIZE_MAX);
    cleanup_owners:
        dispose_parents(&parents);
        serialized_file_reference_index_dispose(&index);
        serialized_file_reference_index_dispose(&index);
        CHECK(passed && live_allocations == 0U);
    }
    return true;
}

static bool check_empty_unavailable_and_engine(void) {
    for (size_t scenario = 0U; scenario < 4U; ++scenario) {
        ReferenceFixture fixture;
        make_fixture(&fixture, scenario == 0U ? 2U : 0U, scenario == 2U, false);
        if (scenario == 3U) {
            memcpy(fixture.bytes + 48U, "2021.3.29f1", 12U);
        }
        ReferenceParents parents = {0};
        SerializedFileReferenceIndex index = {0};
        bool passed = false;
        if (!prepare_parents(&fixture, &parents)) {
            goto cleanup_owners;
        }
        SerializedFileReferenceIndexLimits limits = generous_limits;
        if (!fixture.count) {
            limits.max_reference_types = 0U;
            limits.max_identity_source_bytes = 0U;
        }
        const SerializedFileReferenceIndexResult result =
            serialized_file_reference_index_create(&parents.tail, &limits, &index);
        if (scenario == 0U) {
            passed = result.status == SERIALIZED_FILE_REFERENCE_INDEX_IDENTITIES_UNAVAILABLE &&
                result.reference_type_count == 2U && result.work_used == 1U &&
                !result.required_retained_bytes && !result.peak_retained_bytes &&
                !index.implementation;
        } else if (scenario == 3U) {
            passed = result.status == SERIALIZED_FILE_REFERENCE_INDEX_UNSUPPORTED_ENGINE &&
                !result.reference_type_count && result.work_used == 1U && !index.implementation;
        } else if (result.status == SERIALIZED_FILE_REFERENCE_INDEX_OK) {
            const SerializedFileReferenceKey key = {{NULL, 0U}, {NULL, 0U}, {NULL, 0U}};
            const SerializedFileReferenceQueryLimits empty_limits = {0U, 0U, 2U};
            SerializedFileReferenceMatch match;
            const SerializedFileReferenceQueryResult query =
                serialized_file_reference_index_query(&index, &key, &empty_limits, &match);
            passed = query.status == SERIALIZED_FILE_REFERENCE_INDEX_OK && query.work_used == 2U &&
                match.kind == SERIALIZED_FILE_REFERENCE_MISSING && match.match_count == 0U &&
                match.first_ordinal == SIZE_MAX && match.second_ordinal == SIZE_MAX;
        }
    cleanup_owners:
        dispose_parents(&parents);
        serialized_file_reference_index_dispose(&index);
        CHECK(passed && live_allocations == 0U);
    }
    return true;
}

static bool check_resource_failures(void) {
    ReferenceFixture fixture;
    make_fixture(&fixture, 2U, true, false);
    ReferenceParents parents = {0};
    SerializedFileReferenceIndex index = {0};
    bool passed = false;
    if (!prepare_parents(&fixture, &parents)) {
        goto cleanup_owners;
    }
    const SerializedFileReferenceIndexResult baseline =
        serialized_file_reference_index_create(&parents.tail, &generous_limits, &index);
    if (baseline.status != SERIALIZED_FILE_REFERENCE_INDEX_OK) {
        goto cleanup_owners;
    }
    const uint64_t identity_bytes =
        serialized_file_reference_index_view(&index)->identity_source_bytes;
    serialized_file_reference_index_dispose(&index);
    const ExpectedCharge construction_charges[] = {
        {1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {baseline.required_retained_bytes, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE}};
    for (uint64_t work = 0U; work < baseline.work_used; ++work) {
        SerializedFileReferenceIndexLimits limits = generous_limits;
        limits.max_work = work;
        const SerializedFileReferenceIndexResult result =
            serialized_file_reference_index_create(&parents.tail, &limits, &index);
        const ExpectedCharge* failed_charge;
        uint64_t used;
        if (!expected_charge_failure(construction_charges,
                sizeof(construction_charges) / sizeof(construction_charges[0]),
                work,
                &failed_charge,
                &used)) {
            goto cleanup_owners;
        }
        const uint64_t offset =
            failed_charge->row == SIZE_MAX ? UINT64_MAX : fixture.rows[failed_charge->row];
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED ||
            result.limit != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK ||
            result.work_used != used || result.row_ordinal != failed_charge->row ||
            result.component != failed_charge->component || result.error_offset != offset ||
            result.reference_type_count != (work ? 2U : 0U) ||
            result.required_retained_bytes !=
                (work >= 4U ? baseline.required_retained_bytes : 0U) ||
            result.peak_retained_bytes !=
                (work >= baseline.required_retained_bytes + 4U ? baseline.required_retained_bytes
                                                               : 0U) ||
            index.implementation || live_allocations) {
            goto cleanup_owners;
        }
    }
    for (size_t limit = 0U; limit < 3U; ++limit) {
        SerializedFileReferenceIndexLimits limits = generous_limits;
        SerializedFileReferenceIndexLimit expected;
        if (limit == 0U) {
            limits.max_reference_types = 1U;
            expected = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_REFERENCE_TYPES;
        } else if (limit == 1U) {
            limits.max_identity_source_bytes = identity_bytes - 1U;
            expected = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_IDENTITY_SOURCE_BYTES;
        } else {
            limits.max_retained_bytes = baseline.required_retained_bytes - 1U;
            expected = SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_RETAINED_BYTES;
        }
        const SerializedFileReferenceIndexResult result =
            serialized_file_reference_index_create(&parents.tail, &limits, &index);
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED ||
            result.limit != expected || index.implementation || live_allocations) {
            goto cleanup_owners;
        }
    }
    fail_allocation = true;
    const size_t before_allocation = allocation_calls;
    const SerializedFileReferenceIndexResult failed =
        serialized_file_reference_index_create(&parents.tail, &generous_limits, &index);
    fail_allocation = false;
    if (failed.status != SERIALIZED_FILE_REFERENCE_INDEX_ALLOCATION_FAILED ||
        index.implementation ||
        failed.required_retained_bytes != baseline.required_retained_bytes ||
        failed.peak_retained_bytes || allocation_calls != before_allocation + 1U ||
        live_allocations) {
        goto cleanup_owners;
    }
    SerializedFileReferenceIndexLimits exact = generous_limits;
    exact.max_reference_types = 2U;
    exact.max_identity_source_bytes = identity_bytes;
    exact.max_retained_bytes = baseline.required_retained_bytes;
    exact.max_work = baseline.work_used;
    const SerializedFileReferenceIndexResult exact_result =
        serialized_file_reference_index_create(&parents.tail, &exact, &index);
    if (exact_result.status != SERIALIZED_FILE_REFERENCE_INDEX_OK || live_allocations != 1U) {
        goto cleanup_owners;
    }
    const SerializedFileReferenceKey key = key_for_row(&fixture, 1U);
    SerializedFileReferenceMatch match;
    memset(&match, 0xbc, sizeof(match));
    unsigned char canary[sizeof(match)];
    memcpy(canary, &match, sizeof(canary));
    SerializedFileReferenceMatch expected;
    size_t calls;
    size_t bytes;
    const uint64_t query_work = expected_query(&fixture, &key, &expected, &calls, &bytes);
    /* Alpha's namespace length differs from Beta's: no Alpha namespace bytes
     * or assembly length/bytes are inspected. The second row matches fully. */
    const ExpectedCharge query_charges[] = {{1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS},
        {7U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS},
        {1U, 0U, SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS},
        {7U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_CLASS},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE},
        {4U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_NAMESPACE},
        {1U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY},
        {8U, 1U, SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY},
        {1U, SIZE_MAX, SERIALIZED_FILE_REFERENCE_IDENTITY_NONE}};
    if (query_work != 35U) {
        goto cleanup_owners;
    }
    for (uint64_t work = 0U; work < query_work; ++work) {
        SerializedFileReferenceQueryLimits limits = query_limits;
        limits.max_work = work;
        const SerializedFileReferenceQueryResult result =
            serialized_file_reference_index_query(&index, &key, &limits, &match);
        const ExpectedCharge* failed_charge;
        uint64_t used;
        if (!expected_charge_failure(query_charges,
                sizeof(query_charges) / sizeof(query_charges[0]),
                work,
                &failed_charge,
                &used)) {
            goto cleanup_owners;
        }
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED ||
            result.limit != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_WORK ||
            result.work_used != used || result.row_ordinal != failed_charge->row ||
            result.component != failed_charge->component ||
            memcmp(&match, canary, sizeof(match)) != 0) {
            goto cleanup_owners;
        }
    }
    for (size_t component = 0U; component < 2U; ++component) {
        SerializedFileReferenceQueryLimits limits = query_limits;
        if (!component) {
            limits.max_component_bytes = 7U;
        } else {
            limits.max_key_bytes = 18U;
        }
        const SerializedFileReferenceQueryResult result =
            serialized_file_reference_index_query(&index, &key, &limits, &match);
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_LIMIT_EXCEEDED ||
            result.component != SERIALIZED_FILE_REFERENCE_IDENTITY_ASSEMBLY ||
            result.work_used != 1U || memcmp(&match, canary, sizeof(match)) != 0) {
            goto cleanup_owners;
        }
    }
    passed = check_query(&fixture, &index, &key);
cleanup_owners:
    fail_allocation = false;
    dispose_parents(&parents);
    serialized_file_reference_index_dispose(&index);
    CHECK(passed && live_allocations == 0U);
    return true;
}

static bool check_aliases_and_no_read(void) {
    ReferenceFixture fixture;
    make_fixture(&fixture, 1U, true, false);
    ReferenceParents parents = {0};
    SerializedFileReferenceIndex index = {0};
    bool passed = false;
    if (!prepare_parents(&fixture, &parents)) {
        goto cleanup_owners;
    }
    SerializedFileReferenceIndexLimits mutable_limits = generous_limits;
    SerializedFileReferenceIndex* aliased_outputs[] = {(SerializedFileReferenceIndex*)&parents.tail,
        (SerializedFileReferenceIndex*)&mutable_limits,
        (SerializedFileReferenceIndex*)(void*)fixture.bytes,
        (SerializedFileReferenceIndex*)parents.tail.implementation};
    for (size_t alias = 0U; alias < sizeof(aliased_outputs) / sizeof(aliased_outputs[0]); ++alias) {
        const SerializedFileReferenceIndexResult rejected = serialized_file_reference_index_create(
            &parents.tail, &mutable_limits, aliased_outputs[alias]);
        if (rejected.status != SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT ||
            rejected.work_used) {
            goto cleanup_owners;
        }
    }
    const SerializedFileReferenceIndexResult created =
        serialized_file_reference_index_create(&parents.tail, &generous_limits, &index);
    if (created.status != SERIALIZED_FILE_REFERENCE_INDEX_OK) {
        goto cleanup_owners;
    }
    const void* live_owner = index.implementation;
    const SerializedFileReferenceIndexResult live_rejected =
        serialized_file_reference_index_create(&parents.tail, &generous_limits, &index);
    if (live_rejected.status != SERIALIZED_FILE_REFERENCE_INDEX_INVALID_STATE ||
        live_rejected.work_used || index.implementation != live_owner) {
        goto cleanup_owners;
    }
    SerializedFileReferenceKey key = key_for_row(&fixture, 0U);
    SerializedFileReferenceMatch match;
    memset(&match, 0x91, sizeof(match));
    unsigned char canary[sizeof(match)];
    memcpy(canary, &match, sizeof(canary));
    SerializedFileReferenceMatch* outputs[] = {(SerializedFileReferenceMatch*)&key,
        (SerializedFileReferenceMatch*)&index,
        (SerializedFileReferenceMatch*)(void*)fixture.bytes,
        (SerializedFileReferenceMatch*)index.implementation};
    for (size_t alias = 0U; alias < sizeof(outputs) / sizeof(outputs[0]); ++alias) {
        const SerializedFileReferenceQueryResult result =
            serialized_file_reference_index_query(&index, &key, &query_limits, outputs[alias]);
        if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT || result.work_used) {
            goto cleanup_owners;
        }
    }
    key.class_name = (SerializedFileReferenceKeyPart){(const uint8_t*)&match, 7U};
    SerializedFileReferenceQueryResult result =
        serialized_file_reference_index_query(&index, &key, &query_limits, &match);
    if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT || result.work_used ||
        memcmp(&match, canary, sizeof(match))) {
        goto cleanup_owners;
    }
    key.class_name =
        (SerializedFileReferenceKeyPart){(const uint8_t*)(uintptr_t)(UINTPTR_MAX - 2U), 7U};
    result = serialized_file_reference_index_query(&index, &key, &query_limits, &match);
    if (result.status != SERIALIZED_FILE_REFERENCE_INDEX_INVALID_ARGUMENT || result.work_used) {
        goto cleanup_owners;
    }
#ifndef _WIN32
    const long queried_page = sysconf(_SC_PAGESIZE);
    if (queried_page <= 0) {
        goto cleanup_owners;
    }
    const size_t page = (size_t)queried_page;
    uint8_t* mapping = mmap(NULL, 2U * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        goto cleanup_owners;
    }
    if (mprotect(mapping + page, page, PROT_NONE) != 0) {
        (void)munmap(mapping, 2U * page);
        goto cleanup_owners;
    }
    mapping[page - 1U] = (uint8_t)'P';
    key = key_for_row(&fixture, 0U);
    key.class_name = (SerializedFileReferenceKeyPart){mapping + page - 1U, 1U};
    comparison_calls = 0U;
    result = serialized_file_reference_index_query(&index, &key, &query_limits, &match);
    const bool no_read = result.status == SERIALIZED_FILE_REFERENCE_INDEX_OK &&
        result.work_used == 4U && match.kind == SERIALIZED_FILE_REFERENCE_MISSING &&
        !comparison_calls;
    const bool released = munmap(mapping, 2U * page) == 0;
    if (!no_read || !released) {
        goto cleanup_owners;
    }
#endif
    passed = true;
cleanup_owners:
    dispose_parents(&parents);
    serialized_file_reference_index_dispose(&index);
    CHECK(passed && live_allocations == 0U);
    return true;
}

int main(void) {
    if (!check_domains_and_queries() || !check_empty_unavailable_and_engine() ||
        !check_resource_failures() || !check_aliases_and_no_read()) {
        return EXIT_FAILURE;
    }
    if (live_allocations || atomic_load(&g_allocated_bytes) || atomic_load(&g_allocations_count)) {
        fprintf(stderr, "Reference index allocation remained live\n");
        return EXIT_FAILURE;
    }
    puts("Reference index: original multiplicity, exact tuples, bounds and ownership pass.");
    return EXIT_SUCCESS;
}
