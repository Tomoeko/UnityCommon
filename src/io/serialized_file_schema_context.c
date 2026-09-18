// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_schema_context.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_schema_internal.h"
#include "typetree_common_strings_internal.h"

enum {
    NODE_BYTES = 32,
    NODE_LEVEL_OFFSET = 2,
    NODE_FLAGS_OFFSET = 3,
    REGISTRY_FLAGS = 4
};

static SerializedFileSchemaContextResult initial_result(void) {
    const SerializedFileSchemaContextResult result = {.status = SERIALIZED_FILE_SCHEMA_CONTEXT_OK,
        .type_ordinal = SIZE_MAX,
        .node_ordinal = SIZE_MAX,
        .error_offset = UINT64_MAX};
    return result;
}

static bool valid_arguments(const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind kind,
    const SerializedFileSchemaContext* output) {
    return schema && output &&
        (kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT ||
            kind == SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD) &&
        !serialized_file_storage_overlaps_internal(
            schema, sizeof(*schema), output, sizeof(*output));
}

static bool ranges_are_disjoint(const SerializedFileSchema* schema,
    const void* storage,
    size_t storage_size,
    const SerializedFileSchemaView* view,
    const SerializedFileSchemaContext* output) {
    size_t common_size;
    const uint8_t* common = typetree_common_string_table(&common_size);
    const size_t known_size =
        (size_t)(view->type_entry_source.offset + view->type_entry_source.size);
    const void* const ranges[] = {
        schema, storage, view->prefix.header.header_source.data, common, output};
    const size_t sizes[] = {
        sizeof(*schema), storage_size, known_size, common_size, sizeof(*output)};
    for (size_t first = 0U; first < sizeof(ranges) / sizeof(ranges[0]); ++first) {
        for (size_t second = first + 1U; second < sizeof(ranges) / sizeof(ranges[0]); ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool charge(SerializedFileSchemaContextResult* result,
    uint64_t max_work,
    SerializedFileSchemaContextField field,
    size_t node,
    uint64_t offset) {
    if (result->work_used == max_work) {
        result->status = SERIALIZED_FILE_SCHEMA_CONTEXT_WORK_LIMIT;
        result->field = field;
        result->node_ordinal = node;
        result->error_offset = offset;
        return false;
    }
    ++result->work_used;
    return true;
}

static bool inspect_registry_node(const SerializedFileSchemaNode* node,
    size_t node_count,
    SerializedFileSchemaContext* context,
    SerializedFileSchemaContextResult* result) {
    if ((node->type_flags & REGISTRY_FLAGS) == 0U) {
        return true;
    }
    if (node->type_flags != REGISTRY_FLAGS) {
        result->status = SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_FLAGS;
        result->field = SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_TYPE_FLAGS;
        result->node_ordinal = node->ordinal;
        result->error_offset = node->source.offset + NODE_FLAGS_OFFSET;
        return false;
    }
    /* The addressed selected-child loop stops entirely at its first registry.
     * Restrict the supported subset to a sole final direct child; do not erase
     * nested markers or resume a later root sibling. */
    if (node->parent != 0U || node->next_sibling != SIZE_MAX || node->subtree_end != node_count ||
        context->registry != SIZE_MAX) {
        result->status = SERIALIZED_FILE_SCHEMA_CONTEXT_UNSUPPORTED_REGISTRY_POSITION;
        result->field = SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_LEVEL;
        result->node_ordinal = node->ordinal;
        result->error_offset = node->source.offset + NODE_LEVEL_OFFSET;
        return false;
    }
    context->registry = node->ordinal;
    context->registry_end = node->subtree_end;
    return true;
}

SerializedFileSchemaContextResult serialized_file_schema_context_query(
    const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind kind,
    uint64_t max_work,
    SerializedFileSchemaContext* out_context) {
    SerializedFileSchemaContextResult result = initial_result();
    if (!valid_arguments(schema, kind, out_context)) {
        result.status = SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT;
        return result;
    }
    const void* storage;
    size_t storage_size;
    if (!serialized_file_schema_storage_range_internal(schema, &storage, &storage_size)) {
        result.status = SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_STATE;
        return result;
    }
    const SerializedFileSchemaView* view = serialized_file_schema_view(schema);
    /* A genuine schema retained a mapped metadata prefix through this row.
     * The logical complete file size is deliberately not a mapping extent. */
    if (view->type_entry_source.offset > SIZE_MAX ||
        view->type_entry_source.size > SIZE_MAX - view->type_entry_source.offset) {
        result.status = SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_STATE;
        return result;
    }
    if (!ranges_are_disjoint(schema, storage, storage_size, view, out_context)) {
        result.status = SERIALIZED_FILE_SCHEMA_CONTEXT_INVALID_ARGUMENT;
        return result;
    }
    result.type_ordinal = view->type_ordinal;
    const SerializedFileSchemaRowKind required_row =
        kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT
        ? SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE
        : SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE;
    if (view->row_kind != required_row) {
        result.status = SERIALIZED_FILE_SCHEMA_CONTEXT_WRONG_ROW_KIND;
        return result;
    }
    if (!charge(
            &result, max_work, SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return result;
    }
    SerializedFileSchemaContext context = {.kind = kind,
        .engine_version = view->engine_version,
        .row_kind = view->row_kind,
        .type_ordinal = view->type_ordinal,
        .header_source = view->prefix.header.header_source,
        .file_size = view->prefix.header.file_size,
        .endian_selector = view->prefix.header.endian_selector,
        .type_entry_source = view->type_entry_source,
        .tree = view->tree,
        .root = 0U,
        .first_child = SIZE_MAX,
        .traversal_end = view->node_count,
        .registry = SIZE_MAX,
        .registry_end = SIZE_MAX};
    for (size_t ordinal = 0U; ordinal < view->node_count; ++ordinal) {
        const uint64_t offset = view->tree.nodes_source.offset + ordinal * NODE_BYTES;
        if (!charge(&result,
                max_work,
                SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_TYPE_FLAGS,
                ordinal,
                offset + NODE_FLAGS_OFFSET)) {
            return result;
        }
        const SerializedFileSchemaNode* node = serialized_file_schema_node(schema, ordinal);
        if (ordinal == 0U) {
            context.first_child = node->first_child;
        }
        if (!inspect_registry_node(node, view->node_count, &context, &result)) {
            return result;
        }
    }
    if (!charge(
            &result, max_work, SERIALIZED_FILE_SCHEMA_CONTEXT_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return result;
    }
    if (kind == SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD &&
        context.registry != SIZE_MAX) {
        context.traversal_end = context.registry;
        context.registry_omitted = true;
    }
    *out_context = context;
    return result;
}
