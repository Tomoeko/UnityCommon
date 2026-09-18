// SPDX-License-Identifier: GPL-3.0-only

#include "io/typetree.h"

#include "serialized_metadata_materialization_internal.h"
#include "typetree_directory_internal.h"
#include "typetree_common_strings_internal.h"

#include <limits.h>

/* Unity 2021.3.35f1's declared common buffer is 1170 bytes, including the
 * literal's final extra NUL. Local file tables may contain additional names.
 * The owner-side common_string_table fixture authenticates the full interval. */
static const char g_common_string_table[] =
    "AABB\0AnimationClip\0AnimationCurve\0AnimationState\0Array\0Base\0BitField\0bitset\0bool\0char\0"
    "ColorRGBA\0Component\0data\0deque\0double\0dynamic_array\0FastPropertyName\0first\0float\0Font\0"
    "GameObject\0Generic Mono\0GradientNEW\0GUID\0GUIStyle\0int\0list\0long long\0map\0Matrix4x4f\0MdFour\0"
    "MonoBehaviour\0MonoScript\0m_ByteSize\0m_Curve\0m_EditorClassIdentifier\0m_EditorHideFlags\0m_Enabled\0"
    "m_ExtensionPtr\0m_GameObject\0m_Index\0m_IsArray\0m_IsStatic\0m_MetaFlag\0m_Name\0m_ObjectHideFlags\0"
    "m_PrefabInternal\0m_PrefabParentObject\0m_Script\0m_StaticEditorFlags\0m_Type\0m_Version\0Object\0"
    "pair\0PPtr<Component>\0PPtr<GameObject>\0PPtr<Material>\0PPtr<MonoBehaviour>\0PPtr<MonoScript>\0"
    "PPtr<Object>\0PPtr<Prefab>\0PPtr<Sprite>\0PPtr<TextAsset>\0PPtr<Texture>\0PPtr<Texture2D>\0PPtr<Transform>\0"
    "Prefab\0Quaternionf\0Rectf\0RectInt\0RectOffset\0second\0set\0short\0size\0SInt16\0SInt32\0SInt64\0"
    "SInt8\0staticvector\0string\0TextAsset\0TextMesh\0Texture\0Texture2D\0Transform\0TypelessData\0UInt16\0"
    "UInt32\0UInt64\0UInt8\0unsigned int\0unsigned long long\0unsigned short\0vector\0Vector2f\0Vector3f\0"
    "Vector4f\0m_ScriptingClassIdentifier\0Gradient\0Type*\0int2_storage\0int3_storage\0BoundsInt\0m_CorrespondingSourceObject\0"
    "m_PrefabInstance\0m_PrefabAsset\0FileSize\0Hash128\0";

static const size_t g_common_string_table_size = sizeof(g_common_string_table);

const uint8_t* typetree_common_string_table(size_t* out_size) {
    *out_size = g_common_string_table_size;
    return (const uint8_t*)g_common_string_table;
}

static int scalar_wire_size(const char* type_name) {
    if (!type_name) return 0;
    if (strcmp(type_name, "SInt8") == 0 ||
        strcmp(type_name, "UInt8") == 0 ||
        strcmp(type_name, "bool") == 0 ||
        strcmp(type_name, "char") == 0) return 1;
    if (strcmp(type_name, "SInt16") == 0 ||
        strcmp(type_name, "UInt16") == 0 ||
        strcmp(type_name, "short") == 0 ||
        strcmp(type_name, "unsigned short") == 0) return 2;
    if (strcmp(type_name, "SInt32") == 0 ||
        strcmp(type_name, "UInt32") == 0 ||
        strcmp(type_name, "int") == 0 ||
        strcmp(type_name, "unsigned int") == 0 ||
        strcmp(type_name, "RenderingLayerMask") == 0 ||
        strcmp(type_name, "Type*") == 0 ||
        strcmp(type_name, "BitField") == 0 ||
        strcmp(type_name, "float") == 0) return 4;
    if (strcmp(type_name, "SInt64") == 0 ||
        strcmp(type_name, "UInt64") == 0 ||
        strcmp(type_name, "long long") == 0 ||
        strcmp(type_name, "unsigned long long") == 0 ||
        strcmp(type_name, "FileSize") == 0 ||
        strcmp(type_name, "double") == 0) return 8;
    return 0;
}

static bool node_has_children(const TypeTreeType* type, int index) {
    return index + 1 < type->node_count &&
           type->nodes[index + 1].level > type->nodes[index].level;
}

static int subtree_end(const TypeTreeType* type, int index) {
    const uint8_t level = type->nodes[index].level;
    int end = index + 1;
    while (end < type->node_count && type->nodes[end].level > level) end++;
    return end;
}

static bool validate_array_node(const TypeTreeType* type, int array_index) {
    const TypeTreeNode* array = &type->nodes[array_index];
    if (strcmp(array->type_str, "Array") != 0 ||
        strcmp(array->name_str, "Array") != 0 || array->byte_size != -1 ||
        array_index == 0 ||
        type->nodes[array_index - 1].level + 1U != array->level ||
        array_index + 2 >= type->node_count) return false;

    const TypeTreeNode* size = &type->nodes[array_index + 1];
    const TypeTreeNode* data = &type->nodes[array_index + 2];
    if (size->level != array->level + 1U ||
        data->level != array->level + 1U ||
        strcmp(size->name_str, "size") != 0 ||
        strcmp(size->type_str, "int") != 0 || size->byte_size != 4 ||
        node_has_children(type, array_index + 1) ||
        strcmp(data->name_str, "data") != 0) return false;

    /* size and data are the complete set of direct Array children. */
    if (subtree_end(type, array_index + 2) !=
        subtree_end(type, array_index)) return false;

    const TypeTreeNode* owner = &type->nodes[array_index - 1];
    if (strcmp(owner->type_str, "string") == 0 &&
        (strcmp(data->type_str, "char") != 0 || data->byte_size != 1 ||
         node_has_children(type, array_index + 2))) return false;
    return true;
}

bool typetree_validate_schema(const TypeTreeType* type) {
    if (!type || type->node_count <= 0 || !type->nodes) return false;

    for (int i = 0; i < type->node_count; ++i) {
        const TypeTreeNode* node = &type->nodes[i];
        if (!node->type_str || !node->name_str ||
            node->type_str[0] == '\0' || node->name_str[0] == '\0' ||
            node->byte_size < -1 ||
            (i == 0 && node->level != 0U) ||
            (i > 0 && (node->level == 0U ||
                       node->level > type->nodes[i - 1].level + 1U))) {
            return false;
        }

        const bool has_children = node_has_children(type, i);
        const int wire_size = scalar_wire_size(node->type_str);
        if (wire_size != 0 &&
            (has_children || node->byte_size != wire_size)) return false;

        const bool is_array_marker =
            strcmp(node->type_str, "Array") == 0 ||
            strcmp(node->name_str, "Array") == 0;
        if (is_array_marker && !validate_array_node(type, i)) return false;

        if (strcmp(node->type_str, "string") == 0) {
            if (node->byte_size != -1 || !has_children ||
                i + 1 >= type->node_count ||
                !validate_array_node(type, i + 1)) return false;
        } else if (has_children) {
            const TypeTreeNode* first_child = &type->nodes[i + 1];
            if ((strcmp(first_child->type_str, "Array") == 0 ||
                 strcmp(first_child->name_str, "Array") == 0) &&
                !validate_array_node(type, i + 1)) return false;
        } else if (wire_size == 0 &&
                   strcmp(node->type_str, "Array") != 0) {
            /* The parser has no wire rule for an opaque leaf. */
            return false;
        }
    }
    return true;
}

static const char* resolve_string_in_table(const uint8_t* table, size_t size,
                                           uint32_t offset) {
    if (!table || offset >= size ||
        (offset > 0 && table[offset - 1] != '\0') ||
        !memchr(table + offset, '\0', size - offset)) {
        return NULL;
    }
    return (const char*)table + offset;
}

const char* typetree_resolve_string(const uint8_t* local_table, uint32_t local_size, uint32_t offset) {
    if ((offset & 0x80000000) != 0) {
        uint32_t real_offset = offset & ~0x80000000;
        return resolve_string_in_table(
            (const uint8_t*)g_common_string_table,
            g_common_string_table_size, real_offset);
    } else {
        return resolve_string_in_table(local_table, local_size, offset);
    }
}

bool typetree_bind_node_strings(TypeTreeType* type) {
    if (!type || type->node_count <= 0 || !type->nodes) return false;

    /* Validate every offset before mutating any borrowed pointer. */
    for (int i = 0; i < type->node_count; ++i) {
        const TypeTreeNode* node = &type->nodes[i];
        if (!typetree_resolve_string(type->string_buffer,
                                     type->string_buffer_size,
                                     node->type_str_offset) ||
            !typetree_resolve_string(type->string_buffer,
                                     type->string_buffer_size,
                                     node->name_str_offset)) {
            return false;
        }
    }

    for (int i = 0; i < type->node_count; ++i) {
        TypeTreeNode* node = &type->nodes[i];
        node->type_str = typetree_resolve_string(type->string_buffer,
                                                 type->string_buffer_size,
                                                 node->type_str_offset);
        node->name_str = typetree_resolve_string(type->string_buffer,
                                                 type->string_buffer_size,
                                                 node->name_str_offset);
    }
    return true;
}

static bool read_tree_nodes(
    TypeTreeType* type, ByteStream* stream, uint32_t version, uint32_t node_count) {
    const size_t node_record_size = version >= 18U ? 32U : 24U;
    if (!node_count || node_count > INT_MAX ||
        node_count > stream_remaining(stream) / node_record_size ||
        dxbc_size_multiply_overflows((size_t)node_count, sizeof(*type->nodes))) {
        return false;
    }
    type->node_count = (int)node_count;
    const size_t allocation_size = (size_t)node_count * sizeof(*type->nodes);
    type->nodes = mem_alloc(allocation_size);
    if (!type->nodes) {
        return false;
    }
    memset(type->nodes, 0, allocation_size);

    for (uint32_t index = 0U; index < node_count; ++index) {
        TypeTreeNode* node = &type->nodes[index];
        if (!stream_read_uint16(stream, &node->version) ||
            !stream_read_uint8(stream, &node->level) ||
            !stream_read_uint8(stream, &node->type_flags) ||
            !stream_read_uint32(stream, &node->type_str_offset) ||
            !stream_read_uint32(stream, &node->name_str_offset) ||
            !stream_read_int32(stream, &node->byte_size) ||
            !stream_read_uint32(stream, &node->index) ||
            !stream_read_uint32(stream, &node->meta_flags)) {
            return false;
        }
        if (version >= 18U) {
            /* Unity's BE node swap leaves these bytes untouched. Preserve the
             * raw sequence in the legacy field using a fixed LE bit container. */
            uint8_t opaque[8];
            if (!stream_read_bytes(stream, opaque, sizeof(opaque))) {
                return false;
            }
            for (size_t byte = 0U; byte < sizeof(opaque); ++byte) {
                node->ref_type_hash |= (uint64_t)opaque[byte] << (byte * 8U);
            }
        }
        if ((!index && node->level != 0U) ||
            (index && node->level > type->nodes[index - 1U].level + 1U)) {
            return false;
        }
    }
    return true;
}

static bool read_tree_payload(TypeTreeType* type,
    ByteStream* stream,
    uint32_t version,
    uint32_t node_count,
    uint32_t string_buffer_size) {
    if (!read_tree_nodes(type, stream, version, node_count)) {
        return false;
    }
    type->string_buffer_size = string_buffer_size;
    if (string_buffer_size) {
        if (string_buffer_size > stream_remaining(stream)) {
            return false;
        }
        type->string_buffer = mem_alloc(string_buffer_size);
        if (!type->string_buffer ||
            !stream_read_bytes(stream, type->string_buffer, string_buffer_size)) {
            return false;
        }
    }
    return typetree_bind_node_strings(type);
}

static bool read_dependencies(TypeTreeType* type, ByteStream* stream, uint32_t count) {
    if (count > INT_MAX || count > stream_remaining(stream) / sizeof(*type->dependencies) ||
        dxbc_size_multiply_overflows((size_t)count, sizeof(*type->dependencies))) {
        return false;
    }
    type->dependency_count = (int)count;
    if (!count) {
        return true;
    }
    type->dependencies = mem_alloc((size_t)count * sizeof(*type->dependencies));
    if (!type->dependencies) {
        return false;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        if (!stream_read_int32(stream, &type->dependencies[index])) {
            return false;
        }
    }
    return true;
}

static bool materialize_type_identity(TypeTreeType* type,
    uint32_t class_id_bits,
    uint16_t script_index_bits,
    uint8_t stripped_raw,
    bool has_script_hash,
    SerializedFilePrefixSpan type_hash_source,
    SerializedFilePrefixSpan script_hash_source) {
    if (stripped_raw > 1U) {
        return false;
    }
    type->type_id = serialized_metadata_signed32(class_id_bits);
    type->is_stripped = stripped_raw != 0U;
    type->script_type_index = script_index_bits;
    memcpy(type->type_hash, type_hash_source.data, sizeof(type->type_hash));
    if (has_script_hash) {
        memcpy(type->script_id_hash, script_hash_source.data, sizeof(type->script_id_hash));
    }
    return true;
}

static bool materialize_tree_payload(
    TypeTreeType* type, const SerializedFileDirectoryTree* tree, bool big_endian) {
    if (dxbc_size_add_overflows(tree->nodes_source.size, tree->strings_source.size)) {
        return false;
    }
    ByteStream nodes;
    stream_init(
        &nodes, tree->nodes_source.data, tree->nodes_source.size + tree->strings_source.size);
    stream_set_endian(&nodes, big_endian);
    return read_tree_payload(type, &nodes, 22U, tree->node_count, tree->string_byte_count) &&
        nodes.position == nodes.size;
}

bool typetree_materialize_directory_type(
    TypeTreeType* type, const SerializedFileDirectoryTypeRow* row, bool big_endian) {
    if (!type || !row) {
        return false;
    }
    memset(type, 0, sizeof(*type));
    if (!materialize_type_identity(type,
            row->class_id_bits,
            row->script_index_bits,
            row->stripped_raw,
            row->has_script_hash,
            row->type_hash_source,
            row->script_hash_source)) {
        return false;
    }
    if (!row->has_tree) {
        return true;
    }
    if (!materialize_tree_payload(type, &row->tree, big_endian)) {
        goto fail;
    }
    ByteStream dependencies;
    stream_init(
        &dependencies, row->dependency_words_source.data, row->dependency_words_source.size);
    stream_set_endian(&dependencies, big_endian);
    if (!read_dependencies(type, &dependencies, row->dependency_count) ||
        dependencies.position != dependencies.size || !typetree_validate_schema(type)) {
        goto fail;
    }
    return true;

fail:
    typetree_free_type(type);
    memset(type, 0, sizeof(*type));
    return false;
}

bool typetree_materialize_metadata_tail_reference_type(
    TypeTreeType* type, const SerializedFileMetadataTailReferenceTypeRow* row, bool big_endian) {
    if (!type || !row) {
        return false;
    }
    memset(type, 0, sizeof(*type));
    if (!materialize_type_identity(type,
            row->class_id_bits,
            row->script_index_bits,
            row->stripped_raw,
            row->has_script_hash,
            row->type_hash_source,
            row->script_hash_source)) {
        return false;
    }
    type->is_ref_type = true;
    if (!row->has_tree) {
        return true;
    }
    if (!materialize_tree_payload(type, &row->tree, big_endian)) {
        goto fail;
    }
    type->ref_class_name = serialized_metadata_copy_terminated_string(row->class_name_source);
    if (!type->ref_class_name) {
        goto fail;
    }
    type->ref_namespace = serialized_metadata_copy_terminated_string(row->namespace_source);
    if (!type->ref_namespace) {
        goto fail;
    }
    type->ref_asm_name = serialized_metadata_copy_terminated_string(row->assembly_name_source);
    if (!type->ref_asm_name || !typetree_validate_schema(type)) {
        goto fail;
    }
    return true;

fail:
    typetree_free_type(type);
    memset(type, 0, sizeof(*type));
    return false;
}

bool typetree_read_type(TypeTreeType* type,
    ByteStream* stream,
    uint32_t version,
    bool has_type_tree,
    bool is_ref_type) {
    memset(type, 0, sizeof(TypeTreeType));
    type->is_ref_type = is_ref_type;

    if (!stream_read_int32(stream, &type->type_id)) {
        return false;
    }

    if (version >= 16) {
        uint8_t stripped_byte;
        if (!stream_read_uint8(stream, &stripped_byte) || stripped_byte > 1U) {
            return false;
        }
        type->is_stripped = stripped_byte != 0;
    }

    if (version >= 17) {
        if (!stream_read_uint16(stream, &type->script_type_index)) {
            return false;
        }
    } else {
        type->script_type_index = 0xffff;
    }

    bool has_script_hash = (version < 17 && type->type_id < 0) ||
        (version >= 17 && type->type_id == 114) || // MonoBehaviour
        (is_ref_type && type->script_type_index != 0xffff);

    if (has_script_hash) {
        if (!stream_read_bytes(stream, type->script_id_hash, 16)) {
            return false;
        }
    }

    if (!stream_read_bytes(stream, type->type_hash, 16)) {
        return false;
    }

    if (has_type_tree) {
        uint32_t node_count;
        uint32_t string_buffer_size;
        if (!stream_read_uint32(stream, &node_count)) {
            return false;
        }
        if (!stream_read_uint32(stream, &string_buffer_size)) {
            return false;
        }
        if (!read_tree_payload(type, stream, version, node_count, string_buffer_size)) {
            goto fail;
        }

        if (version >= 21) {
            if (!is_ref_type) {
                uint32_t dep_count;
                if (!stream_read_uint32(stream, &dep_count)) {
                    goto fail;
                }
                if (!read_dependencies(type, stream, dep_count)) {
                    goto fail;
                }
            } else {
                // Read type reference metadata
                size_t cls_len = 0;
                size_t ns_len = 0;
                size_t asm_len = 0;
                type->ref_class_name = stream_read_string_alloc(stream, &cls_len);
                if (!type->ref_class_name) {
                    goto fail;
                }
                type->ref_namespace = stream_read_string_alloc(stream, &ns_len);
                if (!type->ref_namespace) {
                    goto fail;
                }
                type->ref_asm_name = stream_read_string_alloc(stream, &asm_len);
                if (!type->ref_asm_name) {
                    goto fail;
                }
            }
        }
    }

    if (has_type_tree && !typetree_validate_schema(type)) {
        goto fail;
    }
    return true;

fail:
    typetree_free_type(type);
    return false;
}

void typetree_free_type(TypeTreeType* type) {
    if (type->nodes) {
        mem_free(type->nodes, type->node_count * sizeof(TypeTreeNode));
        type->nodes = NULL;
    }
    if (type->string_buffer) {
        mem_free(type->string_buffer, type->string_buffer_size);
        type->string_buffer = NULL;
    }
    if (type->dependencies) {
        mem_free(type->dependencies, type->dependency_count * sizeof(int32_t));
        type->dependencies = NULL;
    }
    if (type->ref_class_name) {
        mem_free(type->ref_class_name, strlen(type->ref_class_name) + 1);
        type->ref_class_name = NULL;
    }
    if (type->ref_namespace) {
        mem_free(type->ref_namespace, strlen(type->ref_namespace) + 1);
        type->ref_namespace = NULL;
    }
    if (type->ref_asm_name) {
        mem_free(type->ref_asm_name, strlen(type->ref_asm_name) + 1);
        type->ref_asm_name = NULL;
    }
    type->node_count = 0;
    type->string_buffer_size = 0;
    type->dependency_count = 0;
}

static bool is_signed_integer_type(const char* type_name) {
    return strcmp(type_name, "SInt8") == 0 ||
           strcmp(type_name, "SInt16") == 0 ||
           strcmp(type_name, "SInt32") == 0 ||
           strcmp(type_name, "SInt64") == 0 ||
           strcmp(type_name, "int") == 0 ||
           strcmp(type_name, "short") == 0 ||
           strcmp(type_name, "long long") == 0;
}

static bool is_unsigned_integer_type(const char* type_name) {
    return strcmp(type_name, "UInt8") == 0 ||
           strcmp(type_name, "UInt16") == 0 ||
           strcmp(type_name, "UInt32") == 0 ||
           strcmp(type_name, "UInt64") == 0 ||
           strcmp(type_name, "unsigned int") == 0 ||
           strcmp(type_name, "unsigned short") == 0 ||
           strcmp(type_name, "unsigned long long") == 0 ||
           strcmp(type_name, "FileSize") == 0 ||
           strcmp(type_name, "RenderingLayerMask") == 0 ||
           strcmp(type_name, "Type*") == 0 ||
           strcmp(type_name, "BitField") == 0 ||
           strcmp(type_name, "bool") == 0 ||
           strcmp(type_name, "char") == 0;
}

static bool typetree_parse_value_internal(const TypeTreeType* type,
                                          int* node_idx,
                                          ByteStream* stream,
                                          TypeTreeValue* out_val,
                                          uint32_t parse_flags,
                                          bool suppress_root_alignment) {
    if (!type || !type->nodes || !node_idx || !stream || !out_val ||
        *node_idx < 0 ||
        *node_idx >= type->node_count) {
        return false;
    }
    const TypeTreeNode* node = &type->nodes[*node_idx];

    memset(out_val, 0, sizeof(*out_val));
    out_val->name = node->name_str;
    out_val->type_str = node->type_str;
    out_val->type = VAL_TYPE_NONE;
    
    bool has_children = false;
    int next_idx = *node_idx + 1;
    if (next_idx < type->node_count && type->nodes[next_idx].level > node->level) {
        has_children = true;
    }
    
    if (!has_children) {
        bool is_signed = is_signed_integer_type(node->type_str);
        bool is_unsigned = is_unsigned_integer_type(node->type_str);
        out_val->integer_is_unsigned = is_unsigned;
        if (node->byte_size == 1 && (is_signed || is_unsigned)) {
            uint8_t val8;
            if (!stream_read_uint8(stream, &val8)) return false;
            out_val->type = VAL_TYPE_INT;
            if (is_signed) {
                out_val->int_val = (int8_t)val8;
            } else {
                out_val->uint_val = val8;
            }
        } else if (node->byte_size == 2 && (is_signed || is_unsigned)) {
            uint16_t val16;
            if (!stream_read_uint16(stream, &val16)) return false;
            out_val->type = VAL_TYPE_INT;
            if (is_signed) {
                out_val->int_val = (int16_t)val16;
            } else {
                out_val->uint_val = val16;
            }
        } else if (node->byte_size == 4) {
            uint32_t val32;
            if (!stream_read_uint32(stream, &val32)) return false;
            if (strcmp(node->type_str, "float") == 0) {
                float value;
                memcpy(&value, &val32, sizeof(value));
                out_val->type = VAL_TYPE_FLOAT;
                out_val->float_val = value;
            } else if (is_signed || is_unsigned) {
                out_val->type = VAL_TYPE_INT;
                if (is_signed) {
                    out_val->int_val = (int32_t)val32;
                } else {
                    out_val->uint_val = val32;
                }
            } else {
                return false;
            }
        } else if (node->byte_size == 8) {
            uint64_t val64;
            if (!stream_read_uint64(stream, &val64)) return false;
            if (strcmp(node->type_str, "double") == 0) {
                double value;
                memcpy(&value, &val64, sizeof(value));
                out_val->type = VAL_TYPE_FLOAT;
                out_val->float_val = value;
            } else if (is_signed || is_unsigned) {
                out_val->type = VAL_TYPE_INT;
                if (is_signed) {
                    out_val->int_val = (int64_t)val64;
                } else {
                    out_val->uint_val = val64;
                }
            } else {
                return false;
            }
        } else {
            return false;
        }
        
        if (!suppress_root_alignment && (node->meta_flags & 0x4000u)) {
            if (!stream_align(stream, 4)) return false;
        }
        (*node_idx)++;
        return true;
    }
    
    bool is_string = strcmp(node->type_str, "string") == 0;
    const TypeTreeNode* child_node = &type->nodes[next_idx];
    bool is_array = strcmp(child_node->type_str, "Array") == 0 || strcmp(child_node->name_str, "Array") == 0;
    
    if (is_string) {
        uint32_t size;
        if (!stream_read_uint32(stream, &size)) return false;
        if (size > stream_remaining(stream) ||
            dxbc_size_add_overflows((size_t)size, 1u)) {
            return false;
        }
        
        char* str = (char*)mem_alloc((size_t)size + 1u);
        if (!str) return false;
        if (size > 0) {
            if (!stream_read_bytes(stream, (uint8_t*)str, size)) {
                mem_free(str, (size_t)size + 1u);
                return false;
            }
        }
        str[size] = '\0';
        
        out_val->type = VAL_TYPE_STRING;
        out_val->string_val = str;
        out_val->string_length = size;
        
        if (!stream_align(stream, 4)) {
            typetree_free_value(out_val);
            return false;
        }
        
        int target_level = node->level;
        (*node_idx)++;
        while (*node_idx < type->node_count && type->nodes[*node_idx].level > target_level) {
            (*node_idx)++;
        }
        return true;
    }
    
    if (is_array) {
        int array_header_idx = next_idx;
        int size_node_idx = array_header_idx + 1;
        int data_node_idx = size_node_idx + 1;
        if (data_node_idx >= type->node_count ||
            type->nodes[size_node_idx].level <= node->level ||
            type->nodes[data_node_idx].level <= node->level) {
            return false;
        }
        
        uint32_t size;
        if (!stream_read_uint32(stream, &size)) return false;
        /* Every schema-valid element consumes at least one wire byte.  This
         * rejects impossible counts before allocating count values. */
        if (size > (uint32_t)INT_MAX || size > stream_remaining(stream)) {
            return false;
        }
        
        out_val->type = VAL_TYPE_ARRAY;
        out_val->array_val.count = (int)size;
        out_val->array_val.elements = NULL;
        out_val->array_val.storage = TYPETREE_ARRAY_VALUES;
        out_val->array_val.packed_bytes = NULL;
        out_val->array_val.packed_bytes_owned = false;

        const TypeTreeNode* data_node = &type->nodes[data_node_idx];
        bool data_has_children = data_node_idx + 1 < type->node_count &&
            type->nodes[data_node_idx + 1].level > data_node->level;
        bool pack_byte_array =
            (parse_flags & (TYPETREE_PARSE_PACK_BYTE_ARRAYS |
                            TYPETREE_PARSE_BORROW_BYTE_ARRAYS)) != 0 &&
            data_node->byte_size == 1 && !data_has_children &&
            strcmp(data_node->type_str, "UInt8") == 0;

        if (pack_byte_array) {
            out_val->array_val.storage = TYPETREE_ARRAY_PACKED_BYTES;
            if (size > 0) {
                if ((parse_flags &
                     TYPETREE_PARSE_BORROW_BYTE_ARRAYS) != 0) {
                    if (size > stream_remaining(stream)) goto fail;
                    out_val->array_val.packed_bytes =
                        stream->data + stream->position;
                    if (!stream_skip(stream, size)) goto fail;
                } else {
                    uint8_t* bytes = (uint8_t*)mem_alloc(size);
                    if (!bytes) goto fail;
                    out_val->array_val.packed_bytes = bytes;
                    out_val->array_val.packed_bytes_owned = true;
                    if (!stream_read_bytes(stream, bytes, size)) goto fail;
                }
            }
        } else if (size > 0) {
            if (dxbc_size_multiply_overflows(
                    (size_t)size, sizeof(TypeTreeValue))) goto fail;
            out_val->array_val.elements = (TypeTreeValue*)mem_alloc(size * sizeof(TypeTreeValue));
            if (!out_val->array_val.elements) goto fail;
            memset(out_val->array_val.elements, 0, size * sizeof(TypeTreeValue));
            
            for (uint32_t i = 0; i < size; i++) {
                int temp_idx = data_node_idx;
                if (!typetree_parse_value_internal(
                        type, &temp_idx, stream,
                        &out_val->array_val.elements[i], parse_flags, true)) {
                    goto fail;
                }
            }
        }
        
        const TypeTreeNode* array_node = &type->nodes[array_header_idx];
        uint32_t alignment_flags = array_node->meta_flags |
                                   data_node->meta_flags;
        if (!suppress_root_alignment) alignment_flags |= node->meta_flags;
        if (alignment_flags & 0x4000u) {
            if (!stream_align(stream, 4)) goto fail;
        }
        
        int target_level = node->level;
        (*node_idx)++;
        while (*node_idx < type->node_count && type->nodes[*node_idx].level > target_level) {
            (*node_idx)++;
        }
        return true;
    }
    
    int child_count = 0;
    int curr_idx = next_idx;
    while (curr_idx < type->node_count && type->nodes[curr_idx].level > node->level) {
        if (type->nodes[curr_idx].level == node->level + 1) {
            child_count++;
        }
        int child_level = type->nodes[curr_idx].level;
        curr_idx++;
        while (curr_idx < type->node_count && type->nodes[curr_idx].level > child_level) {
            curr_idx++;
        }
    }
    
    out_val->type = VAL_TYPE_STRUCT;
    out_val->struct_val.count = child_count;
    if (child_count > 0) {
        if (dxbc_size_multiply_overflows(
                (size_t)child_count, sizeof(TypeTreeValue))) {
            goto fail;
        }
        out_val->struct_val.members = (TypeTreeValue*)mem_alloc(child_count * sizeof(TypeTreeValue));
        if (!out_val->struct_val.members) goto fail;
        memset(out_val->struct_val.members, 0, child_count * sizeof(TypeTreeValue));
        
        int member_idx = 0;
        *node_idx = next_idx;
        while (*node_idx < type->node_count && type->nodes[*node_idx].level > node->level) {
            if (type->nodes[*node_idx].level == node->level + 1) {
                if (!typetree_parse_value_internal(
                        type, node_idx, stream,
                        &out_val->struct_val.members[member_idx],
                        parse_flags, false)) {
                    goto fail;
                }
                member_idx++;
            } else {
                (*node_idx)++;
            }
        }
    } else {
        out_val->struct_val.members = NULL;
        *node_idx = next_idx;
    }
    
    if (!suppress_root_alignment && (node->meta_flags & 0x4000u)) {
        if (!stream_align(stream, 4)) goto fail;
    }
    
    return true;

fail:
    typetree_free_value(out_val);
    return false;
}

bool typetree_parse_value(const TypeTreeType* type, int* node_idx,
                          ByteStream* stream, TypeTreeValue* out_val) {
    if (!typetree_validate_schema(type)) return false;
    return typetree_parse_value_internal(type, node_idx, stream, out_val,
                                         TYPETREE_PARSE_DEFAULT, false);
}

bool typetree_parse_value_ex(const TypeTreeType* type, int* node_idx,
                             ByteStream* stream, TypeTreeValue* out_val,
                             uint32_t parse_flags) {
    const uint32_t known_flags = TYPETREE_PARSE_PACK_BYTE_ARRAYS |
        TYPETREE_PARSE_BORROW_BYTE_ARRAYS;
    if ((parse_flags & ~known_flags) != 0 ||
        (parse_flags & known_flags) == known_flags ||
        !typetree_validate_schema(type)) return false;
    return typetree_parse_value_internal(type, node_idx, stream, out_val,
                                         parse_flags, false);
}

void typetree_free_value(TypeTreeValue* val) {
    if (val->type == VAL_TYPE_STRING) {
        if (val->string_val) {
            mem_free(val->string_val, val->string_length + 1u);
            val->string_val = NULL;
        }
        val->string_length = 0;
    } else if (val->type == VAL_TYPE_ARRAY) {
        if (val->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
            if (val->array_val.packed_bytes &&
                val->array_val.packed_bytes_owned) {
                mem_free((void*)val->array_val.packed_bytes,
                         (size_t)val->array_val.count);
            }
            val->array_val.packed_bytes = NULL;
            val->array_val.packed_bytes_owned = false;
        } else if (val->array_val.elements) {
            for (int i = 0; i < val->array_val.count; i++) {
                typetree_free_value(&val->array_val.elements[i]);
            }
            mem_free(val->array_val.elements, val->array_val.count * sizeof(TypeTreeValue));
            val->array_val.elements = NULL;
        }
        val->array_val.count = 0;
        val->array_val.storage = TYPETREE_ARRAY_VALUES;
        val->array_val.packed_bytes_owned = false;
    } else if (val->type == VAL_TYPE_STRUCT) {
        if (val->struct_val.members) {
            for (int i = 0; i < val->struct_val.count; i++) {
                typetree_free_value(&val->struct_val.members[i]);
            }
            mem_free(val->struct_val.members, val->struct_val.count * sizeof(TypeTreeValue));
            val->struct_val.members = NULL;
        }
    }
    val->type = VAL_TYPE_NONE;
}

const TypeTreeValue* typetree_find_child(const TypeTreeValue* val, const char* name) {
    if (!val || !name) return NULL;
    if (val->type == VAL_TYPE_STRUCT) {
        for (int i = 0; i < val->struct_val.count; i++) {
            if (val->struct_val.members[i].name &&
                strcmp(val->struct_val.members[i].name, name) == 0) {
                return &val->struct_val.members[i];
            }
        }
    }
    return NULL;
}

static const TypeTreeValue* typetree_find_child_n(const TypeTreeValue* val,
                                                  const char* name,
                                                  size_t name_length);

const TypeTreeValue* typetree_find_path(const TypeTreeValue* val, const char* path) {
    if (!val || !path || !*path) return val;
    const TypeTreeValue* curr = val;
    const char* cursor = path;
    while (*cursor && curr) {
        while (*cursor == '/' || *cursor == '.') cursor++;
        if (!*cursor) break;
        const char* end = cursor;
        while (*end && *end != '/' && *end != '.') end++;
        size_t length = (size_t)(end - cursor);
        if (length == 0) return NULL;
        curr = typetree_find_child_n(curr, cursor, length);
        cursor = end;
    }
    return curr;
}

static const TypeTreeValue* typetree_find_child_n(const TypeTreeValue* val,
                                                  const char* name,
                                                  size_t name_length) {
    if (!val || !name || val->type != VAL_TYPE_STRUCT) return NULL;
    for (int i = 0; i < val->struct_val.count; ++i) {
        const char* candidate = val->struct_val.members[i].name;
        if (candidate && strlen(candidate) == name_length &&
            memcmp(candidate, name, name_length) == 0) {
            return &val->struct_val.members[i];
        }
    }
    return NULL;
}

const TypeTreeValue* typetree_get_array(const TypeTreeValue* val) {
    if (!val) return NULL;
    if (val->type == VAL_TYPE_ARRAY) return val;
    if (val->type == VAL_TYPE_STRUCT) {
        const TypeTreeValue* arr = typetree_find_child(val, "Array");
        if (arr && arr->type == VAL_TYPE_ARRAY) return arr;
        arr = typetree_find_child(val, "data");
        if (arr) return typetree_get_array(arr);
    }
    return NULL;
}

bool typetree_value_get_int(const TypeTreeValue* val, int64_t* out_value) {
    if (!val || !out_value || val->type != VAL_TYPE_INT) return false;
    if (val->integer_is_unsigned) {
        if (val->uint_val > INT64_MAX) return false;
        *out_value = (int64_t)val->uint_val;
    } else {
        *out_value = val->int_val;
    }
    return true;
}

bool typetree_value_get_uint(const TypeTreeValue* val, uint64_t* out_value) {
    if (!val || !out_value || val->type != VAL_TYPE_INT) return false;
    if (val->integer_is_unsigned) {
        *out_value = val->uint_val;
    } else {
        if (val->int_val < 0) return false;
        *out_value = (uint64_t)val->int_val;
    }
    return true;
}

bool typetree_array_get_int(const TypeTreeValue* val, int index,
                            int64_t* out_value) {
    const TypeTreeValue* array = typetree_get_array(val);
    if (!array || !out_value || index < 0 || index >= array->array_val.count) {
        return false;
    }
    if (array->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
        if (!array->array_val.packed_bytes) return false;
        *out_value = array->array_val.packed_bytes[index];
        return true;
    }
    if (array->array_val.storage != TYPETREE_ARRAY_VALUES ||
        !array->array_val.elements) {
        return false;
    }
    return typetree_value_get_int(&array->array_val.elements[index],
                                  out_value);
}

bool typetree_array_get_uint(const TypeTreeValue* val, int index,
                             uint64_t* out_value) {
    const TypeTreeValue* array = typetree_get_array(val);
    if (!array || !out_value || index < 0 ||
        index >= array->array_val.count) {
        return false;
    }
    if (array->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
        if (!array->array_val.packed_bytes) return false;
        *out_value = array->array_val.packed_bytes[index];
        return true;
    }
    if (array->array_val.storage != TYPETREE_ARRAY_VALUES ||
        !array->array_val.elements) {
        return false;
    }
    return typetree_value_get_uint(&array->array_val.elements[index],
                                   out_value);
}

bool typetree_get_byte_span(const TypeTreeValue* val, const uint8_t** out_data,
                            size_t* out_size) {
    const TypeTreeValue* array = typetree_get_array(val);
    if (!array || !out_data || !out_size ||
        array->array_val.storage != TYPETREE_ARRAY_PACKED_BYTES ||
        array->array_val.count < 0) {
        return false;
    }
    if (array->array_val.count > 0 && !array->array_val.packed_bytes) {
        return false;
    }
    *out_data = array->array_val.packed_bytes;
    *out_size = (size_t)array->array_val.count;
    return true;
}

bool typetree_array_copy_bytes(const TypeTreeValue* val, size_t offset,
                               uint8_t* destination, size_t count) {
    const TypeTreeValue* array = typetree_get_array(val);
    if (!array || array->array_val.count < 0 ||
        (!destination && count != 0)) {
        return false;
    }
    size_t total = (size_t)array->array_val.count;
    if (offset > total || count > total - offset) return false;
    if (count == 0) return true;

    if (array->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
        if (!array->array_val.packed_bytes) return false;
        memcpy(destination, array->array_val.packed_bytes + offset, count);
        return true;
    }
    if (array->array_val.storage != TYPETREE_ARRAY_VALUES ||
        !array->array_val.elements) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const TypeTreeValue* element =
            &array->array_val.elements[offset + i];
        uint64_t byte = 0;
        if (!typetree_value_get_uint(element, &byte) || byte > UINT8_MAX) {
            return false;
        }
        destination[i] = (uint8_t)byte;
    }
    return true;
}
