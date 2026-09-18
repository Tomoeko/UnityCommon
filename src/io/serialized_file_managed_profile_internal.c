// SPDX-License-Identifier: GPL-3.0-only

#include "serialized_file_managed_profile_internal.h"

#include <limits.h>
#include <string.h>

enum {
    MANAGED_HOST_CLASS = 114,
    REGISTRY_NODE_COUNT = 21,
    HOST_REGISTRY_NODE = 25,
    TEXT_REGISTRY_NODE = 8,
    FLOAT_REGISTRY_NODE = 5,
    TYPE_NAME_OFFSET = 4,
    FIELD_NAME_OFFSET = 8,
    TYPE_HASH_BYTES = 16
};

typedef struct ManagedNodeProfile {
    const char* type_name;
    const char* field_name;
    size_t type_length;
    size_t field_length;
    uint8_t level;
    uint8_t flags;
    uint32_t byte_size;
    uint32_t meta_flags;
} ManagedNodeProfile;

/* Complete exact35 writer profiles. NULL names are authored identifiers whose
 * genuine descriptors are retained without lexical admission rules. The shared
 * registry suffix is qualified even when selected traversal omits its bytes. */
static const ManagedNodeProfile managed_host_prefix[] = {
    {"MonoBehaviour", "Base", 13U, 4U, 0U, 0U, UINT32_MAX, 0x8000U},
    {"PPtr<GameObject>", "m_GameObject", 16U, 12U, 1U, 0U, 12U, 0x41U},
    {"int", "m_FileID", 3U, 8U, 2U, 0U, 4U, 0x41U},
    {"SInt64", "m_PathID", 6U, 8U, 2U, 0U, 8U, 0x41U},
    {"UInt8", "m_Enabled", 5U, 9U, 1U, 0U, 1U, 0x4101U},
    {"PPtr<MonoScript>", "m_Script", 16U, 8U, 1U, 0U, 12U, 0x0U},
    {"int", "m_FileID", 3U, 8U, 2U, 0U, 4U, 0x800001U},
    {"SInt64", "m_PathID", 6U, 8U, 2U, 0U, 8U, 0x800001U},
    {"string", "m_Name", 6U, 6U, 1U, 0U, UINT32_MAX, 0x88001U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x84001U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x80001U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x80001U},
    {"string", NULL, 6U, 0U, 1U, 0U, UINT32_MAX, 0x8000U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x4001U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x1U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x1U},
    {"managedReference", NULL, 16U, 0U, 1U, 2U, 8U, 0x0U},
    {"SInt64", "rid", 6U, 3U, 2U, 0U, 8U, 0x0U},
    {"managedReference", NULL, 16U, 0U, 1U, 2U, 8U, 0x0U},
    {"SInt64", "rid", 6U, 3U, 2U, 0U, 8U, 0x0U},
    {"Object", NULL, 6U, 0U, 1U, 8U, UINT32_MAX, 0x0U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x0U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x0U},
    {"managedRefArrayItem", "data", 19U, 4U, 3U, 2U, 8U, 0x0U},
    {"SInt64", "rid", 6U, 3U, 4U, 0U, 8U, 0x0U},
};

static const ManagedNodeProfile managed_text_prefix[] = {
    {NULL, "Base", 0U, 4U, 0U, 0U, UINT32_MAX, 0x8000U},
    {"int", NULL, 3U, 0U, 1U, 0U, 4U, 0x0U},
    {"string", NULL, 6U, 0U, 1U, 0U, UINT32_MAX, 0x8000U},
    {"Array", "Array", 5U, 5U, 2U, 1U, UINT32_MAX, 0x4001U},
    {"int", "size", 3U, 4U, 3U, 0U, 4U, 0x1U},
    {"char", "data", 4U, 4U, 3U, 0U, 1U, 0x1U},
    {"managedReference", NULL, 16U, 0U, 1U, 2U, 8U, 0x0U},
    {"SInt64", "rid", 6U, 3U, 2U, 0U, 8U, 0x0U},
};

static const ManagedNodeProfile managed_float_prefix[] = {
    {NULL, "Base", 0U, 4U, 0U, 0U, UINT32_MAX, 0x8000U},
    {"int", NULL, 3U, 0U, 1U, 0U, 4U, 0x0U},
    {"float", NULL, 5U, 0U, 1U, 0U, 4U, 0x0U},
    {"managedReference", NULL, 16U, 0U, 1U, 2U, 8U, 0x0U},
    {"SInt64", "rid", 6U, 3U, 2U, 0U, 8U, 0x0U},
};

static const ManagedNodeProfile managed_registry_suffix[] = {
    {"ManagedReferencesRegistry", "references", 25U, 10U, 1U, 4U, UINT32_MAX, 0x8001U},
    {"int", "version", 3U, 7U, 2U, 0U, 4U, 0x1U},
    {"vector", "RefIds", 6U, 6U, 2U, 0U, UINT32_MAX, 0x8001U},
    {"Array", "Array", 5U, 5U, 3U, 1U, UINT32_MAX, 0xc001U},
    {"int", "size", 3U, 4U, 4U, 0U, 4U, 0x1U},
    {"ReferencedObject", "data", 16U, 4U, 4U, 0U, UINT32_MAX, 0x8001U},
    {"SInt64", "rid", 6U, 3U, 5U, 0U, 8U, 0x1U},
    {"ReferencedManagedType", "type", 21U, 4U, 5U, 0U, UINT32_MAX, 0x208001U},
    {"string", "class", 6U, 5U, 6U, 0U, UINT32_MAX, 0x208001U},
    {"Array", "Array", 5U, 5U, 7U, 1U, UINT32_MAX, 0x204001U},
    {"int", "size", 3U, 4U, 8U, 0U, 4U, 0x200001U},
    {"char", "data", 4U, 4U, 8U, 0U, 1U, 0x200001U},
    {"string", "ns", 6U, 2U, 6U, 0U, UINT32_MAX, 0x208001U},
    {"Array", "Array", 5U, 5U, 7U, 1U, UINT32_MAX, 0x204001U},
    {"int", "size", 3U, 4U, 8U, 0U, 4U, 0x200001U},
    {"char", "data", 4U, 4U, 8U, 0U, 1U, 0x200001U},
    {"string", "asm", 6U, 3U, 6U, 0U, UINT32_MAX, 0x208001U},
    {"Array", "Array", 5U, 5U, 7U, 1U, UINT32_MAX, 0x204001U},
    {"int", "size", 3U, 4U, 8U, 0U, 4U, 0x200001U},
    {"char", "data", 4U, 4U, 8U, 0U, 1U, 0x200001U},
    {"ReferencedObjectData", "data", 20U, 4U, 5U, 0U, 0U, 0x1U},
};

static const ManagedProfile managed_host_profile = {MANAGED_PROFILE_HOST, HOST_REGISTRY_NODE};
static const ManagedProfile managed_text_profile = {MANAGED_PROFILE_TEXT, TEXT_REGISTRY_NODE};
static const ManagedProfile managed_float_profile = {MANAGED_PROFILE_FLOAT, FLOAT_REGISTRY_NODE};

_Static_assert(sizeof(managed_host_prefix) / sizeof(managed_host_prefix[0]) == HOST_REGISTRY_NODE,
    "host prefix must reach its original registry node");
_Static_assert(sizeof(managed_text_prefix) / sizeof(managed_text_prefix[0]) == TEXT_REGISTRY_NODE,
    "text payload prefix must reach its original registry node");
_Static_assert(
    sizeof(managed_float_prefix) / sizeof(managed_float_prefix[0]) == FLOAT_REGISTRY_NODE,
    "float payload prefix must reach its original registry node");
_Static_assert(
    sizeof(managed_registry_suffix) / sizeof(managed_registry_suffix[0]) == REGISTRY_NODE_COUNT,
    "all complete profiles must retain the registry suffix");

typedef struct ProfileQualification {
    ManagedProfileResult result;
    uint64_t max_work;
} ProfileQualification;

static bool reject_profile(ProfileQualification* qualification,
    SerializedFileManagedValuesStatus status,
    SerializedFileManagedValuesField field,
    size_t node_ordinal,
    uint64_t offset) {
    qualification->result.status = status;
    qualification->result.field = field;
    qualification->result.node_ordinal = node_ordinal;
    qualification->result.error_offset = offset;
    return false;
}

static bool charge_profile(ProfileQualification* qualification,
    SerializedFileManagedValuesField field,
    size_t node_ordinal,
    uint64_t offset) {
    if (qualification->result.work_used == qualification->max_work) {
        return reject_profile(qualification,
            SERIALIZED_FILE_MANAGED_VALUES_LIMIT_EXCEEDED,
            field,
            node_ordinal,
            offset);
    }
    ++qualification->result.work_used;
    return true;
}

static const ManagedProfile* select_profile(ProfileQualification* qualification,
    const SerializedFileSchemaView* view,
    SerializedFileSchemaContextKind context_kind) {
    const bool ordinary = context_kind == SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT;
    const SerializedFileSchemaRowKind expected_kind =
        ordinary ? SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE : SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE;
    const uint32_t expected_class = ordinary ? MANAGED_HOST_CLASS : UINT32_MAX;
    if (view->row_kind != expected_kind || view->class_id_bits != expected_class ||
        view->stripped_raw != 0U || view->script_index_bits > INT16_MAX || !view->has_script_hash ||
        view->script_hash_source.size != TYPE_HASH_BYTES ||
        view->type_hash_source.size != TYPE_HASH_BYTES) {
        reject_profile(qualification,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_TYPE,
            SIZE_MAX,
            view->type_entry_source.offset);
        return NULL;
    }
    if (ordinary && view->node_count == HOST_REGISTRY_NODE + REGISTRY_NODE_COUNT) {
        return &managed_host_profile;
    }
    if (!ordinary && view->node_count == TEXT_REGISTRY_NODE + REGISTRY_NODE_COUNT) {
        return &managed_text_profile;
    }
    if (!ordinary && view->node_count == FLOAT_REGISTRY_NODE + REGISTRY_NODE_COUNT) {
        return &managed_float_profile;
    }
    reject_profile(qualification,
        SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
        SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
        SIZE_MAX,
        view->tree.node_count_source.offset);
    return NULL;
}

static const ManagedNodeProfile* profile_prefix(ManagedProfileKind kind) {
    switch (kind) {
    case MANAGED_PROFILE_HOST:
        return managed_host_prefix;
    case MANAGED_PROFILE_TEXT:
        return managed_text_prefix;
    case MANAGED_PROFILE_FLOAT:
        return managed_float_prefix;
    }
    return NULL;
}

static bool qualify_name(ProfileQualification* qualification,
    const SerializedFileSchemaString* actual,
    const char* expected,
    size_t expected_length,
    SerializedFileManagedValuesField field,
    size_t node_ordinal,
    uint64_t offset) {
    if (!charge_profile(qualification, field, node_ordinal, offset)) {
        return false;
    }
    if (!expected) {
        return true;
    }
    if (actual->byte_count != expected_length) {
        return reject_profile(qualification,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
            field,
            node_ordinal,
            offset);
    }
    for (size_t index = 0U; index < expected_length; ++index) {
        if (!charge_profile(qualification, field, node_ordinal, offset)) {
            return false;
        }
        if (actual->bytes[index] != (uint8_t)expected[index]) {
            return reject_profile(qualification,
                SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
                field,
                node_ordinal,
                offset);
        }
    }
    return true;
}

static bool qualify_node(ProfileQualification* qualification,
    const SerializedFileSchemaNode* node,
    const ManagedNodeProfile* expected) {
    if (!charge_profile(qualification,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            node->ordinal,
            node->source.offset)) {
        return false;
    }
    static const uint8_t empty_tail[8] = {0U};
    if (node->version != 1U || node->level != expected->level ||
        node->type_flags != expected->flags || node->byte_size_bits != expected->byte_size ||
        node->index_bits != node->ordinal || node->meta_flags != expected->meta_flags ||
        memcmp(node->opaque_tail, empty_tail, sizeof(empty_tail)) != 0) {
        return reject_profile(qualification,
            SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_SCHEMA,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_SCHEMA_NODE,
            node->ordinal,
            node->source.offset);
    }
    /* Name failures cite their original offset words. A common-string offset
   * is not a file coordinate, even when that string supplied the comparison. */
    return qualify_name(qualification,
               &node->type_name,
               expected->type_name,
               expected->type_length,
               SERIALIZED_FILE_MANAGED_VALUES_FIELD_TYPE_NAME,
               node->ordinal,
               node->source.offset + TYPE_NAME_OFFSET) &&
        qualify_name(qualification,
            &node->field_name,
            expected->field_name,
            expected->field_length,
            SERIALIZED_FILE_MANAGED_VALUES_FIELD_FIELD_NAME,
            node->ordinal,
            node->source.offset + FIELD_NAME_OFFSET);
}

ManagedProfileResult serialized_file_managed_profile_qualify(const SerializedFileSchema* schema,
    SerializedFileSchemaContextKind context_kind,
    uint64_t max_work,
    const ManagedProfile** out_profile) {
    ProfileQualification qualification = {
        .result = {.status = SERIALIZED_FILE_MANAGED_VALUES_INVALID_ARGUMENT,
            .field = SERIALIZED_FILE_MANAGED_VALUES_FIELD_NONE,
            .type_ordinal = SIZE_MAX,
            .node_ordinal = SIZE_MAX,
            .error_offset = UINT64_MAX},
        .max_work = max_work};
    if (!schema || !out_profile ||
        (context_kind != SERIALIZED_FILE_SCHEMA_CONTEXT_ORDINARY_OBJECT &&
            context_kind != SERIALIZED_FILE_SCHEMA_CONTEXT_SELECTED_REFERENCE_PAYLOAD)) {
        return qualification.result;
    }
    const SerializedFileSchemaView* view = serialized_file_schema_view(schema);
    if (!view) {
        qualification.result.status = SERIALIZED_FILE_MANAGED_VALUES_INVALID_STATE;
        return qualification.result;
    }
    qualification.result.type_ordinal = view->type_ordinal;
    if (view->engine_version != SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1) {
        qualification.result.status = SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENGINE;
        return qualification.result;
    }
    if (view->prefix.header.endian_selector != 0U) {
        qualification.result.status = SERIALIZED_FILE_MANAGED_VALUES_UNSUPPORTED_ENDIAN;
        return qualification.result;
    }
    const ManagedProfile* profile = select_profile(&qualification, view, context_kind);
    if (!profile) {
        return qualification.result;
    }
    const ManagedNodeProfile* prefix = profile_prefix(profile->kind);
    for (size_t ordinal = 0U; ordinal < view->node_count; ++ordinal) {
        const ManagedNodeProfile* expected = ordinal < profile->registry_ordinal
            ? &prefix[ordinal]
            : &managed_registry_suffix[ordinal - profile->registry_ordinal];
        const SerializedFileSchemaNode* node = serialized_file_schema_node(schema, ordinal);
        if (!qualify_node(&qualification, node, expected)) {
            return qualification.result;
        }
    }
    qualification.result.status = SERIALIZED_FILE_MANAGED_VALUES_OK;
    *out_profile = profile;
    return qualification.result;
}
