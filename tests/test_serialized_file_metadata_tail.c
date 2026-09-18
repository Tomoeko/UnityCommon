#include "io/serialized_file_metadata_tail.h"

#include "serialized_metadata_tail_fixture.h"

#include "common/common.h"

#include <stdio.h>
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

static const SerializedFileMetadataTailLimits generous_limits = {
    8U, 8U, 8U, 16U, 128U, 256U, 2048U, 65536U, 0U, UINT64_C(1048576)};
static size_t query_count;

typedef struct GuardedTail {
    uint64_t before;
    SerializedFileMetadataTail value;
    uint64_t after;
} GuardedTail;

typedef struct ExpectedFailure {
    SerializedFileMetadataTailStatus status;
    SerializedFileMetadataTailLimit limit;
    SerializedFileMetadataTailField field;
    size_t row;
    uint64_t offset;
    uint64_t work; /* UINT64_MAX leaves work to a separate assertion. */
} ExpectedFailure;

static void guarded_init(GuardedTail* output) {
    memset(output, 0xa5, sizeof(*output));
    serialized_file_metadata_tail_init(&output->value);
}

static bool guards_intact(const GuardedTail* output) {
    CHECK(output->before == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(output->after == UINT64_C(0xa5a5a5a5a5a5a5a5));
    return true;
}

static bool span_equal(SerializedFilePrefixSpan actual, SerializedFilePrefixSpan expected) {
    CHECK(actual.data == expected.data && actual.offset == expected.offset &&
        actual.size == expected.size);
    return true;
}

static bool tree_equal(
    const SerializedFileDirectoryTree* actual, const SerializedFileDirectoryTree* expected) {
    CHECK(actual->node_count == expected->node_count &&
        actual->string_byte_count == expected->string_byte_count);
    CHECK(span_equal(actual->node_count_source, expected->node_count_source));
    CHECK(span_equal(actual->string_count_source, expected->string_count_source));
    CHECK(span_equal(actual->nodes_source, expected->nodes_source));
    CHECK(span_equal(actual->strings_source, expected->strings_source));
    return true;
}

static bool rows_equal(const TailFixture* fixture, const SerializedFileMetadataTail* tail) {
    for (size_t ordinal = 0U; ordinal < fixture->script_count; ++ordinal) {
        const SerializedFileMetadataTailScriptRow* actual =
            serialized_file_metadata_tail_script(tail, ordinal);
        const SerializedFileMetadataTailScriptRow* expected = &fixture->scripts[ordinal];
        CHECK(actual && actual->ordinal == ordinal &&
            actual->file_index_bits == expected->file_index_bits &&
            actual->local_identifier_bits == expected->local_identifier_bits);
        CHECK(span_equal(actual->source, expected->source));
        CHECK(span_equal(actual->file_index_source, expected->file_index_source));
        CHECK(span_equal(actual->alignment_source, expected->alignment_source));
        CHECK(span_equal(actual->local_identifier_source, expected->local_identifier_source));
    }
    for (size_t ordinal = 0U; ordinal < fixture->external_count; ++ordinal) {
        const SerializedFileMetadataTailExternalRow* actual =
            serialized_file_metadata_tail_external(tail, ordinal);
        const SerializedFileMetadataTailExternalRow* expected = &fixture->externals[ordinal];
        CHECK(actual && actual->ordinal == ordinal && actual->type_bits == expected->type_bits);
        CHECK(span_equal(actual->source, expected->source));
        CHECK(span_equal(actual->leading_string_source, expected->leading_string_source));
        CHECK(span_equal(actual->guid_source, expected->guid_source));
        CHECK(span_equal(actual->type_source, expected->type_source));
        CHECK(span_equal(actual->path_source, expected->path_source));
        for (size_t word = 0U; word < 4U; ++word) {
            CHECK(actual->guid_words[word] == expected->guid_words[word]);
        }
    }
    for (size_t ordinal = 0U; ordinal < fixture->reference_count; ++ordinal) {
        const SerializedFileMetadataTailReferenceTypeRow* actual =
            serialized_file_metadata_tail_reference_type(tail, ordinal);
        const SerializedFileMetadataTailReferenceTypeRow* expected = &fixture->references[ordinal];
        CHECK(actual && actual->ordinal == ordinal &&
            actual->class_id_bits == expected->class_id_bits &&
            actual->stripped_raw == expected->stripped_raw &&
            actual->script_index_bits == expected->script_index_bits &&
            actual->has_script_hash == expected->has_script_hash &&
            actual->has_tree == expected->has_tree);
        CHECK(span_equal(actual->source, expected->source));
        CHECK(span_equal(actual->class_id_source, expected->class_id_source));
        CHECK(span_equal(actual->stripped_source, expected->stripped_source));
        CHECK(span_equal(actual->script_index_source, expected->script_index_source));
        CHECK(span_equal(actual->script_hash_source, expected->script_hash_source));
        CHECK(span_equal(actual->type_hash_source, expected->type_hash_source));
        CHECK(tree_equal(&actual->tree, &expected->tree));
        CHECK(span_equal(actual->class_name_source, expected->class_name_source));
        CHECK(span_equal(actual->namespace_source, expected->namespace_source));
        CHECK(span_equal(actual->assembly_name_source, expected->assembly_name_source));
    }
    CHECK(!serialized_file_metadata_tail_script(tail, fixture->script_count));
    CHECK(!serialized_file_metadata_tail_external(tail, fixture->external_count));
    CHECK(!serialized_file_metadata_tail_reference_type(tail, fixture->reference_count));
    CHECK(!serialized_file_metadata_tail_script(tail, SIZE_MAX));
    CHECK(!serialized_file_metadata_tail_external(tail, SIZE_MAX));
    CHECK(!serialized_file_metadata_tail_reference_type(tail, SIZE_MAX));
    return true;
}

static bool accepted_fixture(TailFixture* fixture,
    const SerializedFileMetadataTailLimits* limits,
    size_t* out_retained,
    uint64_t* out_work) {
    SerializedFileDirectory parent;
    CHECK(tail_fixture_parent(fixture, &parent));
    const size_t parent_retained = serialized_file_directory_view(&parent)->retained_bytes;
    const size_t allocations = g_allocations_count;
    GuardedTail output;
    guarded_init(&output);
    ++query_count;
    const SerializedFileMetadataTailResult result = serialized_file_metadata_tail_create(
        &parent, fixture->bytes, fixture->end, fixture->logical_size, limits, &output.value);
    CHECK(result.status == SERIALIZED_FILE_METADATA_TAIL_OK &&
        result.limit == SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE &&
        result.field == SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE &&
        result.row_ordinal == SIZE_MAX && result.error_offset == UINT64_MAX &&
        !result.peak_scratch_bytes);
    const SerializedFileMetadataTailView* view = serialized_file_metadata_tail_view(&output.value);
    CHECK(view && result.required_retained_bytes == view->retained_bytes &&
        result.peak_retained_bytes == view->retained_bytes &&
        g_allocations_count == allocations + 1U);
    CHECK(view->script_count == fixture->script_count &&
        view->external_count == fixture->external_count &&
        view->reference_type_count == fixture->reference_count &&
        view->node_record_count == fixture->nodes &&
        view->tree_string_byte_count == fixture->tree_strings &&
        view->terminated_string_byte_count == fixture->terminated_strings);
    const SerializedFilePrefixSpan source = {
        fixture->bytes + TAIL_FIXTURE_START, TAIL_FIXTURE_START, fixture->end - TAIL_FIXTURE_START};
    CHECK(span_equal(view->source, source));
    CHECK(span_equal(view->script_count_source, fixture->counts[0]));
    CHECK(span_equal(view->external_count_source, fixture->counts[1]));
    CHECK(span_equal(view->reference_type_count_source, fixture->counts[2]));
    CHECK(span_equal(view->script_rows_source, fixture->tables[0]));
    CHECK(span_equal(view->external_rows_source, fixture->tables[1]));
    CHECK(span_equal(view->reference_type_rows_source, fixture->tables[2]));
    CHECK(span_equal(view->user_information_source, fixture->information));
    CHECK(view->directory.engine_version == fixture->version && !view->directory.type_count &&
        !view->directory.object_count &&
        view->directory.remaining_metadata.offset == TAIL_FIXTURE_START &&
        view->directory.remaining_metadata.size == source.size &&
        view->directory.retained_bytes == parent_retained);
    CHECK(view->directory.prefix.header.header_source.data == fixture->bytes &&
        view->directory.prefix.header.file_size == fixture->logical_size &&
        view->directory.prefix.header.endian_selector == (fixture->big_endian ? 1U : 0U));
    const uint64_t pass_work =
        source.size + fixture->script_count + fixture->external_count + fixture->reference_count;
    CHECK(result.work_used == 2U * pass_work + result.required_retained_bytes + 3U);
    CHECK(guards_intact(&output));

    /* No retained descriptor may point into the parent's allocation. The byte
     * backing stays live while all row spans are inspected after parent disposal. */
    serialized_file_directory_dispose(&parent);
    CHECK(!serialized_file_directory_view(&parent) && rows_equal(fixture, &output.value));
    CHECK(view->directory.retained_bytes == parent_retained);
    if (out_retained) {
        *out_retained = result.required_retained_bytes;
    }
    if (out_work) {
        *out_work = result.work_used;
    }
    serialized_file_metadata_tail_dispose(&output.value);
    serialized_file_metadata_tail_dispose(&output.value);
    CHECK(!serialized_file_metadata_tail_view(&output.value) && guards_intact(&output));
    CHECK(!g_allocations_count && !g_allocated_bytes);
    return true;
}

static bool failure_matches(SerializedFileMetadataTailResult actual, ExpectedFailure expected) {
    if (actual.status != expected.status || actual.limit != expected.limit ||
        actual.field != expected.field || actual.row_ordinal != expected.row ||
        actual.error_offset != expected.offset ||
        (expected.work != UINT64_MAX && actual.work_used != expected.work)) {
        fprintf(stderr,
            "Tail failure actual=%d/%d/%d row=%zu offset=%llu work=%llu; "
            "expected=%d/%d/%d row=%zu offset=%llu work=%llu\n",
            (int)actual.status,
            (int)actual.limit,
            (int)actual.field,
            actual.row_ordinal,
            (unsigned long long)actual.error_offset,
            (unsigned long long)actual.work_used,
            (int)expected.status,
            (int)expected.limit,
            (int)expected.field,
            expected.row,
            (unsigned long long)expected.offset,
            (unsigned long long)expected.work);
        return false;
    }
    CHECK(!actual.peak_scratch_bytes);
    return true;
}

static bool rejected_fixture(const TailFixture* fixture,
    const SerializedFileDirectory* parent,
    size_t mapping,
    const SerializedFileMetadataTailLimits* limits,
    ExpectedFailure expected,
    SerializedFileMetadataTailResult* out_result) {
    uint8_t input_before[TAIL_FIXTURE_CAPACITY];
    memcpy(input_before, fixture->bytes, sizeof(input_before));
    uint8_t parent_before[sizeof(SerializedFileDirectoryView)];
    const SerializedFileDirectoryView* parent_view = serialized_file_directory_view(parent);
    memcpy(parent_before, parent_view, sizeof(parent_before));
    uint8_t limits_before[sizeof(*limits)];
    memcpy(limits_before, limits, sizeof(limits_before));
    const size_t allocations = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    GuardedTail output;
    guarded_init(&output);
    ++query_count;
    const SerializedFileMetadataTailResult result = serialized_file_metadata_tail_create(
        parent, fixture->bytes, mapping, fixture->logical_size, limits, &output.value);
    CHECK(failure_matches(result, expected));
    CHECK(!output.value.implementation && !serialized_file_metadata_tail_view(&output.value));
    CHECK(!serialized_file_metadata_tail_script(&output.value, 0U));
    CHECK(!serialized_file_metadata_tail_external(&output.value, 0U));
    CHECK(!serialized_file_metadata_tail_reference_type(&output.value, 0U));
    CHECK(guards_intact(&output));
    CHECK(memcmp(input_before, fixture->bytes, sizeof(input_before)) == 0);
    CHECK(parent_view == serialized_file_directory_view(parent) &&
        memcmp(parent_before, parent_view, sizeof(parent_before)) == 0);
    CHECK(memcmp(limits_before, limits, sizeof(limits_before)) == 0);
    CHECK(g_allocations_count == allocations && g_allocated_bytes == allocated_bytes);
    serialized_file_metadata_tail_dispose(&output.value);
    if (out_result) {
        *out_result = result;
    }
    return true;
}

static const TailFixtureEvent* unavailable_event(
    const TailFixture* fixture, uint64_t end, uint64_t* out_work) {
    uint64_t work = 1U;
    for (size_t index = 0U; index < fixture->event_count; ++index) {
        const TailFixtureEvent* event = &fixture->events[index];
        if (event->size && (event->offset > end || event->size > end - event->offset)) {
            *out_work = work;
            return event;
        }
        work += event->work;
    }
    return NULL;
}

static bool wire_versions_and_empty_tables(void) {
    for (unsigned version = 0U; version < 2U; ++version) {
        for (unsigned order = 0U; order < 2U; ++order) {
            for (unsigned tree = 0U; tree < 2U; ++tree) {
                for (unsigned empty = 0U; empty < 2U; ++empty) {
                    TailFixture fixture;
                    tail_fixture_build(&fixture,
                        order != 0U,
                        tree != 0U,
                        version ? SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1
                                : SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                        empty != 0U);
                    SerializedFileMetadataTailLimits limits = generous_limits;
                    if (empty) {
                        limits = (SerializedFileMetadataTailLimits){
                            0U, 0U, 0U, 0U, 0U, 1U, 13U, 65536U, 0U, 1048576U};
                        CHECK(fixture.end == 86U && fixture.tables[0].offset == 77U &&
                            fixture.counts[1].offset == 77U && fixture.counts[2].offset == 81U);
                    } else {
                        CHECK(fixture.scripts[0].file_index_source.offset == 77U &&
                            fixture.scripts[0].alignment_source.offset == 81U &&
                            fixture.scripts[0].alignment_source.size == 3U &&
                            fixture.scripts[0].local_identifier_source.offset == 84U &&
                            fixture.scripts[1].source.offset == 92U &&
                            !fixture.scripts[1].alignment_source.size);
                    }
                    CHECK(accepted_fixture(&fixture, &limits, NULL, NULL));
                }
            }
        }
    }
    return true;
}

static bool every_mapping_and_metadata_end(void) {
    for (unsigned order = 0U; order < 2U; ++order) {
        for (unsigned tree = 0U; tree < 2U; ++tree) {
            TailFixture fixture;
            tail_fixture_build(&fixture,
                order != 0U,
                tree != 0U,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                false);
            SerializedFileDirectory parent;
            CHECK(tail_fixture_parent(&fixture, &parent));
            for (size_t end = 0U; end < fixture.end; ++end) {
                uint64_t work = 0U;
                const TailFixtureEvent* event = unavailable_event(&fixture, end, &work);
                CHECK(event);
                const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING,
                    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                    event->field,
                    event->row,
                    end,
                    work};
                CHECK(rejected_fixture(&fixture, &parent, end, &generous_limits, expected, NULL));
            }
            serialized_file_directory_dispose(&parent);
            for (size_t end = TAIL_FIXTURE_START; end < fixture.end; ++end) {
                tail_fixture_header(&fixture, end, 4096U);
                CHECK(tail_fixture_parent(&fixture, &parent));
                uint64_t work = 0U;
                const TailFixtureEvent* event = unavailable_event(&fixture, end, &work);
                CHECK(event);
                const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
                    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                    event->field,
                    event->row,
                    end,
                    work};
                CHECK(rejected_fixture(
                    &fixture, &parent, fixture.end, &generous_limits, expected, NULL));
                /* Declared end wins even if mapping and all budgets also end. */
                SerializedFileMetadataTailLimits limits = generous_limits;
                limits.max_tail_bytes = 0U;
                limits.max_terminated_string_bytes = 0U;
                limits.max_work = 1U;
                if (end == TAIL_FIXTURE_START) {
                    CHECK(rejected_fixture(&fixture, &parent, 0U, &limits, expected, NULL));
                }
                serialized_file_directory_dispose(&parent);
            }
        }
    }
    return true;
}

static uint64_t work_before_event(const TailFixture* fixture, size_t selected) {
    uint64_t work = 1U;
    for (size_t index = 0U; index < selected; ++index) {
        work += fixture->events[index].work;
    }
    return work;
}

static size_t event_at(const TailFixture* fixture, uint64_t offset) {
    for (size_t index = 0U; index < fixture->event_count; ++index) {
        if (fixture->events[index].size && fixture->events[index].offset == offset) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool count_domains_and_caps(void) {
    static const SerializedFileMetadataTailField fields[] = {
        SERIALIZED_FILE_METADATA_TAIL_FIELD_SCRIPT_COUNT,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_COUNT,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_COUNT};
    static const SerializedFileMetadataTailLimit kinds[] = {
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_SCRIPTS,
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXTERNALS,
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_REFERENCE_TYPES};
    static const uint32_t invalid_counts[] = {UINT32_C(0x80000000), UINT32_MAX};
    for (unsigned order = 0U; order < 2U; ++order) {
        for (size_t table = 0U; table < 3U; ++table) {
            for (size_t variant = 0U; variant < 3U; ++variant) {
                TailFixture fixture;
                tail_fixture_build(&fixture,
                    order != 0U,
                    false,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    false);
                const size_t offset = (size_t)fixture.counts[table].offset;
                tail_fixture_store(fixture.bytes + offset,
                    4U,
                    variant < 2U ? invalid_counts[variant] : INT32_MAX,
                    fixture.big_endian);
                SerializedFileDirectory parent;
                CHECK(tail_fixture_parent(&fixture, &parent));
                SerializedFileMetadataTailLimits limits = generous_limits;
                if (table == 0U) {
                    limits.max_scripts = 0U;
                } else if (table == 1U) {
                    limits.max_externals = 0U;
                } else {
                    limits.max_reference_types = 0U;
                }
                const size_t selected = event_at(&fixture, offset);
                CHECK(selected != SIZE_MAX);
                const ExpectedFailure expected = {variant < 2U
                        ? SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA
                        : SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
                    variant < 2U ? SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE : kinds[table],
                    fields[table],
                    SIZE_MAX,
                    offset,
                    work_before_event(&fixture, selected) + 4U};
                CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
                serialized_file_directory_dispose(&parent);
            }
        }
    }
    return true;
}

static bool aggregate_byte_and_tree_limits(void) {
    TailFixture fixture;
    tail_fixture_build(&fixture, true, true, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
    SerializedFileDirectory parent;
    CHECK(tail_fixture_parent(&fixture, &parent));
    for (uint64_t maximum = 0U; maximum < fixture.end - TAIL_FIXTURE_START; ++maximum) {
        SerializedFileMetadataTailLimits limits = generous_limits;
        limits.max_tail_bytes = maximum;
        uint64_t work = 0U;
        const TailFixtureEvent* event =
            unavailable_event(&fixture, TAIL_FIXTURE_START + maximum, &work);
        CHECK(event);
        const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_TAIL_BYTES,
            event->field,
            event->row,
            event->offset,
            work};
        CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
    }
    uint64_t terminated = 0U;
    for (size_t index = 0U; index < fixture.event_count; ++index) {
        const TailFixtureEvent* event = &fixture.events[index];
        if (!event->terminated_byte) {
            continue;
        }
        SerializedFileMetadataTailLimits limits = generous_limits;
        limits.max_terminated_string_bytes = terminated++;
        const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_TERMINATED_STRING_BYTES,
            event->field,
            event->row,
            event->offset,
            work_before_event(&fixture, index)};
        CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
    }
    CHECK(terminated == fixture.terminated_strings);
    for (unsigned strings = 0U; strings < 2U; ++strings) {
        const uint64_t total = strings ? fixture.tree_strings : fixture.nodes;
        for (uint64_t maximum = 0U; maximum < total; ++maximum) {
            uint64_t accumulated = 0U;
            size_t row = 0U;
            for (; row < fixture.reference_count; ++row) {
                accumulated += strings ? fixture.references[row].tree.string_byte_count
                                       : fixture.references[row].tree.node_count;
                if (accumulated > maximum) {
                    break;
                }
            }
            CHECK(row < fixture.reference_count);
            const uint64_t offset = strings
                ? fixture.references[row].tree.string_count_source.offset
                : fixture.references[row].tree.node_count_source.offset;
            const size_t selected = event_at(&fixture, offset);
            CHECK(selected != SIZE_MAX);
            SerializedFileMetadataTailLimits limits = generous_limits;
            if (strings) {
                limits.max_tree_string_bytes = maximum;
            } else {
                limits.max_node_records = maximum;
            }
            const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
                strings ? SERIALIZED_FILE_METADATA_TAIL_LIMIT_TREE_STRING_BYTES
                        : SERIALIZED_FILE_METADATA_TAIL_LIMIT_NODES,
                SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
                row,
                offset,
                work_before_event(&fixture, selected) + 4U};
            CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
        }
    }
    serialized_file_directory_dispose(&parent);
    SerializedFileMetadataTailLimits exact = generous_limits;
    exact.max_scripts = fixture.script_count;
    exact.max_externals = fixture.external_count;
    exact.max_reference_types = fixture.reference_count;
    exact.max_node_records = fixture.nodes;
    exact.max_tree_string_bytes = fixture.tree_strings;
    exact.max_terminated_string_bytes = fixture.terminated_strings;
    exact.max_tail_bytes = fixture.end - TAIL_FIXTURE_START;
    CHECK(accepted_fixture(&fixture, &exact, NULL, NULL));
    return true;
}

/* This is the public charge grammar applied to the fixture's construction
 * schedule, not a replay of product decoding or storage-layout internals. */
static ExpectedFailure expected_work_cutoff(const TailFixture* fixture,
    size_t retained,
    uint64_t maximum,
    size_t* out_required,
    size_t* out_peak) {
    ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        0U};
    *out_required = 0U;
    *out_peak = 0U;
    if (!maximum) {
        return expected;
    }
    uint64_t work = 1U;
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        for (size_t index = 0U; index < fixture->event_count; ++index) {
            const TailFixtureEvent* event = &fixture->events[index];
            if (event->work > maximum - work) {
                expected.field = event->field;
                expected.row = event->row;
                expected.offset = event->offset;
                expected.work = work;
                return expected;
            }
            work += event->work;
        }
        if (maximum == work) {
            expected.work = work;
            return expected;
        }
        ++work; /* Planning after preflight, publication after replay. */
        if (!pass) {
            *out_required = retained;
            if (retained > maximum - work) {
                expected.work = work;
                return expected;
            }
            work += retained;
            *out_peak = retained;
        }
    }
    /* Callers request a strict cutoff below the complete successful charge. */
    expected.work = UINT64_MAX;
    return expected;
}

static bool work_and_storage_limits(void) {
    for (unsigned tree = 0U; tree < 2U; ++tree) {
        TailFixture fixture;
        tail_fixture_build(
            &fixture, false, tree != 0U, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
        size_t retained = 0U;
        uint64_t complete_work = 0U;
        CHECK(accepted_fixture(&fixture, &generous_limits, &retained, &complete_work));
        SerializedFileDirectory parent;
        CHECK(tail_fixture_parent(&fixture, &parent));
        for (uint64_t maximum = 0U; maximum < complete_work; ++maximum) {
            SerializedFileMetadataTailLimits limits = generous_limits;
            limits.max_work = maximum;
            size_t required = 0U;
            size_t peak = 0U;
            const ExpectedFailure expected =
                expected_work_cutoff(&fixture, retained, maximum, &required, &peak);
            CHECK(expected.work != UINT64_MAX);
            SerializedFileMetadataTailResult result;
            CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, &result));
            CHECK(result.required_retained_bytes == required && result.peak_retained_bytes == peak);
        }
        const uint64_t pass_work = fixture.end - TAIL_FIXTURE_START + fixture.script_count +
            fixture.external_count + fixture.reference_count;
        for (unsigned zero = 0U; zero < 2U; ++zero) {
            SerializedFileMetadataTailLimits limits = generous_limits;
            limits.max_retained_bytes = zero ? 0U : retained - 1U;
            const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
                SERIALIZED_FILE_METADATA_TAIL_LIMIT_RETAINED_BYTES,
                SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
                SIZE_MAX,
                UINT64_MAX,
                pass_work + 2U};
            SerializedFileMetadataTailResult result;
            CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, &result));
            CHECK(result.required_retained_bytes == retained && !result.peak_retained_bytes);
        }
        serialized_file_directory_dispose(&parent);
        SerializedFileMetadataTailLimits exact = generous_limits;
        exact.max_work = complete_work;
        exact.max_retained_bytes = retained;
        CHECK(accepted_fixture(&fixture, &exact, NULL, NULL));
        exact.max_scratch_bytes = 1U;
        CHECK(accepted_fixture(&fixture, &exact, NULL, NULL));
    }
    return true;
}

static bool unsupported_shapes_and_unsigned_trees(void) {
    for (unsigned order = 0U; order < 2U; ++order) {
        for (unsigned tree = 0U; tree < 2U; ++tree) {
            TailFixture fixture;
            tail_fixture_build(&fixture,
                order != 0U,
                tree != 0U,
                SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                false);
            const size_t start = (size_t)fixture.references[0].source.offset;
            tail_fixture_store(fixture.bytes + start, 4U, UINT32_MAX, fixture.big_endian);
            SerializedFileDirectory parent;
            CHECK(tail_fixture_parent(&fixture, &parent));
            const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TYPE_SHAPE,
                SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                SERIALIZED_FILE_METADATA_TAIL_FIELD_REFERENCE_TYPE_ENTRY,
                0U,
                start,
                work_before_event(&fixture, event_at(&fixture, start)) + 7U};
            CHECK(
                rejected_fixture(&fixture, &parent, fixture.end, &generous_limits, expected, NULL));
            serialized_file_directory_dispose(&parent);
        }
        for (unsigned strings = 0U; strings < 2U; ++strings) {
            for (unsigned high = 0U; high < 2U; ++high) {
                TailFixture fixture;
                tail_fixture_build(&fixture,
                    order != 0U,
                    true,
                    SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1,
                    false);
                const uint64_t offset = strings
                    ? fixture.references[0].tree.string_count_source.offset
                    : fixture.references[0].tree.node_count_source.offset;
                const uint32_t value = high ? UINT32_MAX : UINT32_C(0x80000000);
                tail_fixture_store(fixture.bytes + offset, 4U, value, fixture.big_endian);
                SerializedFileDirectory parent;
                CHECK(tail_fixture_parent(&fixture, &parent));
                const ExpectedFailure cap = {SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
                    strings ? SERIALIZED_FILE_METADATA_TAIL_LIMIT_TREE_STRING_BYTES
                            : SERIALIZED_FILE_METADATA_TAIL_LIMIT_NODES,
                    SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
                    0U,
                    offset,
                    work_before_event(&fixture, event_at(&fixture, offset)) + 4U};
                CHECK(
                    rejected_fixture(&fixture, &parent, fixture.end, &generous_limits, cap, NULL));
                SerializedFileMetadataTailLimits limits = generous_limits;
                limits.max_node_records = UINT64_MAX;
                limits.max_tree_string_bytes = UINT64_MAX;
                const ExpectedFailure extent = {SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
                    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                    SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
                    0U,
                    fixture.end,
                    UINT64_MAX};
                CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, extent, NULL));
                serialized_file_directory_dispose(&parent);

                /* Full-u32 counts reach physical extent admission when caps
                 * allow them. The high logical end never causes a giant map. */
                const uint64_t metadata_end = UINT64_C(0x20000000000);
                tail_fixture_header(&fixture, metadata_end, metadata_end + 1U);
                CHECK(tail_fixture_parent(&fixture, &parent));
                const ExpectedFailure mapping = {SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING,
                    SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                    SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
                    0U,
                    fixture.end,
                    UINT64_MAX};
                CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, mapping, NULL));
                serialized_file_directory_dispose(&parent);
            }
        }
        TailFixture fixture;
        tail_fixture_build(
            &fixture, order != 0U, true, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
        const uint64_t offset = fixture.references[0].tree.node_count_source.offset;
        tail_fixture_store(fixture.bytes + offset, 4U, 0U, fixture.big_endian);
        SerializedFileDirectory parent;
        CHECK(tail_fixture_parent(&fixture, &parent));
        SerializedFileMetadataTailLimits limits = generous_limits;
        limits.max_node_records = 0U;
        const ExpectedFailure zero = {SERIALIZED_FILE_METADATA_TAIL_UNSUPPORTED_TREE_SHAPE,
            SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
            SERIALIZED_FILE_METADATA_TAIL_FIELD_TREE,
            0U,
            offset,
            work_before_event(&fixture, event_at(&fixture, offset)) + 4U};
        CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, zero, NULL));
        serialized_file_directory_dispose(&parent);
    }
    return true;
}

static bool optional_hashes_duplicates_and_exhaustion(void) {
    for (unsigned order = 0U; order < 2U; ++order) {
        TailFixture fixture;
        tail_fixture_build(
            &fixture, order != 0U, false, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_29F1, false);
        /* Preserve the existing widths while exercising the other sign-bit
         * representations and a native class with a nonnegative index. */
        fixture.references[0].script_index_bits = UINT16_MAX;
        tail_fixture_store(fixture.bytes + fixture.references[0].script_index_source.offset,
            2U,
            UINT16_MAX,
            fixture.big_endian);
        fixture.references[1].class_id_bits = 49U;
        fixture.references[1].script_index_bits = 0U;
        tail_fixture_store(fixture.bytes + fixture.references[1].class_id_source.offset,
            4U,
            49U,
            fixture.big_endian);
        tail_fixture_store(fixture.bytes + fixture.references[1].script_index_source.offset,
            2U,
            0U,
            fixture.big_endian);
        fixture.references[2].class_id_bits = 49U;
        fixture.references[2].script_index_bits = 0U;
        fixture.references[2].stripped_raw = fixture.references[1].stripped_raw;
        memcpy(fixture.bytes + fixture.references[2].source.offset,
            fixture.bytes + fixture.references[1].source.offset,
            fixture.references[1].source.size);
        CHECK(accepted_fixture(&fixture, &generous_limits, NULL, NULL));

        for (unsigned variant = 0U; variant < 3U; ++variant) {
            tail_fixture_build(
                &fixture, order != 0U, false, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
            ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_MALFORMED_METADATA,
                SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                SERIALIZED_FILE_METADATA_TAIL_FIELD_EXHAUSTION,
                SIZE_MAX,
                fixture.end,
                UINT64_MAX};
            if (variant == 0U) {
                fixture.bytes[fixture.end - 1U] = 0xffU;
                expected.field = SERIALIZED_FILE_METADATA_TAIL_FIELD_USER_INFORMATION;
            } else if (variant == 1U) {
                tail_fixture_header(&fixture, fixture.end + 1U, 4096U);
            } else {
                fixture.bytes[fixture.information.offset] = 0U;
                expected.offset = fixture.information.offset + 1U;
            }
            SerializedFileDirectory parent;
            CHECK(tail_fixture_parent(&fixture, &parent));
            CHECK(
                rejected_fixture(&fixture, &parent, fixture.end, &generous_limits, expected, NULL));
            serialized_file_directory_dispose(&parent);
        }
    }
    return true;
}

static bool first_failure_precedence(void) {
    TailFixture fixture;
    tail_fixture_build(&fixture, false, true, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
    SerializedFileDirectory parent;
    CHECK(tail_fixture_parent(&fixture, &parent));
    const uint64_t offset = fixture.externals[0].leading_string_source.offset;
    const size_t selected = event_at(&fixture, offset);
    CHECK(selected != SIZE_MAX);
    const uint64_t work = work_before_event(&fixture, selected);
    SerializedFileMetadataTailLimits limits = generous_limits;
    limits.max_tail_bytes = offset - TAIL_FIXTURE_START;
    limits.max_terminated_string_bytes = 0U;
    limits.max_work = work;
    ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING,
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_EXTERNAL_LEADING_STRING,
        0U,
        offset,
        work};
    CHECK(rejected_fixture(&fixture, &parent, (size_t)offset, &limits, expected, NULL));
    expected.status = SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED;
    expected.limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_TAIL_BYTES;
    CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
    ++limits.max_tail_bytes;
    expected.limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_TERMINATED_STRING_BYTES;
    CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
    ++limits.max_terminated_string_bytes;
    expected.limit = SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK;
    CHECK(rejected_fixture(&fixture, &parent, fixture.end, &limits, expected, NULL));
    serialized_file_directory_dispose(&parent);
    return true;
}

static bool argument_failure(const SerializedFileDirectory* parent,
    const uint8_t* bytes,
    size_t mapping,
    uint64_t logical_size,
    const SerializedFileMetadataTailLimits* limits,
    SerializedFileMetadataTail* output,
    SerializedFileMetadataTailStatus status,
    uint64_t work) {
    ++query_count;
    const SerializedFileMetadataTailResult result =
        serialized_file_metadata_tail_create(parent, bytes, mapping, logical_size, limits, output);
    const ExpectedFailure expected = {status,
        status == SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED
            ? SERIALIZED_FILE_METADATA_TAIL_LIMIT_WORK
            : SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
        SERIALIZED_FILE_METADATA_TAIL_FIELD_NONE,
        SIZE_MAX,
        UINT64_MAX,
        work};
    CHECK(failure_matches(result, expected));
    CHECK(!result.required_retained_bytes && !result.peak_retained_bytes);
    return true;
}

static bool binding_arguments_and_aliases(void) {
    TailFixture fixture;
    tail_fixture_build(&fixture, false, true, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
    SerializedFileDirectory parent;
    CHECK(tail_fixture_parent(&fixture, &parent));
    SerializedFileDirectory empty_parent;
    serialized_file_directory_init(&empty_parent);
    SerializedFileMetadataTailLimits limits = generous_limits;
    GuardedTail output;
    guarded_init(&output);
    uint8_t input_before[TAIL_FIXTURE_CAPACITY];
    memcpy(input_before, fixture.bytes, sizeof(input_before));
    const SerializedFileDirectoryView* parent_view = serialized_file_directory_view(&parent);
    uint8_t owner_before[sizeof(*parent_view)];
    memcpy(owner_before, parent_view, sizeof(owner_before));
    const void* parent_implementation = parent.implementation;
    const size_t allocations = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;

    CHECK(argument_failure(NULL,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&empty_parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        NULL,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        NULL,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        NULL,
        1U,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        NULL,
        0U,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_SOURCE_MISMATCH,
        1U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.end - 1U,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        input_before,
        fixture.end,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_SOURCE_MISMATCH,
        1U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size + 1U,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_SOURCE_MISMATCH,
        1U));
    limits.max_work = 0U;
    CHECK(argument_failure(&parent,
        input_before,
        fixture.end,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_LIMIT_EXCEEDED,
        0U));
    limits = generous_limits;

    /* Deliberately overlapping pointers are admitted only as invalid arguments;
     * none are dereferenced as a forged live owner or modified by the test. */
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        (SerializedFileMetadataTail*)(void*)fixture.bytes,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        (const SerializedFileMetadataTailLimits*)(const void*)fixture.bytes,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        (SerializedFileMetadataTail*)(void*)&limits,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        (SerializedFileMetadataTail*)(void*)&parent,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        (const SerializedFileMetadataTailLimits*)(const void*)&parent,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        (const uint8_t*)&parent,
        sizeof(parent),
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        (const uint8_t*)&limits,
        sizeof(limits),
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        (SerializedFileMetadataTail*)(void*)parent_view,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        (const SerializedFileMetadataTailLimits*)(const void*)parent_view,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(argument_failure(&parent,
        (const uint8_t*)parent_view,
        sizeof(*parent_view),
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_ARGUMENT,
        0U));
    CHECK(!output.value.implementation && guards_intact(&output));
    CHECK(parent.implementation == parent_implementation &&
        memcmp(owner_before, parent_view, sizeof(owner_before)) == 0);
    CHECK(memcmp(input_before, fixture.bytes, sizeof(input_before)) == 0);
    CHECK(memcmp(&limits, &generous_limits, sizeof(limits)) == 0);
    CHECK(g_allocations_count == allocations && g_allocated_bytes == allocated_bytes);

    ++query_count;
    const SerializedFileMetadataTailResult admitted = serialized_file_metadata_tail_create(
        &parent, fixture.bytes, fixture.end, fixture.logical_size, &limits, &output.value);
    CHECK(admitted.status == SERIALIZED_FILE_METADATA_TAIL_OK);
    const void* implementation = output.value.implementation;
    CHECK(argument_failure(&parent,
        fixture.bytes,
        fixture.end,
        fixture.logical_size,
        &limits,
        &output.value,
        SERIALIZED_FILE_METADATA_TAIL_INVALID_STATE,
        0U));
    CHECK(output.value.implementation == implementation && guards_intact(&output));
    serialized_file_directory_dispose(&parent);
    CHECK(rows_equal(&fixture, &output.value));
    serialized_file_metadata_tail_dispose(&output.value);
    serialized_file_metadata_tail_init(NULL);
    serialized_file_metadata_tail_dispose(NULL);
    CHECK(!serialized_file_metadata_tail_view(NULL));
    CHECK(!serialized_file_metadata_tail_script(NULL, 0U));
    CHECK(!serialized_file_metadata_tail_external(NULL, 0U));
    CHECK(!serialized_file_metadata_tail_reference_type(NULL, 0U));
    CHECK(!g_allocations_count && !g_allocated_bytes);
    return true;
}

static bool guarded_mapping_boundaries(void) {
#ifndef _WIN32
    const long page_result = sysconf(_SC_PAGESIZE);
    CHECK(page_result > 0);
    const size_t page_size = (size_t)page_result;
    CHECK(page_size >= TAIL_FIXTURE_CAPACITY && page_size <= SIZE_MAX / 2U);
    TailFixture fixture;
    tail_fixture_build(&fixture, true, true, SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1, false);
    const size_t cuts[] = {0U,
        73U,
        77U,
        81U,
        82U,
        84U,
        (size_t)fixture.externals[0].guid_source.offset + 3U,
        (size_t)fixture.references[0].tree.nodes_source.offset + 7U,
        fixture.end - 1U,
        fixture.end};
    for (size_t index = 0U; index < sizeof(cuts) / sizeof(cuts[0]); ++index) {
        const size_t cut = cuts[index];
        uint8_t* mapping =
            mmap(NULL, page_size * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(mapping != MAP_FAILED);
        uint8_t* bytes = mapping + page_size - cut;
        memcpy(bytes, fixture.bytes, sizeof(fixture.bytes));
        SerializedFileDirectory parent;
        CHECK(tail_fixture_parent_at(&fixture, bytes, &parent));
        CHECK(mprotect(mapping + page_size, page_size, PROT_NONE) == 0);
        GuardedTail output;
        guarded_init(&output);
        ++query_count;
        const SerializedFileMetadataTailResult result = serialized_file_metadata_tail_create(
            &parent, bytes, cut, fixture.logical_size, &generous_limits, &output.value);
        if (cut < fixture.end) {
            uint64_t work = 0U;
            const TailFixtureEvent* event = unavailable_event(&fixture, cut, &work);
            CHECK(event);
            const ExpectedFailure expected = {SERIALIZED_FILE_METADATA_TAIL_INCOMPLETE_MAPPING,
                SERIALIZED_FILE_METADATA_TAIL_LIMIT_NONE,
                event->field,
                event->row,
                cut,
                work};
            CHECK(failure_matches(result, expected) && !output.value.implementation);
        } else {
            CHECK(result.status == SERIALIZED_FILE_METADATA_TAIL_OK);
            CHECK(serialized_file_metadata_tail_reference_type(&output.value, 2U));
        }
        /* Disposal promises to read neither borrowed input nor its parent.
         * Protect both pages before exercising that lifetime boundary. */
        CHECK(mprotect(mapping, page_size, PROT_NONE) == 0);
        serialized_file_directory_dispose(&parent);
        serialized_file_metadata_tail_dispose(&output.value);
        CHECK(guards_intact(&output));
        CHECK(munmap(mapping, page_size * 2U) == 0);
        CHECK(!g_allocations_count && !g_allocated_bytes);
    }
#endif
    return true;
}

int main(void) {
    if (!wire_versions_and_empty_tables() || !every_mapping_and_metadata_end() ||
        !count_domains_and_caps() || !aggregate_byte_and_tree_limits() ||
        !work_and_storage_limits() || !unsupported_shapes_and_unsigned_trees() ||
        !optional_hashes_duplicates_and_exhaustion() || !first_failure_precedence() ||
        !binding_arguments_and_aliases() || !guarded_mapping_boundaries()) {
        return 1;
    }
    if (g_allocations_count || g_allocated_bytes) {
        fprintf(stderr, "Metadata-tail tests leaked tracked allocations\n");
        return 1;
    }
    printf("SerializedFile metadata tail: %zu independent queries passed\n", query_count);
    return 0;
}
