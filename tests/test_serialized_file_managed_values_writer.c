#include "io/serialized_file_managed_values.h"

#include "test_serialized_file_managed_values_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool node_matches(const SerializedFileSchemaNode* node,
    const ManagedExpectedValue* expected,
    const uint8_t* bytes) {
    static const struct OriginalSchema {
        size_t nodes;
        size_t strings;
        size_t node_count;
    } schemas[] = {{116U, 1588U, 46U}, {2149U, 3077U, 29U}, {3358U, 4190U, 26U}};

    const size_t schema_index = expected->type == SIZE_MAX ? 0U : expected->type + 1U;
    CHECK(schema_index < sizeof(schemas) / sizeof(schemas[0]));
    const size_t node_base = schemas[schema_index].nodes;
    const size_t strings_base = schemas[schema_index].strings;
    const size_t node_count = schemas[schema_index].node_count;
    const size_t offset = node_base + expected->node * 32U;
    CHECK(node->ordinal == expected->node && managed_test_span(node->source, bytes, offset, 32U));
    CHECK(node->version == managed_test_read_le(bytes + offset, 2U));
    CHECK(node->level == bytes[offset + 2U] && node->type_flags == bytes[offset + 3U]);
    CHECK(node->type_offset_bits == managed_test_read_le(bytes + offset + 4U, 4U));
    CHECK(node->name_offset_bits == managed_test_read_le(bytes + offset + 8U, 4U));
    CHECK(node->byte_size_bits == managed_test_read_le(bytes + offset + 12U, 4U));
    CHECK(node->index_bits == managed_test_read_le(bytes + offset + 16U, 4U));
    CHECK(node->meta_flags == managed_test_read_le(bytes + offset + 20U, 4U));
    CHECK(memcmp(node->opaque_tail, bytes + offset + 24U, 8U) == 0);
    size_t subtree_end = expected->node + 1U;
    while (subtree_end < node_count && bytes[node_base + subtree_end * 32U + 2U] > node->level) {
        ++subtree_end;
    }
    size_t parent = SIZE_MAX;
    for (size_t previous = expected->node; previous != 0U; --previous) {
        if (bytes[node_base + (previous - 1U) * 32U + 2U] < node->level) {
            parent = previous - 1U;
            break;
        }
    }
    size_t child_count = 0U;
    for (size_t child = expected->node + 1U; child < subtree_end; ++child) {
        if (bytes[node_base + child * 32U + 2U] == node->level + 1U) {
            ++child_count;
        }
    }
    const size_t first_child = child_count == 0U ? SIZE_MAX : expected->node + 1U;
    const size_t sibling =
        subtree_end < node_count && bytes[node_base + subtree_end * 32U + 2U] == node->level
        ? subtree_end
        : SIZE_MAX;
    CHECK(node->parent == parent && node->first_child == first_child &&
        node->child_count == child_count && node->next_sibling == sibling &&
        node->subtree_end == subtree_end);
    const SerializedFileSchemaString names[] = {node->type_name, node->field_name};
    const uint32_t encodings[] = {node->type_offset_bits, node->name_offset_bits};
    for (size_t index = 0U; index < 2U; ++index) {
        const bool common = (encodings[index] & UINT32_C(0x80000000)) != 0U;
        const size_t relative = encodings[index] & UINT32_C(0x7fffffff);
        CHECK(names[index].encoded_offset == encodings[index]);
        CHECK(names[index].space ==
            (common ? SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35
                    : SERIALIZED_FILE_SCHEMA_STRING_LOCAL));
        CHECK(names[index].source_offset == (common ? relative : strings_base + relative));
        CHECK(names[index].bytes != NULL && names[index].bytes[names[index].byte_count] == 0U);
        if (!common) {
            CHECK(names[index].bytes == bytes + strings_base + relative);
        }
    }
    return true;
}

static bool value_matches(const SerializedFileManagedValue* value,
    const ManagedExpectedValue* expected,
    size_t ordinal,
    const uint8_t* bytes) {
    CHECK(value && value->ordinal == ordinal && value->kind == expected->kind);
    CHECK(value->row_kind ==
        (expected->type == SIZE_MAX ? SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE
                                    : SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE));
    CHECK(value->type_ordinal == (expected->type == SIZE_MAX ? 0U : expected->type));
    CHECK(value->registry_row_ordinal == expected->row && value->raw_bits == expected->bits);
    CHECK(node_matches(&value->schema_node, expected, bytes));
    const bool string = expected->kind == SERIALIZED_FILE_MANAGED_VALUE_BYTE_STRING;
    const size_t padding = expected->padding == SIZE_MAX ? 0U : expected->padding;
    const size_t consumed = expected->size + padding + (string ? 4U : 0U);
    CHECK(managed_test_span(value->source, bytes, expected->offset, consumed));
    if (expected->padding == SIZE_MAX) {
        CHECK(managed_test_absent(value->padding_source));
    } else {
        CHECK(managed_test_span(
            value->padding_source, bytes, expected->offset + consumed - padding, padding));
    }
    if (string) {
        CHECK(value->length_bits == expected->size && value->raw_bits == 0U);
        CHECK(managed_test_absent(value->scalar_source));
        CHECK(managed_test_span(value->length_source, bytes, expected->offset, 4U));
        CHECK(managed_test_span(value->bytes_source, bytes, expected->offset + 4U, expected->size));
        CHECK(memcmp(value->bytes_source.data, expected->string, expected->size) == 0);
        CHECK(value->array_schema_ordinal == expected->node + 1U);
        CHECK(value->size_schema_ordinal == expected->node + 2U);
        CHECK(value->data_schema_ordinal == expected->node + 3U);
        CHECK(managed_test_span(value->array_schema_source,
            bytes,
            (size_t)value->schema_node.source.offset + 32U,
            32U));
        CHECK(managed_test_span(
            value->size_schema_source, bytes, (size_t)value->schema_node.source.offset + 64U, 32U));
        CHECK(managed_test_span(
            value->data_schema_source, bytes, (size_t)value->schema_node.source.offset + 96U, 32U));
    } else {
        CHECK(value->length_bits == 0U && managed_test_absent(value->length_source));
        CHECK(managed_test_absent(value->bytes_source));
        CHECK(managed_test_span(value->scalar_source, bytes, expected->offset, expected->size));
        CHECK(managed_test_read_le(value->scalar_source.data, expected->size) == expected->bits);
        CHECK(value->array_schema_ordinal == SIZE_MAX && value->size_schema_ordinal == SIZE_MAX &&
            value->data_schema_ordinal == SIZE_MAX);
        CHECK(managed_test_absent(value->array_schema_source) &&
            managed_test_absent(value->size_schema_source) &&
            managed_test_absent(value->data_schema_source));
    }
    return true;
}

static bool row_matches(const SerializedFileManagedRegistryRow* row,
    const ManagedExpectedRow* expected,
    size_t ordinal,
    const uint8_t* bytes) {
    CHECK(row && row->ordinal == ordinal && row->rid_bits == expected->rid);
    CHECK(managed_test_span(row->source, bytes, expected->offset, expected->size));
    CHECK(managed_test_span(
        row->payload_source, bytes, expected->payload_offset, expected->payload_size));
    CHECK(row->first_value == expected->first_value && row->value_count == expected->value_count);
    CHECK(row->rid_value_ordinal == expected->first_value);
    for (size_t component = 0U; component < 3U; ++component) {
        CHECK(row->identity_value_ordinals[component] == expected->first_value + component + 1U);
    }
    CHECK(row->first_payload_value == expected->first_value + 4U);
    CHECK(row->payload_value_count == expected->value_count - 4U);
    CHECK(row->selected_type_ordinal == expected->selected);
    const bool is_null = expected->type == SIZE_MAX;
    CHECK(row->kind ==
        (is_null ? SERIALIZED_FILE_MANAGED_REGISTRY_NULL
                 : SERIALIZED_FILE_MANAGED_REGISTRY_SELECTED_PAYLOAD));
    CHECK(row->type_match.kind ==
        (is_null ? SERIALIZED_FILE_REFERENCE_MISSING : SERIALIZED_FILE_REFERENCE_UNIQUE));
    CHECK(row->type_match.match_count == (is_null ? 0U : 1U));
    CHECK(row->type_match.first_ordinal == expected->type &&
        row->type_match.second_ordinal == SIZE_MAX);
    return true;
}

static bool slot_matches(
    const SerializedFileManagedRidSlot* slot, const ManagedExpectedSlot* expected, size_t ordinal) {
    CHECK(slot && slot->ordinal == ordinal && slot->value_ordinal == expected->value);
    CHECK(slot->registry_row_ordinal == expected->row && slot->rid_bits == expected->rid);
    CHECK(slot->match_count == expected->match_count &&
        slot->first_registry_row == expected->first_match && slot->second_registry_row == SIZE_MAX);
    SerializedFileManagedRidResolution resolution = SERIALIZED_FILE_MANAGED_RID_MISSING;
    if (expected->is_null) {
        resolution = SERIALIZED_FILE_MANAGED_RID_NULL;
    } else if (expected->match_count != 0U) {
        resolution = SERIALIZED_FILE_MANAGED_RID_UNIQUE;
    }
    CHECK(slot->resolution == resolution);
    return true;
}

static bool selected_matches(const SerializedFileManagedSelectedType* selected,
    size_t ordinal,
    size_t original_type,
    const uint8_t* bytes) {
    CHECK(selected && selected->ordinal == ordinal && selected->original.ordinal == original_type);
    const bool alpha = original_type == 0U;
    CHECK(managed_test_span(
        selected->original.source, bytes, alpha ? 2102U : 3311U, alpha ? 1209U : 1113U));
    CHECK(selected->schema.row_kind == SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE &&
        selected->schema.type_ordinal == original_type);
    CHECK(selected->schema.node_count == (alpha ? 29U : 26U));
    CHECK(selected->context.kind == SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD &&
        selected->context.registry_omitted);
    CHECK(selected->context.root == 0U && selected->context.first_child == 1U);
    CHECK(selected->context.traversal_end == (alpha ? 8U : 5U));
    CHECK(selected->context.registry == selected->context.traversal_end &&
        selected->context.registry_end == selected->schema.node_count);
    CHECK(managed_test_span(
        selected->schema.type_entry_source, bytes, alpha ? 2102U : 3311U, alpha ? 1209U : 1113U));
    CHECK(selected->schema.prefix.header.header_source.data == bytes);
    return true;
}

static bool host_matches(const CommonFileBytes* file, size_t ordinal) {
    const ManagedExpectedHost* expected = &managed_expected_hosts[ordinal];
    ManagedTestParents parents;
    CHECK(managed_test_parents_create(file->data, file->size, &parents));
    SerializedFileManagedValues owner;
    serialized_file_managed_values_init(&owner);
    const SerializedFileManagedValuesLimits limits = managed_test_limits();
    const SerializedFileManagedValuesResult result = serialized_file_managed_values_create(
        &parents.directory, &parents.tail, ordinal, file->data, file->size, &limits, &owner);
    managed_test_parents_dispose(&parents);
    CHECK(result.status == SERIALIZED_FILE_MANAGED_VALUES_OK);
    CHECK(result.limit == SERIALIZED_FILE_MANAGED_VALUES_LIMIT_NONE);
    const SerializedFileManagedValuesView* view = serialized_file_managed_values_view(&owner);
    CHECK(
        view && view->object.ordinal == ordinal && view->object.path_id_bits == expected->path_id);
    CHECK(view->value_count == expected->value_count &&
        view->registry_row_count == expected->row_count);
    CHECK(view->rid_slot_count == expected->slot_count &&
        view->selected_type_count == expected->selected_count);
    CHECK(view->array_element_count == expected->array_count && view->array_count_value == 9U);
    CHECK(view->registry_version_value == 10U + expected->array_count &&
        view->registry_count_value == 11U + expected->array_count);
    CHECK(view->registry_version_bits == 2U && view->registry_count_bits == expected->row_count);
    CHECK(view->missing_rid_slots == 0U && view->ambiguous_rid_slots == 0U);
    CHECK(view->consumed_bytes == expected->size &&
        view->retained_bytes == result.required_retained_bytes);
    CHECK(managed_test_span(view->payload_source, file->data, expected->offset, expected->size));
    CHECK(managed_test_span(
        view->registry_source, file->data, expected->registry_offset, expected->registry_size));
    CHECK(managed_test_absent(view->registry_padding_source));
    CHECK(view->schema.node_count == 46U && view->context.registry == 25U &&
        view->context.traversal_end == 46U && !view->context.registry_omitted);
    SerializedFileManagedValue copied_values[64];
    SerializedFileManagedRegistryRow copied_rows[8];
    SerializedFileManagedRidSlot copied_slots[16];
    SerializedFileManagedSelectedType copied_types[2];
    size_t cursor = expected->offset;
    uint64_t scalar_bytes = 0U;
    uint64_t string_bytes = 0U;
    uint64_t padding_bytes = 0U;
    for (size_t index = 0U; index < expected->value_count; ++index) {
        const SerializedFileManagedValue* value =
            serialized_file_managed_values_value(&owner, index);
        CHECK(value_matches(
            value, &managed_expected_values[expected->first_value + index], index, file->data));
        CHECK(value->source.offset == cursor);
        cursor += (size_t)value->source.size;
        scalar_bytes += value->scalar_source.size;
        string_bytes += value->bytes_source.size;
        padding_bytes += value->padding_source.size;
        copied_values[index] = *value;
    }
    CHECK(cursor == expected->offset + expected->size && view->scalar_bytes == scalar_bytes &&
        view->string_bytes == string_bytes && view->padding_bytes == padding_bytes);
    for (size_t index = 0U; index < expected->row_count; ++index) {
        const SerializedFileManagedRegistryRow* row =
            serialized_file_managed_values_registry_row(&owner, index);
        CHECK(row_matches(
            row, &managed_expected_rows[expected->first_row + index], index, file->data));
        copied_rows[index] = *row;
    }
    for (size_t index = 0U; index < expected->slot_count; ++index) {
        const SerializedFileManagedRidSlot* slot =
            serialized_file_managed_values_rid_slot(&owner, index);
        CHECK(slot_matches(slot, &managed_expected_slots[expected->first_slot + index], index));
        copied_slots[index] = *slot;
    }
    for (size_t index = 0U; index < expected->selected_count; ++index) {
        const SerializedFileManagedSelectedType* type =
            serialized_file_managed_values_selected_type(&owner, index);
        const size_t original_type = ordinal == 8U ? 1U : index;
        CHECK(selected_matches(type, index, original_type, file->data));
        copied_types[index] = *type;
    }
    CHECK(!serialized_file_managed_values_value(&owner, expected->value_count));
    CHECK(!serialized_file_managed_values_registry_row(&owner, expected->row_count));
    CHECK(!serialized_file_managed_values_rid_slot(&owner, expected->slot_count));
    CHECK(!serialized_file_managed_values_selected_type(&owner, expected->selected_count));
    const SerializedFileManagedValuesView copied_view = *view;
    serialized_file_managed_values_dispose(&owner);
    serialized_file_managed_values_dispose(&owner);
    CHECK(!serialized_file_managed_values_view(&owner));
    CHECK(!serialized_file_managed_values_value(&owner, 0U));
    CHECK(!serialized_file_managed_values_registry_row(&owner, 0U));
    CHECK(!serialized_file_managed_values_rid_slot(&owner, 0U));
    CHECK(!serialized_file_managed_values_selected_type(&owner, 0U));
    CHECK(managed_test_span(
        copied_view.payload_source, file->data, expected->offset, expected->size));
    for (size_t index = 0U; index < expected->value_count; ++index) {
        CHECK(value_matches(&copied_values[index],
            &managed_expected_values[expected->first_value + index],
            index,
            file->data));
    }
    for (size_t index = 0U; index < expected->row_count; ++index) {
        CHECK(row_matches(&copied_rows[index],
            &managed_expected_rows[expected->first_row + index],
            index,
            file->data));
    }
    for (size_t index = 0U; index < expected->slot_count; ++index) {
        CHECK(slot_matches(
            &copied_slots[index], &managed_expected_slots[expected->first_slot + index], index));
    }
    for (size_t index = 0U; index < expected->selected_count; ++index) {
        CHECK(
            selected_matches(&copied_types[index], index, ordinal == 8U ? 1U : index, file->data));
    }
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
    bool passed = true;
    for (size_t ordinal = 0U; ordinal < MANAGED_WRITER_HOST_COUNT && passed; ++ordinal) {
        passed = host_matches(&file, ordinal);
    }
    common_file_bytes_dispose(&file);
    if (!passed) {
        return EXIT_FAILURE;
    }
    puts("Managed writer: 9 hosts, 222 values, 19 registry rows and 34 RID slots "
         "match original bytes.");
    return EXIT_SUCCESS;
}
