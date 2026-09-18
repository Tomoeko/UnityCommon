// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_values.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_schema_internal.h"
#include "serialized_metadata_reader_internal.h"
#include "typetree_common_strings_internal.h"

#include "common/common.h"

enum {
    TEXT_ASSET_CLASS = 49,
    MONO_BEHAVIOUR_CLASS = 114,
    TEXT_ASSET_NODES = 9,
    MONO_BEHAVIOUR_NODES = 13,
    MONO_BEHAVIOUR_TEXT_ASSET_NODES = 15,
    MONO_BEHAVIOUR_FIELD_NODE = 12,
    TEXT_ASSET_VALUES = 3,
    MONO_BEHAVIOUR_VALUES = 10,
    MONO_BEHAVIOUR_TEXT_ASSET_VALUES = 12,
    MONO_BEHAVIOUR_FIELD_VALUE = 9,
    LENGTH_BYTES = 4,
    ALIGNMENT = 4,
    ORDINARY_ALIGNMENT_FLAG = 0x4000,
    TYPE_NAME_OFFSET = 4,
    FIELD_NAME_OFFSET = 8,
    OBJECT_BYTE_SIZE_OFFSET = 16
};

typedef struct ValuesStorage {
    SerializedFileValuesView view;
    SerializedFileValue values[];
} ValuesStorage;

/* Only the three fixed profiles select a count. Their complete tail sizes are
 * representable before any caller-controlled storage limit is considered. */
_Static_assert(MONO_BEHAVIOUR_TEXT_ASSET_VALUES <=
        (SIZE_MAX - sizeof(ValuesStorage)) / sizeof(SerializedFileValue),
    "supported value profiles must fit one allocation");

typedef struct NodeProfile {
    const char* type_name;
    const char* field_name;
    size_t type_length;
    size_t field_length;
    uint8_t level;
    uint8_t flags;
    uint32_t byte_size;
    uint32_t meta_flags;
    size_t parent;
    size_t first_child;
    size_t next_sibling;
    size_t child_count;
    size_t subtree_end;
} NodeProfile;

/* The complete admitted schema is pinned by both official writer fixtures.
 * Treat different flags or opaque tails as unsupported; do not infer their
 * meaning from the node names or erase them to fit this profile. */
static const NodeProfile text_asset_nodes[TEXT_ASSET_NODES] = {
    {"TextAsset", "Base", 9U, 4U, 0U, 0U, UINT32_MAX, 0x8000U, SIZE_MAX, 1U, SIZE_MAX, 2U, 9U},
    {"string", "m_Name", 6U, 6U, 1U, 0U, UINT32_MAX, 0x88001U, 0U, 2U, 5U, 1U, 5U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x84001U, 1U, 3U, SIZE_MAX, 2U, 5U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x80001U, 2U, SIZE_MAX, 4U, 0U, 4U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x80001U, 2U, SIZE_MAX, SIZE_MAX, 0U, 5U},
    {"string", "m_Script", 6U, 8U, 1U, 0U, UINT32_MAX, 0x4008001U, 0U, 6U, SIZE_MAX, 1U, 9U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x4004001U, 5U, 7U, SIZE_MAX, 2U, 9U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x4000001U, 6U, SIZE_MAX, 8U, 0U, 8U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x4000001U, 6U, SIZE_MAX, SIZE_MAX, 0U, 9U}};

/* The final name is an authored field, not a Unity control name. NULL admits
 * its genuine descriptor without a lexical scan or a fixture-name allowlist. */
static const NodeProfile mono_behaviour_nodes[MONO_BEHAVIOUR_NODES] = {
    {"MonoBehaviour",
        "Base",
        13U,
        4U,
        0U,
        0U,
        UINT32_MAX,
        0x8000U,
        SIZE_MAX,
        1U,
        SIZE_MAX,
        5U,
        13U},
    {"PPtr<GameObject>", "m_GameObject", 16U, 12U, 1U, 0U, 12U, 0x41U, 0U, 2U, 4U, 2U, 4U},
    {"int", "m_FileID", 3U, 8U, 2U, 0U, 4U, 0x41U, 1U, SIZE_MAX, 3U, 0U, 3U},
    {"SInt64", "m_PathID", 6U, 8U, 2U, 0U, 8U, 0x41U, 1U, SIZE_MAX, SIZE_MAX, 0U, 4U},
    {"UInt8", "m_Enabled", 5U, 9U, 1U, 0U, 1U, 0x4101U, 0U, SIZE_MAX, 5U, 0U, 5U},
    {"PPtr<MonoScript>", "m_Script", 16U, 8U, 1U, 0U, 12U, 0U, 0U, 6U, 8U, 2U, 8U},
    {"int", "m_FileID", 3U, 8U, 2U, 0U, 4U, 0x800001U, 5U, SIZE_MAX, 7U, 0U, 7U},
    {"SInt64", "m_PathID", 6U, 8U, 2U, 0U, 8U, 0x800001U, 5U, SIZE_MAX, SIZE_MAX, 0U, 8U},
    {"string", "m_Name", 6U, 6U, 1U, 0U, UINT32_MAX, 0x88001U, 0U, 9U, 12U, 1U, 12U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x84001U, 8U, 10U, SIZE_MAX, 2U, 12U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x80001U, 9U, SIZE_MAX, 11U, 0U, 11U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x80001U, 9U, SIZE_MAX, SIZE_MAX, 0U, 12U},
    {"int", NULL, 3U, 0U, 1U, 0U, 4U, 0U, 0U, SIZE_MAX, SIZE_MAX, 0U, 13U},
};

/* The observed dollar is part of the complete type spelling. This fixed tail
 * shares the preceding twelve nodes, not a general PPtr or managed-type rule. */
static const NodeProfile
    mono_behaviour_text_asset_tail[MONO_BEHAVIOUR_TEXT_ASSET_NODES - MONO_BEHAVIOUR_FIELD_NODE] = {
        {"PPtr<$TextAsset>", NULL, 16U, 0U, 1U, 0U, 12U, 0U, 0U, 13U, SIZE_MAX, 2U, 15U},
        {"int", "m_FileID", 3U, 8U, 2U, 0U, 4U, 0x800001U, 12U, SIZE_MAX, 14U, 0U, 14U},
        {"SInt64", "m_PathID", 6U, 8U, 2U, 0U, 8U, 0x800001U, 12U, SIZE_MAX, SIZE_MAX, 0U, 15U}};

typedef struct ValueRole {
    size_t schema_ordinal;
    SerializedFileValueKind kind;
    size_t parent;
    size_t first_child;
    size_t next_sibling;
    size_t child_count;
    size_t subtree_end;
} ValueRole;

static const ValueRole text_asset_roles[TEXT_ASSET_VALUES] = {
    {0U, SERIALIZED_FILE_VALUE_CONTAINER, SIZE_MAX, 1U, SIZE_MAX, 2U, 3U},
    {1U, SERIALIZED_FILE_VALUE_BYTE_STRING, 0U, SIZE_MAX, 2U, 0U, 2U},
    {5U, SERIALIZED_FILE_VALUE_BYTE_STRING, 0U, SIZE_MAX, SIZE_MAX, 0U, 3U}};

static const ValueRole mono_behaviour_roles[MONO_BEHAVIOUR_VALUES] = {
    {0U, SERIALIZED_FILE_VALUE_CONTAINER, SIZE_MAX, 1U, SIZE_MAX, 5U, 10U},
    {1U, SERIALIZED_FILE_VALUE_CONTAINER, 0U, 2U, 4U, 2U, 4U},
    {2U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 1U, SIZE_MAX, 3U, 0U, 3U},
    {3U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 1U, SIZE_MAX, SIZE_MAX, 0U, 4U},
    {4U, SERIALIZED_FILE_VALUE_UNSIGNED_INTEGER, 0U, SIZE_MAX, 5U, 0U, 5U},
    {5U, SERIALIZED_FILE_VALUE_CONTAINER, 0U, 6U, 8U, 2U, 8U},
    {6U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 5U, SIZE_MAX, 7U, 0U, 7U},
    {7U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 5U, SIZE_MAX, SIZE_MAX, 0U, 8U},
    {8U, SERIALIZED_FILE_VALUE_BYTE_STRING, 0U, SIZE_MAX, 9U, 0U, 9U},
    {12U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 0U, SIZE_MAX, SIZE_MAX, 0U, 10U}};

static const ValueRole mono_behaviour_text_asset_tail_roles[MONO_BEHAVIOUR_TEXT_ASSET_VALUES -
    MONO_BEHAVIOUR_FIELD_VALUE] = {
    {12U, SERIALIZED_FILE_VALUE_CONTAINER, 0U, 10U, SIZE_MAX, 2U, 12U},
    {13U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 9U, SIZE_MAX, 11U, 0U, 11U},
    {14U, SERIALIZED_FILE_VALUE_SIGNED_INTEGER, 9U, SIZE_MAX, SIZE_MAX, 0U, 12U}};

typedef struct ObjectProfile {
    /* Only the fixed selectors below combine these base tables with the
     * reference tail; callers must not index them by the profile counts. */
    const NodeProfile* nodes;
    size_t node_count;
    const ValueRole* roles;
    size_t value_count;
    size_t maximum_depth;
} ObjectProfile;

static const ObjectProfile text_asset_profile = {
    text_asset_nodes, TEXT_ASSET_NODES, text_asset_roles, TEXT_ASSET_VALUES, 1U};
static const ObjectProfile mono_behaviour_profile = {
    mono_behaviour_nodes, MONO_BEHAVIOUR_NODES, mono_behaviour_roles, MONO_BEHAVIOUR_VALUES, 2U};
static const ObjectProfile mono_behaviour_text_asset_profile = {mono_behaviour_nodes,
    MONO_BEHAVIOUR_TEXT_ASSET_NODES,
    mono_behaviour_roles,
    MONO_BEHAVIOUR_TEXT_ASSET_VALUES,
    2U};

/* These selectors receive ordinals bounded by one of the fixed profiles. The
 * reference variant changes only its authored tail and the complete root end;
 * all shared prefix controls and sibling links remain exact table facts. */
static NodeProfile profile_node_at(const ObjectProfile* profile, size_t ordinal) {
    if (profile == &mono_behaviour_text_asset_profile) {
        if (ordinal >= MONO_BEHAVIOUR_FIELD_NODE) {
            return mono_behaviour_text_asset_tail[ordinal - MONO_BEHAVIOUR_FIELD_NODE];
        }
        if (ordinal == 0U) {
            NodeProfile root = mono_behaviour_nodes[0];
            root.subtree_end = MONO_BEHAVIOUR_TEXT_ASSET_NODES;
            return root;
        }
    }
    return profile->nodes[ordinal];
}

static ValueRole profile_role_at(const ObjectProfile* profile, size_t ordinal) {
    if (profile == &mono_behaviour_text_asset_profile) {
        if (ordinal >= MONO_BEHAVIOUR_FIELD_VALUE) {
            return mono_behaviour_text_asset_tail_roles[ordinal - MONO_BEHAVIOUR_FIELD_VALUE];
        }
        if (ordinal == 0U) {
            ValueRole root = mono_behaviour_roles[0];
            root.subtree_end = MONO_BEHAVIOUR_TEXT_ASSET_VALUES;
            return root;
        }
    }
    return profile->roles[ordinal];
}

typedef struct ValuesBuild {
    const SerializedFileValuesLimits* limits;
    SerializedFileValuesResult result;
    const SerializedFileSchema* schema;
    const uint8_t* bytes;
    SerializedFileValuesView view;
    const ObjectProfile* profile;
} ValuesBuild;

typedef struct ValueCursor {
    uint64_t position;
    uint64_t end;
    uint64_t string_bytes;
    uint64_t padding_bytes;
    uint64_t integer_bytes;
} ValueCursor;

static SerializedFileValuesResult initial_result(void) {
    const SerializedFileValuesResult result = {.status = SERIALIZED_FILE_VALUES_OK,
        .object_ordinal = SIZE_MAX,
        .type_ordinal = SIZE_MAX,
        .node_ordinal = SIZE_MAX,
        .error_offset = UINT64_MAX,
        .context_result = {.status = SERIALIZED_FILE_SCHEMA_CONTEXT_OK,
            .type_ordinal = SIZE_MAX,
            .node_ordinal = SIZE_MAX,
            .error_offset = UINT64_MAX}};
    return result;
}

static bool reject(ValuesBuild* build,
    SerializedFileValuesStatus status,
    SerializedFileValuesLimit limit,
    SerializedFileValuesField field,
    size_t node,
    uint64_t offset) {
    build->result.status = status;
    build->result.limit = limit;
    build->result.field = field;
    build->result.node_ordinal = node;
    build->result.error_offset = offset;
    return false;
}

static bool reject_owner(
    ValuesBuild* build, SerializedFileValuesStatus status, SerializedFileValuesLimit limit) {
    return reject(build, status, limit, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX);
}

static bool charge(ValuesBuild* build,
    uint64_t amount,
    SerializedFileValuesField field,
    size_t node,
    uint64_t offset) {
    if (amount > build->limits->max_work - build->result.work_used) {
        return reject(build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_VALUES_LIMIT_WORK,
            field,
            node,
            offset);
    }
    build->result.work_used += amount;
    return true;
}

static bool disjoint_ranges(const void* const* ranges, const size_t* sizes, size_t count) {
    for (size_t first = 0U; first < count; ++first) {
        for (size_t second = first + 1U; second < count; ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool admit_owner_ranges(ValuesBuild* build,
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    const uint8_t* bytes,
    size_t mapped_size,
    const SerializedFileValues* output) {
    const void* directory_storage;
    size_t directory_size;
    if (!serialized_file_directory_storage_range_internal(
            directory, &directory_storage, &directory_size)) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_STATE, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    const void* schema_storage = NULL;
    size_t schema_size = 0U;
    const bool live_schema =
        serialized_file_schema_storage_range_internal(schema, &schema_storage, &schema_size);
    const SerializedFileDirectoryView* directory_view = serialized_file_directory_view(directory);
    uint64_t known_end =
        directory_view->parsed_metadata_source.offset + directory_view->parsed_metadata_source.size;
    const void* other_backing = NULL;
    size_t other_backing_size = 0U;
    if (live_schema) {
        const SerializedFileSchemaView* schema_view = serialized_file_schema_view(schema);
        /* A second backing is valid input to the lineage check below. Include
         * its known prefix separately only when it is not the directory's. */
        if (schema_view->prefix.header.header_source.data !=
            directory_view->prefix.header.header_source.data) {
            other_backing_size = (size_t)(schema_view->type_entry_source.offset +
                schema_view->type_entry_source.size);
            other_backing = schema_view->prefix.header.header_source.data;
        } else {
            const uint64_t schema_end =
                schema_view->type_entry_source.offset + schema_view->type_entry_source.size;
            if (schema_end > known_end) {
                known_end = schema_end;
            }
        }
    }
    size_t common_size;
    const uint8_t* common = typetree_common_string_table(&common_size);
    const void* const ranges[] = {directory,
        schema,
        build->limits,
        output,
        directory_storage,
        schema_storage,
        directory_view->prefix.header.header_source.data,
        common,
        other_backing};
    const size_t sizes[] = {sizeof(*directory),
        sizeof(*schema),
        sizeof(*build->limits),
        sizeof(*output),
        directory_size,
        schema_size,
        (size_t)known_end,
        common_size,
        other_backing_size};
    if (!disjoint_ranges(ranges, sizes, sizeof(ranges) / sizeof(ranges[0]))) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_ARGUMENT, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    for (size_t index = 0U; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
        if (index == 6U || index == 8U) {
            continue;
        }
        if (serialized_file_storage_overlaps_internal(
                bytes, mapped_size, ranges[index], sizes[index])) {
            return reject_owner(
                build, SERIALIZED_FILE_VALUES_INVALID_ARGUMENT, SERIALIZED_FILE_VALUES_LIMIT_NONE);
        }
    }
    return true;
}

static bool admit_arguments(ValuesBuild* build,
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    const uint8_t* bytes,
    size_t mapped_size,
    SerializedFileValues* output) {
    if (!directory || !schema || !bytes || !build->limits || !output) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_ARGUMENT, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    const void* const handles[] = {directory, schema, build->limits, output};
    const size_t handle_sizes[] = {
        sizeof(*directory), sizeof(*schema), sizeof(*build->limits), sizeof(*output)};
    if (!disjoint_ranges(handles, handle_sizes, sizeof(handles) / sizeof(handles[0]))) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_ARGUMENT, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    if (!admit_owner_ranges(build, directory, schema, bytes, mapped_size, output)) {
        return false;
    }
    if (output->implementation) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_STATE, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    return true;
}

static bool same_span(SerializedFilePrefixSpan first, SerializedFilePrefixSpan second) {
    return first.data == second.data && first.offset == second.offset && first.size == second.size;
}

static bool same_tree(
    const SerializedFileDirectoryTree* first, const SerializedFileDirectoryTree* second) {
    return first->node_count == second->node_count &&
        first->string_byte_count == second->string_byte_count &&
        same_span(first->node_count_source, second->node_count_source) &&
        same_span(first->string_count_source, second->string_count_source) &&
        same_span(first->nodes_source, second->nodes_source) &&
        same_span(first->strings_source, second->strings_source);
}

static bool same_original_type(const SerializedFileDirectoryView* directory,
    const SerializedFileDirectoryTypeRow* type,
    const SerializedFileSchemaView* schema) {
    return schema->engine_version == directory->engine_version &&
        schema->row_kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE &&
        schema->type_ordinal == type->ordinal &&
        same_span(schema->prefix.header.header_source, directory->prefix.header.header_source) &&
        schema->prefix.header.file_size == directory->prefix.header.file_size &&
        schema->prefix.header.endian_selector == directory->prefix.header.endian_selector &&
        same_span(schema->type_entry_source, type->source) &&
        same_tree(&schema->tree, &type->tree) && schema->class_id_bits == type->class_id_bits &&
        schema->script_index_bits == type->script_index_bits &&
        schema->stripped_raw == type->stripped_raw &&
        schema->has_script_hash == type->has_script_hash &&
        same_span(schema->script_hash_source, type->script_hash_source) &&
        same_span(schema->type_hash_source, type->type_hash_source) &&
        same_span(schema->dependency_count_source, type->dependency_count_source) &&
        same_span(schema->dependency_words_source, type->dependency_words_source) &&
        schema->dependency_count == type->dependency_count;
}

static bool admit_source(ValuesBuild* build,
    const SerializedFileDirectory* directory,
    size_t object_ordinal,
    size_t mapped_size) {
    const SerializedFileDirectoryView* view = serialized_file_directory_view(directory);
    if (view->engine_version != SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_UNSUPPORTED_ENGINE, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    if (view->prefix.header.endian_selector != 0U) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_UNSUPPORTED_ENDIAN, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    const SerializedFileDirectoryObjectRow* object =
        serialized_file_directory_object(directory, object_ordinal);
    if (!object) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_ARGUMENT, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    build->result.object_ordinal = object_ordinal;
    build->result.type_ordinal = object->type_ordinal;
    const SerializedFileDirectoryTypeRow* type =
        serialized_file_directory_type(directory, object->type_ordinal);
    if (!type->has_tree) {
        return reject(build,
            SERIALIZED_FILE_VALUES_NO_EMBEDDED_SCHEMA,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_TYPE,
            SIZE_MAX,
            type->source.offset);
    }
    if (build->bytes != view->prefix.header.header_source.data ||
        mapped_size > view->prefix.header.file_size) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_SOURCE_MISMATCH, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    /* The addressed converter advances a signed32 object-relative cursor.
     * Admit its complete nonnegative domain before any payload mapping or read.
     */
    if (object->payload.size > INT32_MAX) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_SIZE,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            object->source.offset + OBJECT_BYTE_SIZE_OFFSET);
    }
    const uint64_t object_end = object->payload.offset + object->payload.size;
    if (object_end > mapped_size) {
        return reject(build,
            SERIALIZED_FILE_VALUES_INCOMPLETE_MAPPING,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            mapped_size);
    }
    const SerializedFileSchemaView* schema = serialized_file_schema_view(build->schema);
    if (!schema) {
        return reject_owner(
            build, SERIALIZED_FILE_VALUES_INVALID_STATE, SERIALIZED_FILE_VALUES_LIMIT_NONE);
    }
    if (!same_original_type(view, type, schema)) {
        return reject(build,
            SERIALIZED_FILE_VALUES_SOURCE_MISMATCH,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_TYPE,
            SIZE_MAX,
            type->source.offset);
    }
    build->view.object = *object;
    build->view.schema = *schema;
    return true;
}

static bool qualify_context(ValuesBuild* build) {
    build->result.context_attempted = true;
    build->result.context_result = serialized_file_schema_context_query(build->schema,
        SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT,
        build->limits->max_work - build->result.work_used,
        &build->view.context);
    const SerializedFileSchemaContextResult* nested = &build->result.context_result;
    build->result.work_used += nested->work_used;
    if (nested->status != SERIALIZED_FILE_SCHEMA_CONTEXT_OK) {
        const bool work_limit = nested->status == SERIALIZED_FILE_SCHEMA_CONTEXT_WORK_LIMIT;
        return reject(build,
            work_limit ? SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED
                       : SERIALIZED_FILE_VALUES_CONTEXT_REJECTED,
            work_limit ? SERIALIZED_FILE_VALUES_LIMIT_WORK : SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_CONTEXT,
            nested->node_ordinal,
            nested->error_offset);
    }
    return true;
}

static bool name_matches(ValuesBuild* build,
    const SerializedFileSchemaString* actual,
    const char* expected,
    size_t expected_length,
    SerializedFileValuesField field,
    size_t node,
    uint64_t offset) {
    if (!charge(build, 1U, field, node, offset)) {
        return false;
    }
    if (!expected) {
        return true;
    }
    if (actual->byte_count != expected_length) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            field,
            node,
            offset);
    }
    for (size_t index = 0U; index < expected_length; ++index) {
        if (!charge(build, 1U, field, node, offset)) {
            return false;
        }
        if (actual->bytes[index] != (uint8_t)expected[index]) {
            return reject(build,
                SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA,
                SERIALIZED_FILE_VALUES_LIMIT_NONE,
                field,
                node,
                offset);
        }
    }
    return true;
}

static bool profile_node_matches(
    const SerializedFileSchemaNode* node, const NodeProfile* expected) {
    static const uint8_t empty_tail[8] = {0U};
    return node->version == 1U && node->level == expected->level &&
        node->type_flags == expected->flags && node->byte_size_bits == expected->byte_size &&
        node->index_bits == node->ordinal && node->meta_flags == expected->meta_flags &&
        memcmp(node->opaque_tail, empty_tail, sizeof(empty_tail)) == 0 &&
        node->parent == expected->parent && node->first_child == expected->first_child &&
        node->next_sibling == expected->next_sibling &&
        node->child_count == expected->child_count && node->subtree_end == expected->subtree_end;
}

static bool qualify_profile(ValuesBuild* build, bool allow_ordinary) {
    const SerializedFileSchemaView* view = &build->view.schema;
    const bool text_asset = view->class_id_bits == TEXT_ASSET_CLASS &&
        view->script_index_bits == UINT16_MAX && view->stripped_raw == 0U && !view->has_script_hash;
    const bool mono_behaviour = allow_ordinary && view->class_id_bits == MONO_BEHAVIOUR_CLASS &&
        view->script_index_bits <= INT16_MAX && view->stripped_raw == 0U && view->has_script_hash &&
        view->script_hash_source.size == 16U && view->type_hash_source.size == 16U;
    if (!text_asset && !mono_behaviour) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_TYPE,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_TYPE,
            SIZE_MAX,
            view->type_entry_source.offset);
    }
    if (text_asset) {
        build->profile = &text_asset_profile;
    } else if (view->node_count == MONO_BEHAVIOUR_TEXT_ASSET_NODES) {
        build->profile = &mono_behaviour_text_asset_profile;
    } else {
        build->profile = &mono_behaviour_profile;
    }
    const ObjectProfile* profile = build->profile;
    if (view->node_count != profile->node_count || build->view.context.registry != SIZE_MAX ||
        build->view.context.traversal_end != profile->node_count) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
            SIZE_MAX,
            view->tree.node_count_source.offset);
    }
    for (size_t ordinal = 0U; ordinal < profile->node_count; ++ordinal) {
        const SerializedFileSchemaNode* node = serialized_file_schema_node(build->schema, ordinal);
        const NodeProfile expected = profile_node_at(profile, ordinal);
        if (!charge(build,
                1U,
                SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
                ordinal,
                node->source.offset)) {
            return false;
        }
        if (!profile_node_matches(node, &expected)) {
            return reject(build,
                SERIALIZED_FILE_VALUES_UNSUPPORTED_SCHEMA,
                SERIALIZED_FILE_VALUES_LIMIT_NONE,
                SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
                ordinal,
                node->source.offset);
        }
        if (!name_matches(build,
                &node->type_name,
                expected.type_name,
                expected.type_length,
                SERIALIZED_FILE_VALUES_FIELD_TYPE_NAME,
                ordinal,
                node->source.offset + TYPE_NAME_OFFSET) ||
            !name_matches(build,
                &node->field_name,
                expected.field_name,
                expected.field_length,
                SERIALIZED_FILE_VALUES_FIELD_FIELD_NAME,
                ordinal,
                node->source.offset + FIELD_NAME_OFFSET)) {
            return false;
        }
    }
    return true;
}

static bool admit_payload(ValuesBuild* build) {
    const SerializedFilePrefixRange payload = build->view.object.payload;
    if (payload.offset % ALIGNMENT != 0U) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_OBJECT_ALIGNMENT,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            payload.offset);
    }
    SerializedFileValuesLimit exceeded = SERIALIZED_FILE_VALUES_LIMIT_NONE;
    if (payload.size > build->limits->max_payload_bytes) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_PAYLOAD_BYTES;
    } else if (build->profile->value_count > build->limits->max_values) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_VALUES;
    } else if (build->limits->max_depth < build->profile->maximum_depth) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_DEPTH;
    }
    if (exceeded != SERIALIZED_FILE_VALUES_LIMIT_NONE) {
        return reject(build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            exceeded,
            SERIALIZED_FILE_VALUES_FIELD_OBJECT,
            SIZE_MAX,
            payload.offset);
    }
    return true;
}

static bool take_span(ValuesBuild* build,
    ValueCursor* cursor,
    size_t width,
    SerializedFileValuesField field,
    size_t node,
    SerializedFilePrefixSpan* out_span) {
    if (width > cursor->end - cursor->position) {
        return reject(build,
            SERIALIZED_FILE_VALUES_TRUNCATED_OBJECT,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            field,
            node,
            cursor->end);
    }
    if (!charge(build, (uint64_t)width + 1U, field, node, cursor->position)) {
        return false;
    }
    *out_span = (SerializedFilePrefixSpan){
        build->bytes + (size_t)cursor->position, cursor->position, width};
    cursor->position += width;
    return true;
}

static SerializedFileValue initial_value(ValuesBuild* build, size_t ordinal) {
    const ValueRole role = profile_role_at(build->profile, ordinal);
    const SerializedFileValue value = {.ordinal = ordinal,
        .kind = role.kind,
        .schema_node = *serialized_file_schema_node(build->schema, role.schema_ordinal),
        .parent = role.parent,
        .first_child = role.first_child,
        .next_sibling = role.next_sibling,
        .child_count = role.child_count,
        .subtree_end = role.subtree_end,
        .array_schema_ordinal = SIZE_MAX,
        .size_schema_ordinal = SIZE_MAX,
        .data_schema_ordinal = SIZE_MAX,
        .array_schema_source = {NULL, UINT64_MAX, 0U},
        .size_schema_source = {NULL, UINT64_MAX, 0U},
        .data_schema_source = {NULL, UINT64_MAX, 0U},
        .length_source = {NULL, UINT64_MAX, 0U},
        .bytes_source = {NULL, UINT64_MAX, 0U},
        .padding_source = {NULL, UINT64_MAX, 0U},
        .integer_source = {NULL, UINT64_MAX, 0U}};
    return value;
}

static bool read_padding(ValuesBuild* build,
    ValueCursor* cursor,
    size_t schema_ordinal,
    SerializedFilePrefixSpan* out_span) {
    const size_t padding = (size_t)((ALIGNMENT - cursor->position % ALIGNMENT) % ALIGNMENT);
    if (padding > build->limits->max_padding_bytes - cursor->padding_bytes) {
        return reject(build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_VALUES_LIMIT_PADDING_BYTES,
            SERIALIZED_FILE_VALUES_FIELD_PADDING,
            schema_ordinal,
            cursor->position);
    }
    cursor->padding_bytes += padding;
    return take_span(
        build, cursor, padding, SERIALIZED_FILE_VALUES_FIELD_PADDING, schema_ordinal, out_span);
}

static bool complete_value(ValuesBuild* build,
    const ValueCursor* cursor,
    uint64_t start,
    SerializedFileValue* value,
    SerializedFileValue* out_value) {
    value->source = (SerializedFilePrefixSpan){
        build->bytes + (size_t)start, start, (size_t)(cursor->position - start)};
    if (!charge(build,
            1U,
            SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE,
            value->schema_node.ordinal,
            start)) {
        return false;
    }
    if (out_value) {
        *out_value = *value;
    }
    return true;
}

static bool read_string(
    ValuesBuild* build, ValueCursor* cursor, size_t ordinal, SerializedFileValue* out_value) {
    SerializedFileValue value = initial_value(build, ordinal);
    const size_t schema_ordinal = value.schema_node.ordinal;
    value.array_schema_ordinal = schema_ordinal + 1U;
    value.size_schema_ordinal = schema_ordinal + 2U;
    value.data_schema_ordinal = schema_ordinal + 3U;
    value.array_schema_source =
        serialized_file_schema_node(build->schema, value.array_schema_ordinal)->source;
    value.size_schema_source =
        serialized_file_schema_node(build->schema, value.size_schema_ordinal)->source;
    value.data_schema_source =
        serialized_file_schema_node(build->schema, value.data_schema_ordinal)->source;
    const uint64_t start = cursor->position;
    if (!take_span(build,
            cursor,
            LENGTH_BYTES,
            SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH,
            value.size_schema_ordinal,
            &value.length_source)) {
        return false;
    }
    value.length_bits = serialized_metadata_decode_u32(value.length_source.data, 0U);
    if (value.length_bits > INT32_MAX) {
        return reject(build,
            SERIALIZED_FILE_VALUES_UNSUPPORTED_STRING_LENGTH,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH,
            value.size_schema_ordinal,
            value.length_source.offset);
    }
    SerializedFileValuesLimit exceeded = SERIALIZED_FILE_VALUES_LIMIT_NONE;
    if (value.length_bits > build->limits->max_string_bytes) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_STRING_BYTES;
    } else if (value.length_bits > build->limits->max_total_string_bytes - cursor->string_bytes) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_TOTAL_STRING_BYTES;
    }
    if (exceeded != SERIALIZED_FILE_VALUES_LIMIT_NONE) {
        return reject(build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            exceeded,
            SERIALIZED_FILE_VALUES_FIELD_STRING_LENGTH,
            value.size_schema_ordinal,
            value.length_source.offset);
    }
    cursor->string_bytes += value.length_bits;
    if (!take_span(build,
            cursor,
            value.length_bits,
            SERIALIZED_FILE_VALUES_FIELD_STRING_BYTES,
            value.data_schema_ordinal,
            &value.bytes_source)) {
        return false;
    }
    if (!read_padding(build, cursor, schema_ordinal, &value.padding_source)) {
        return false;
    }
    return complete_value(build, cursor, start, &value, out_value);
}

static bool read_integer(
    ValuesBuild* build, ValueCursor* cursor, size_t ordinal, SerializedFileValue* out_value) {
    SerializedFileValue value = initial_value(build, ordinal);
    const size_t schema_ordinal = value.schema_node.ordinal;
    const size_t width = value.schema_node.byte_size_bits;
    const uint64_t start = cursor->position;
    SerializedFileValuesLimit exceeded = SERIALIZED_FILE_VALUES_LIMIT_NONE;
    if (width > build->limits->max_integer_bytes) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_INTEGER_BYTES;
    } else if (width > build->limits->max_total_integer_bytes - cursor->integer_bytes) {
        exceeded = SERIALIZED_FILE_VALUES_LIMIT_TOTAL_INTEGER_BYTES;
    }
    if (exceeded != SERIALIZED_FILE_VALUES_LIMIT_NONE) {
        return reject(build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            exceeded,
            SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES,
            schema_ordinal,
            start);
    }
    cursor->integer_bytes += width;
    if (!take_span(build,
            cursor,
            width,
            SERIALIZED_FILE_VALUES_FIELD_INTEGER_BYTES,
            schema_ordinal,
            &value.integer_source)) {
        return false;
    }
    /* Full profile qualification proves these three widths. Keep original
     * bits even for signed fields; no signed conversion or host alignment. */
    if (width == 1U) {
        value.integer_bits = value.integer_source.data[0];
    } else if (width == 4U) {
        value.integer_bits = serialized_metadata_decode_u32(value.integer_source.data, 0U);
    } else {
        value.integer_bits = serialized_metadata_decode_u64(value.integer_source.data, 0U);
    }
    if ((value.schema_node.meta_flags & ORDINARY_ALIGNMENT_FLAG) != 0U &&
        !read_padding(build, cursor, schema_ordinal, &value.padding_source)) {
        return false;
    }
    return complete_value(build, cursor, start, &value, out_value);
}

static bool read_pptr(
    ValuesBuild* build, ValueCursor* cursor, size_t ordinal, ValuesStorage* storage) {
    SerializedFileValue value = initial_value(build, ordinal);
    const uint64_t start = cursor->position;
    /* The admitted PPtr is an ordinary fixed pair, not the converter's
     * separate size8 route. Children consume packed int4 and SInt64 bytes8. */
    for (size_t child = value.first_child; child < value.subtree_end; ++child) {
        if (!read_integer(build, cursor, child, storage ? &storage->values[child] : NULL)) {
            return false;
        }
    }
    return complete_value(build, cursor, start, &value, storage ? &storage->values[ordinal] : NULL);
}

static bool read_profile_values(ValuesBuild* build, ValueCursor* cursor, ValuesStorage* storage) {
    if (build->profile == &text_asset_profile) {
        for (size_t ordinal = 1U; ordinal < TEXT_ASSET_VALUES; ++ordinal) {
            if (!read_string(build, cursor, ordinal, storage ? &storage->values[ordinal] : NULL)) {
                return false;
            }
        }
        return true;
    }
    if (!read_pptr(build, cursor, 1U, storage) ||
        !read_integer(build, cursor, 4U, storage ? &storage->values[4] : NULL) ||
        !read_pptr(build, cursor, 5U, storage) ||
        !read_string(build, cursor, 8U, storage ? &storage->values[8] : NULL)) {
        return false;
    }
    if (build->profile == &mono_behaviour_text_asset_profile) {
        return read_pptr(build, cursor, MONO_BEHAVIOUR_FIELD_VALUE, storage);
    }
    return read_integer(build,
        cursor,
        MONO_BEHAVIOUR_FIELD_VALUE,
        storage ? &storage->values[MONO_BEHAVIOUR_FIELD_VALUE] : NULL);
}

static bool parse_payload(ValuesBuild* build, ValuesStorage* storage) {
    const SerializedFilePrefixRange payload = build->view.object.payload;
    ValueCursor cursor = {.position = payload.offset, .end = payload.offset + payload.size};
    if (!read_profile_values(build, &cursor, storage)) {
        return false;
    }
    if (cursor.position != cursor.end) {
        return reject(build,
            SERIALIZED_FILE_VALUES_TRAILING_OBJECT_BYTES,
            SERIALIZED_FILE_VALUES_LIMIT_NONE,
            SERIALIZED_FILE_VALUES_FIELD_OBJECT_END,
            0U,
            cursor.position);
    }
    if (!charge(build, 1U, SERIALIZED_FILE_VALUES_FIELD_SCHEMA_NODE, 0U, payload.offset)) {
        return false;
    }
    build->view.value_count = build->profile->value_count;
    build->view.maximum_depth = build->profile->maximum_depth;
    build->view.consumed_bytes = payload.size;
    build->view.string_bytes = cursor.string_bytes;
    build->view.padding_bytes = cursor.padding_bytes;
    build->view.integer_bytes = cursor.integer_bytes;
    if (storage) {
        storage->values[0] = initial_value(build, 0U);
        storage->values[0].source = (SerializedFilePrefixSpan){
            build->bytes + (size_t)payload.offset, payload.offset, (size_t)payload.size};
    }
    return true;
}

static SerializedFileValuesResult create_values(const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* out_values,
    bool allow_ordinary) {
    ValuesBuild build = {
        .limits = limits, .result = initial_result(), .schema = schema, .bytes = mapped_prefix};
    if (!admit_arguments(&build, directory, schema, mapped_prefix, mapped_size, out_values) ||
        !charge(&build, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX) ||
        !admit_source(&build, directory, object_ordinal, mapped_size) || !qualify_context(&build) ||
        !qualify_profile(&build, allow_ordinary) || !admit_payload(&build) ||
        !parse_payload(&build, NULL) ||
        !charge(&build, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return build.result;
    }
    const size_t retained_bytes =
        sizeof(ValuesStorage) + build.profile->value_count * sizeof(SerializedFileValue);
    build.result.required_retained_bytes = retained_bytes;
    if (retained_bytes > limits->max_retained_bytes) {
        reject_owner(&build,
            SERIALIZED_FILE_VALUES_LIMIT_EXCEEDED,
            SERIALIZED_FILE_VALUES_LIMIT_RETAINED_BYTES);
        return build.result;
    }
    if (!charge(&build, retained_bytes, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        return build.result;
    }
    ValuesStorage* storage = mem_alloc(retained_bytes);
    if (!storage) {
        reject_owner(
            &build, SERIALIZED_FILE_VALUES_ALLOCATION_FAILED, SERIALIZED_FILE_VALUES_LIMIT_NONE);
        return build.result;
    }
    build.result.peak_retained_bytes = retained_bytes;
    memset(storage, 0, retained_bytes);
    if (!parse_payload(&build, storage) ||
        !charge(&build, 1U, SERIALIZED_FILE_VALUES_FIELD_NONE, SIZE_MAX, UINT64_MAX)) {
        mem_free(storage, retained_bytes);
        return build.result;
    }
    build.view.retained_bytes = retained_bytes;
    storage->view = build.view;
    out_values->implementation = storage;
    return build.result;
}

SerializedFileValuesResult serialized_file_values_create_text_asset(
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* out_values) {
    return create_values(
        directory, schema, object_ordinal, mapped_prefix, mapped_size, limits, out_values, false);
}

SerializedFileValuesResult serialized_file_values_create_ordinary(
    const SerializedFileDirectory* directory,
    const SerializedFileSchema* schema,
    size_t object_ordinal,
    const uint8_t* mapped_prefix,
    size_t mapped_size,
    const SerializedFileValuesLimits* limits,
    SerializedFileValues* out_values) {
    return create_values(
        directory, schema, object_ordinal, mapped_prefix, mapped_size, limits, out_values, true);
}

void serialized_file_values_init(SerializedFileValues* values) {
    if (values) {
        values->implementation = NULL;
    }
}

void serialized_file_values_dispose(SerializedFileValues* values) {
    if (values && values->implementation) {
        ValuesStorage* storage = values->implementation;
        mem_free(storage, storage->view.retained_bytes);
        values->implementation = NULL;
    }
}

const SerializedFileValuesView* serialized_file_values_view(const SerializedFileValues* values) {
    const ValuesStorage* storage = values ? values->implementation : NULL;
    return storage ? &storage->view : NULL;
}

const SerializedFileValue* serialized_file_values_value(
    const SerializedFileValues* values, size_t value_ordinal) {
    const ValuesStorage* storage = values ? values->implementation : NULL;
    return storage && value_ordinal < storage->view.value_count ? &storage->values[value_ordinal]
                                                                : NULL;
}
