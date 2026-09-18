// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_file_schema.h"

#include "serialized_file_directory_internal.h"
#include "serialized_file_metadata_tail_internal.h"
#include "serialized_file_schema_internal.h"
#include "serialized_metadata_reader_internal.h"
#include "typetree_common_strings_internal.h"

#include "common/common.h"

#include <stdalign.h>

_Static_assert(SIZE_MAX >= UINT32_MAX, "Schema counts retain their full u32 domain");
_Static_assert(SIZE_MAX <= UINT64_MAX, "Mapped sizes must fit source coordinates");

enum {
    NODE_BYTES = 32,
    HIERARCHY_LEVELS = 256,
    RADIX_BUCKETS = 256,
    RADIX_PASSES = 4,
    NAMES_PER_NODE = 2
};

typedef struct SchemaStorage {
    SerializedFileSchemaView view;
    SerializedFileSchemaNode* nodes;
} SchemaStorage;

typedef struct NameRequest {
    uint32_t encoded_offset;
    size_t slot; /* Two stable slots per original node: type, then field name. */
} NameRequest;

typedef struct SchemaParent {
    const void* handle;
    size_t handle_size;
    const void* storage;
    size_t storage_size;
    const SerializedFileDirectoryView* directory;
    const SerializedFileMetadataTailView* tail;
    const uint8_t* bytes;
    size_t known_size;
} SchemaParent;

typedef struct SchemaLayout {
    size_t nodes_offset;
    size_t retained_bytes;
    size_t scratch_bytes;
    size_t request_count;
} SchemaLayout;

typedef struct SchemaCompile {
    const SerializedFileSchemaLimits* limits;
    SerializedFileSchemaResult result;
    SerializedFileSchemaView view;
    SchemaStorage* storage;
    NameRequest* requests;
    NameRequest* sorted;
    size_t request_count;
    const uint8_t* common_bytes;
    size_t common_size;
} SchemaCompile;

static SerializedFileSchemaResult initial_result(void) {
    const SerializedFileSchemaResult result = {.status = SERIALIZED_FILE_SCHEMA_OK,
        .type_ordinal = SIZE_MAX,
        .node_ordinal = SIZE_MAX,
        .error_offset = UINT64_MAX};
    return result;
}

static SerializedFilePrefixSpan absent_span(void) {
    const SerializedFilePrefixSpan span = {NULL, UINT64_MAX, 0U};
    return span;
}

static bool reject(SchemaCompile* compile,
    SerializedFileSchemaStatus status,
    SerializedFileSchemaLimit limit,
    SerializedFileSchemaField field,
    size_t node,
    SerializedFileSchemaErrorSpace space,
    uint64_t offset) {
    compile->result.status = status;
    compile->result.limit = limit;
    compile->result.field = field;
    compile->result.node_ordinal = node;
    compile->result.error_space = space;
    compile->result.error_offset = offset;
    return false;
}

static bool reject_owner(
    SchemaCompile* compile, SerializedFileSchemaStatus status, SerializedFileSchemaLimit limit) {
    return reject(compile,
        status,
        limit,
        SERIALIZED_FILE_SCHEMA_FIELD_NONE,
        SIZE_MAX,
        SERIALIZED_FILE_SCHEMA_ERROR_NO_SOURCE,
        UINT64_MAX);
}

static bool charge(SchemaCompile* compile,
    uint64_t amount,
    SerializedFileSchemaField field,
    size_t node,
    SerializedFileSchemaErrorSpace space,
    uint64_t offset) {
    if (amount > compile->limits->max_work - compile->result.work_used) {
        return reject(compile,
            SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED,
            SERIALIZED_FILE_SCHEMA_LIMIT_WORK,
            field,
            node,
            space,
            offset);
    }
    compile->result.work_used += amount;
    return true;
}

static bool charge_owner(SchemaCompile* compile, uint64_t amount) {
    return charge(compile,
        amount,
        SERIALIZED_FILE_SCHEMA_FIELD_NONE,
        SIZE_MAX,
        SERIALIZED_FILE_SCHEMA_ERROR_NO_SOURCE,
        UINT64_MAX);
}

static bool basic_arguments(const SchemaParent* parent,
    const SerializedFileSchemaLimits* limits,
    const SerializedFileSchema* output) {
    return parent->handle && limits && output &&
        !serialized_file_storage_overlaps_internal(
            parent->handle, parent->handle_size, limits, sizeof(*limits)) &&
        !serialized_file_storage_overlaps_internal(
            parent->handle, parent->handle_size, output, sizeof(*output)) &&
        !serialized_file_storage_overlaps_internal(
            limits, sizeof(*limits), output, sizeof(*output));
}

static bool prepare_parent(SchemaParent* parent, SerializedFileSchemaRowKind kind) {
    uint64_t end;
    if (kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE) {
        const SerializedFileDirectory* directory = parent->handle;
        if (!serialized_file_directory_storage_range_internal(
                directory, &parent->storage, &parent->storage_size)) {
            return false;
        }
        parent->directory = serialized_file_directory_view(directory);
        end = parent->directory->remaining_metadata.offset;
    } else {
        const SerializedFileMetadataTail* tail = parent->handle;
        if (!serialized_file_metadata_tail_storage_range_internal(
                tail, &parent->storage, &parent->storage_size)) {
            return false;
        }
        parent->tail = serialized_file_metadata_tail_view(tail);
        parent->directory = &parent->tail->directory;
        if (parent->tail->source.size > UINT64_MAX - parent->tail->source.offset) {
            return false;
        }
        end = parent->tail->source.offset + parent->tail->source.size;
    }
    if (end > SIZE_MAX) {
        return false;
    }
    parent->bytes = parent->directory->prefix.header.header_source.data;
    parent->known_size = (size_t)end;
    return parent->bytes != NULL;
}

static bool parent_ranges_disjoint(
    const SchemaParent* parent, const SchemaCompile* compile, const SerializedFileSchema* output) {
    const void* const ranges[] = {parent->handle,
        parent->storage,
        parent->bytes,
        compile->common_bytes,
        compile->limits,
        output};
    const size_t sizes[] = {parent->handle_size,
        parent->storage_size,
        parent->known_size,
        compile->common_size,
        sizeof(*compile->limits),
        sizeof(*output)};
    for (size_t first = 0U; first < 6U; ++first) {
        for (size_t second = first + 1U; second < 6U; ++second) {
            if (serialized_file_storage_overlaps_internal(
                    ranges[first], sizes[first], ranges[second], sizes[second])) {
                return false;
            }
        }
    }
    return true;
}

static bool select_row(SchemaCompile* compile,
    const SchemaParent* parent,
    SerializedFileSchemaRowKind kind,
    size_t ordinal) {
    if (parent->directory->engine_version != SERIALIZED_FILE_DIRECTORY_ENGINE_2021_3_35F1) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_UNSUPPORTED_ENGINE, SERIALIZED_FILE_SCHEMA_LIMIT_NONE);
    }
    SerializedFileSchemaView* view = &compile->view;
    view->engine_version = parent->directory->engine_version;
    view->prefix = parent->directory->prefix;
    view->row_kind = kind;
    view->type_ordinal = ordinal;
    bool has_tree;
    if (kind == SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE) {
        const SerializedFileDirectoryTypeRow* row =
            serialized_file_directory_type(parent->handle, ordinal);
        if (!row) {
            return reject_owner(compile,
                SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE);
        }
        view->type_entry_source = row->source;
        view->tree = row->tree;
        view->class_id_bits = row->class_id_bits;
        view->script_index_bits = row->script_index_bits;
        view->stripped_raw = row->stripped_raw;
        view->has_script_hash = row->has_script_hash;
        view->script_hash_source = row->script_hash_source;
        view->type_hash_source = row->type_hash_source;
        view->class_name_source = absent_span();
        view->namespace_source = absent_span();
        view->assembly_name_source = absent_span();
        view->dependency_count_source = row->dependency_count_source;
        view->dependency_words_source = row->dependency_words_source;
        view->dependency_count = row->dependency_count;
        has_tree = row->has_tree;
    } else {
        const SerializedFileMetadataTailReferenceTypeRow* row =
            serialized_file_metadata_tail_reference_type(parent->handle, ordinal);
        if (!row) {
            return reject_owner(compile,
                SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE);
        }
        view->type_entry_source = row->source;
        view->tree = row->tree;
        view->class_id_bits = row->class_id_bits;
        view->script_index_bits = row->script_index_bits;
        view->stripped_raw = row->stripped_raw;
        view->has_script_hash = row->has_script_hash;
        view->script_hash_source = row->script_hash_source;
        view->type_hash_source = row->type_hash_source;
        view->class_name_source = row->class_name_source;
        view->namespace_source = row->namespace_source;
        view->assembly_name_source = row->assembly_name_source;
        view->dependency_count_source = absent_span();
        view->dependency_words_source = absent_span();
        has_tree = row->has_tree;
    }
    compile->result.type_ordinal = ordinal;
    if (!has_tree) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_NO_EMBEDDED_TREE, SERIALIZED_FILE_SCHEMA_LIMIT_NONE);
    }
    view->node_count = view->tree.node_count;
    return true;
}

static bool source_is_backed(const SchemaParent* parent, SerializedFilePrefixSpan span) {
    return span.offset <= parent->known_size && span.size <= parent->known_size - span.offset &&
        span.data == parent->bytes + (size_t)span.offset;
}

static bool admit_tree(SchemaCompile* compile, const SchemaParent* parent) {
    const SerializedFileDirectoryTree* tree = &compile->view.tree;
    if (tree->node_count > compile->limits->max_nodes) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_NODES);
    }
    if (tree->string_byte_count > compile->limits->max_local_string_bytes) {
        return reject_owner(compile,
            SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED,
            SERIALIZED_FILE_SCHEMA_LIMIT_LOCAL_STRINGS);
    }
    const uint64_t node_bytes = (uint64_t)tree->node_count * NODE_BYTES;
    if (!tree->node_count || node_bytes != tree->nodes_source.size ||
        tree->string_byte_count != tree->strings_source.size ||
        !source_is_backed(parent, tree->nodes_source) ||
        !source_is_backed(parent, tree->strings_source) ||
        tree->nodes_source.offset > UINT64_MAX - node_bytes ||
        tree->strings_source.offset != tree->nodes_source.offset + node_bytes) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_SOURCE_MISMATCH, SERIALIZED_FILE_SCHEMA_LIMIT_NONE);
    }
    return true;
}

static bool plan_storage(SchemaCompile* compile, SchemaLayout* layout) {
    if (!charge_owner(compile, 1U)) {
        return false;
    }
    layout->retained_bytes = sizeof(SchemaStorage);
    if (!serialized_file_storage_append_array_internal(&layout->retained_bytes,
            alignof(SerializedFileSchemaNode),
            compile->view.node_count,
            sizeof(SerializedFileSchemaNode),
            &layout->nodes_offset)) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_RETAINED);
    }
    compile->result.required_retained_bytes = layout->retained_bytes;
    if (layout->retained_bytes > compile->limits->max_retained_bytes) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_RETAINED);
    }
    size_t request_offset;
    if (dxbc_size_multiply_overflows(compile->view.node_count, NAMES_PER_NODE)) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH);
    }
    layout->request_count = compile->view.node_count * NAMES_PER_NODE;
    if (!serialized_file_storage_append_array_internal(&layout->scratch_bytes,
            alignof(NameRequest),
            layout->request_count,
            sizeof(NameRequest),
            &request_offset) ||
        dxbc_size_multiply_overflows(layout->scratch_bytes, 2U)) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH);
    }
    layout->scratch_bytes *= 2U;
    compile->result.required_scratch_bytes = layout->scratch_bytes;
    if (layout->scratch_bytes > compile->limits->max_scratch_bytes) {
        return reject_owner(
            compile, SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED, SERIALIZED_FILE_SCHEMA_LIMIT_SCRATCH);
    }
    return true;
}

static void decode_node(SchemaCompile* compile, size_t ordinal) {
    const SerializedFilePrefixSpan source = compile->view.tree.nodes_source;
    const size_t offset = ordinal * NODE_BYTES;
    const uint8_t* bytes = source.data + offset;
    const uint8_t endian = compile->view.prefix.header.endian_selector;
    SerializedFileSchemaNode* node = &compile->storage->nodes[ordinal];
    node->ordinal = ordinal;
    node->source = (SerializedFilePrefixSpan){bytes, source.offset + offset, NODE_BYTES};
    node->version = serialized_metadata_decode_u16(bytes, endian);
    node->level = bytes[2];
    node->type_flags = bytes[3];
    node->type_offset_bits = serialized_metadata_decode_u32(bytes + 4U, endian);
    node->name_offset_bits = serialized_metadata_decode_u32(bytes + 8U, endian);
    node->byte_size_bits = serialized_metadata_decode_u32(bytes + 12U, endian);
    node->index_bits = serialized_metadata_decode_u32(bytes + 16U, endian);
    node->meta_flags = serialized_metadata_decode_u32(bytes + 20U, endian);
    memcpy(node->opaque_tail, bytes + 24U, sizeof(node->opaque_tail));
    node->parent = SIZE_MAX;
    node->first_child = SIZE_MAX;
    node->next_sibling = SIZE_MAX;
    const size_t slot = ordinal * NAMES_PER_NODE;
    compile->requests[slot] = (NameRequest){node->type_offset_bits, slot};
    compile->requests[slot + 1U] = (NameRequest){node->name_offset_bits, slot + 1U};
}

static bool close_subtree(SchemaCompile* compile, size_t ordinal, size_t end) {
    const SerializedFileSchemaNode* node = &compile->storage->nodes[ordinal];
    if (!charge(compile,
            1U,
            SERIALIZED_FILE_SCHEMA_FIELD_LEVEL,
            ordinal,
            SERIALIZED_FILE_SCHEMA_ERROR_FILE,
            node->source.offset + 2U)) {
        return false;
    }
    compile->storage->nodes[ordinal].subtree_end = end;
    return true;
}

static bool compile_nodes(SchemaCompile* compile) {
    size_t ancestors[HIERARCHY_LEVELS];
    size_t last_child[HIERARCHY_LEVELS];
    size_t active_count = 0U;
    const size_t count = compile->view.node_count;
    for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
        const uint64_t offset = compile->view.tree.nodes_source.offset + ordinal * NODE_BYTES;
        if (!charge(compile,
                NODE_BYTES + 1U,
                SERIALIZED_FILE_SCHEMA_FIELD_NODE_RECORD,
                ordinal,
                SERIALIZED_FILE_SCHEMA_ERROR_FILE,
                offset)) {
            return false;
        }
        decode_node(compile, ordinal);
        SerializedFileSchemaNode* node = &compile->storage->nodes[ordinal];
        const size_t level = node->level;
        if ((!ordinal && level) || (ordinal && (!level || level > active_count))) {
            return reject(compile,
                SERIALIZED_FILE_SCHEMA_MALFORMED_HIERARCHY,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
                SERIALIZED_FILE_SCHEMA_FIELD_LEVEL,
                ordinal,
                SERIALIZED_FILE_SCHEMA_ERROR_FILE,
                offset + 2U);
        }
        if (level > compile->limits->max_depth) {
            return reject(compile,
                SERIALIZED_FILE_SCHEMA_LIMIT_EXCEEDED,
                SERIALIZED_FILE_SCHEMA_LIMIT_DEPTH,
                SERIALIZED_FILE_SCHEMA_FIELD_LEVEL,
                ordinal,
                SERIALIZED_FILE_SCHEMA_ERROR_FILE,
                offset + 2U);
        }
        while (active_count > level) {
            --active_count;
            if (!close_subtree(compile, ancestors[active_count], ordinal)) {
                return false;
            }
        }
        if (level) {
            node->parent = ancestors[level - 1U];
            SerializedFileSchemaNode* parent = &compile->storage->nodes[node->parent];
            if (last_child[level - 1U] == SIZE_MAX) {
                parent->first_child = ordinal;
            } else {
                compile->storage->nodes[last_child[level - 1U]].next_sibling = ordinal;
            }
            ++parent->child_count;
            last_child[level - 1U] = ordinal;
        }
        ancestors[level] = ordinal;
        last_child[level] = SIZE_MAX;
        active_count = level + 1U;
        if (level > compile->view.maximum_depth) {
            compile->view.maximum_depth = level;
        }
    }
    while (active_count) {
        --active_count;
        if (!close_subtree(compile, ancestors[active_count], count)) {
            return false;
        }
    }
    return true;
}

static bool sort_name_requests(SchemaCompile* compile) {
    NameRequest* source = compile->requests;
    NameRequest* destination = compile->sorted;
    for (unsigned int pass = 0U; pass < RADIX_PASSES; ++pass) {
        if (!charge_owner(compile, 2U * RADIX_BUCKETS)) {
            return false;
        }
        size_t offsets[RADIX_BUCKETS] = {0};
        const unsigned int shift = pass * 8U;
        for (size_t index = 0U; index < compile->request_count; ++index) {
            if (!charge_owner(compile, 1U)) {
                return false;
            }
            const uint32_t bucket = (source[index].encoded_offset >> shift) & UINT32_C(0xff);
            ++offsets[bucket];
        }
        size_t start = 0U;
        for (size_t bucket = 0U; bucket < RADIX_BUCKETS; ++bucket) {
            const size_t count = offsets[bucket];
            offsets[bucket] = start;
            start += count;
        }
        for (size_t index = 0U; index < compile->request_count; ++index) {
            if (!charge_owner(compile, 1U)) {
                return false;
            }
            const uint32_t bucket = (source[index].encoded_offset >> shift) & UINT32_C(0xff);
            destination[offsets[bucket]++] = source[index];
        }
        NameRequest* previous = source;
        source = destination;
        destination = previous;
    }
    /* Four stable passes return sorted requests to the original scratch half. */
    return source == compile->requests;
}

static SerializedFileSchemaField request_field(const NameRequest* request) {
    return request->slot % NAMES_PER_NODE ? SERIALIZED_FILE_SCHEMA_FIELD_NAME_OFFSET
                                          : SERIALIZED_FILE_SCHEMA_FIELD_TYPE_OFFSET;
}

static uint64_t request_error_offset(const SchemaCompile* compile,
    const NameRequest* request,
    SerializedFileSchemaStringSpace space) {
    if (space == SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35) {
        return request->encoded_offset & UINT32_C(0x7fffffff);
    }
    const SerializedFileSchemaNode* node = &compile->storage->nodes[request->slot / NAMES_PER_NODE];
    return node->source.offset + (request->slot % NAMES_PER_NODE ? 8U : 4U);
}

static bool resolve_request(SchemaCompile* compile,
    const NameRequest* request,
    SerializedFileSchemaStringSpace space,
    const uint8_t* bytes,
    uint64_t origin,
    size_t start,
    size_t end,
    bool terminated) {
    const size_t ordinal = request->slot / NAMES_PER_NODE;
    const uint32_t offset = request->encoded_offset & UINT32_C(0x7fffffff);
    const SerializedFileSchemaErrorSpace error_space = space == SERIALIZED_FILE_SCHEMA_STRING_LOCAL
        ? SERIALIZED_FILE_SCHEMA_ERROR_FILE
        : SERIALIZED_FILE_SCHEMA_ERROR_COMMON_EXACT35;
    const uint64_t error_offset = request_error_offset(compile, request, space);
    if (!charge(compile, 1U, request_field(request), ordinal, error_space, error_offset)) {
        return false;
    }
    if (offset != start) {
        return reject(compile,
            SERIALIZED_FILE_SCHEMA_INVALID_STRING_OFFSET,
            SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
            request_field(request),
            ordinal,
            error_space,
            error_offset);
    }
    if (!terminated) {
        return reject(compile,
            SERIALIZED_FILE_SCHEMA_UNTERMINATED_STRING,
            SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
            SERIALIZED_FILE_SCHEMA_FIELD_STRING_TERMINATOR,
            ordinal,
            error_space,
            origin + end);
    }
    SerializedFileSchemaNode* node = &compile->storage->nodes[ordinal];
    SerializedFileSchemaString* name =
        request->slot % NAMES_PER_NODE ? &node->field_name : &node->type_name;
    *name = (SerializedFileSchemaString){
        space, request->encoded_offset, bytes + start, end - start - 1U, origin + start};
    return true;
}

static bool resolve_table(SchemaCompile* compile,
    SerializedFileSchemaStringSpace space,
    const uint8_t* bytes,
    size_t size,
    uint64_t origin,
    size_t* request_index) {
    const bool common = space == SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35;
    const SerializedFileSchemaErrorSpace error_space =
        common ? SERIALIZED_FILE_SCHEMA_ERROR_COMMON_EXACT35 : SERIALIZED_FILE_SCHEMA_ERROR_FILE;
    size_t start = 0U;
    for (size_t cursor = 0U; cursor < size; ++cursor) {
        if (!charge(compile,
                1U,
                SERIALIZED_FILE_SCHEMA_FIELD_STRING_TERMINATOR,
                SIZE_MAX,
                error_space,
                origin + cursor)) {
            return false;
        }
        const bool terminated = bytes[cursor] == 0U;
        if (!terminated && cursor + 1U < size) {
            continue;
        }
        const size_t end = cursor + 1U;
        while (*request_index < compile->request_count) {
            const NameRequest* request = &compile->requests[*request_index];
            if (((request->encoded_offset & UINT32_C(0x80000000)) != 0U) != common ||
                (request->encoded_offset & UINT32_C(0x7fffffff)) >= end) {
                break;
            }
            if (!resolve_request(compile, request, space, bytes, origin, start, end, terminated)) {
                return false;
            }
            ++*request_index;
        }
        start = end;
    }
    if (*request_index < compile->request_count) {
        const NameRequest* request = &compile->requests[*request_index];
        if (((request->encoded_offset & UINT32_C(0x80000000)) != 0U) == common) {
            const size_t node = request->slot / NAMES_PER_NODE;
            const uint64_t error_offset = request_error_offset(compile, request, space);
            if (!charge(compile, 1U, request_field(request), node, error_space, error_offset)) {
                return false;
            }
            return reject(compile,
                SERIALIZED_FILE_SCHEMA_INVALID_STRING_OFFSET,
                SERIALIZED_FILE_SCHEMA_LIMIT_NONE,
                request_field(request),
                node,
                error_space,
                error_offset);
        }
    }
    return true;
}

static bool resolve_names(SchemaCompile* compile) {
    size_t request_index = 0U;
    const SerializedFilePrefixSpan local = compile->view.tree.strings_source;
    return resolve_table(compile,
               SERIALIZED_FILE_SCHEMA_STRING_LOCAL,
               local.data,
               local.size,
               local.offset,
               &request_index) &&
        resolve_table(compile,
            SERIALIZED_FILE_SCHEMA_STRING_COMMON_EXACT35,
            compile->common_bytes,
            compile->common_size,
            0U,
            &request_index) &&
        request_index == compile->request_count;
}

static SerializedFileSchemaResult create_schema(SchemaParent parent,
    SerializedFileSchemaRowKind kind,
    size_t ordinal,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* output) {
    SchemaCompile compile = {.limits = limits, .result = initial_result()};
    if (!basic_arguments(&parent, limits, output)) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT;
        return compile.result;
    }
    if (!prepare_parent(&parent, kind)) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_INVALID_STATE;
        return compile.result;
    }
    compile.common_bytes = typetree_common_string_table(&compile.common_size);
    if (!parent_ranges_disjoint(&parent, &compile, output)) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_INVALID_ARGUMENT;
        return compile.result;
    }
    if (output->implementation) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_INVALID_STATE;
        return compile.result;
    }
    SchemaLayout layout = {0};
    if (!charge_owner(&compile, 1U) || !select_row(&compile, &parent, kind, ordinal) ||
        !admit_tree(&compile, &parent) || !plan_storage(&compile, &layout) ||
        !charge_owner(&compile, layout.retained_bytes)) {
        return compile.result;
    }
    compile.storage = mem_alloc(layout.retained_bytes);
    if (!compile.storage) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_ALLOCATION_FAILED;
        return compile.result;
    }
    compile.result.peak_retained_bytes = layout.retained_bytes;
    memset(compile.storage, 0, layout.retained_bytes);
    compile.storage->nodes =
        (SerializedFileSchemaNode*)((uint8_t*)compile.storage + layout.nodes_offset);
    if (!charge_owner(&compile, layout.scratch_bytes)) {
        goto cleanup_storage;
    }
    compile.requests = mem_alloc(layout.scratch_bytes);
    if (!compile.requests) {
        compile.result.status = SERIALIZED_FILE_SCHEMA_ALLOCATION_FAILED;
        goto cleanup_storage;
    }
    compile.result.peak_scratch_bytes = layout.scratch_bytes;
    memset(compile.requests, 0, layout.scratch_bytes);
    compile.request_count = layout.request_count;
    compile.sorted = compile.requests + compile.request_count;
    if (compile_nodes(&compile) && sort_name_requests(&compile) && resolve_names(&compile) &&
        charge_owner(&compile, 1U)) {
        compile.view.retained_bytes = layout.retained_bytes;
        compile.storage->view = compile.view;
        output->implementation = compile.storage;
        compile.storage = NULL;
    }
    mem_free(compile.requests, layout.scratch_bytes);
cleanup_storage:
    mem_free(compile.storage, layout.retained_bytes);
    return compile.result;
}

void serialized_file_schema_init(SerializedFileSchema* schema) {
    if (schema) {
        schema->implementation = NULL;
    }
}

void serialized_file_schema_dispose(SerializedFileSchema* schema) {
    if (schema && schema->implementation) {
        SchemaStorage* storage = schema->implementation;
        mem_free(storage, storage->view.retained_bytes);
        schema->implementation = NULL;
    }
}

SerializedFileSchemaResult serialized_file_schema_create_ordinary(
    const SerializedFileDirectory* directory,
    size_t ordinary_type_ordinal,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* out_schema) {
    const SchemaParent parent = {.handle = directory, .handle_size = sizeof(*directory)};
    return create_schema(
        parent, SERIALIZED_FILE_SCHEMA_ORDINARY_TYPE, ordinary_type_ordinal, limits, out_schema);
}

SerializedFileSchemaResult serialized_file_schema_create_reference(
    const SerializedFileMetadataTail* tail,
    size_t reference_type_ordinal,
    const SerializedFileSchemaLimits* limits,
    SerializedFileSchema* out_schema) {
    const SchemaParent parent = {.handle = tail, .handle_size = sizeof(*tail)};
    return create_schema(
        parent, SERIALIZED_FILE_SCHEMA_REFERENCE_TYPE, reference_type_ordinal, limits, out_schema);
}

const SerializedFileSchemaView* serialized_file_schema_view(const SerializedFileSchema* schema) {
    const SchemaStorage* storage = schema ? schema->implementation : NULL;
    return storage ? &storage->view : NULL;
}

const SerializedFileSchemaNode* serialized_file_schema_node(
    const SerializedFileSchema* schema, size_t node_ordinal) {
    const SchemaStorage* storage = schema ? schema->implementation : NULL;
    return storage && node_ordinal < storage->view.node_count ? &storage->nodes[node_ordinal]
                                                              : NULL;
}

bool serialized_file_schema_storage_range_internal(
    const SerializedFileSchema* schema, const void** out_data, size_t* out_size) {
    const SchemaStorage* storage = schema ? schema->implementation : NULL;
    if (!storage) {
        return false;
    }
    *out_data = storage;
    *out_size = storage->view.retained_bytes;
    return true;
}
