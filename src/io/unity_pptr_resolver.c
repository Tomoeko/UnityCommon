// SPDX-License-Identifier: GPL-3.0-only

#include "io/unity_pptr_resolver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum {
    UNITY_PPTR_MAX_LOCATOR_BYTES = 65535,
};

typedef struct {
    char* text;
    bool absolute;
} NormalizedPath;

typedef enum {
    BUILTIN_CONTRACT_NONE = 0,
    BUILTIN_CONTRACT_EXTRA,
    BUILTIN_CONTRACT_DEFAULT_RESOURCES,
    BUILTIN_CONTRACT_INVALID,
} BuiltinContract;

static bool is_separator(char value) {
#ifdef _WIN32
    /* Unity locators are slash-separated, while native catalog paths may use
     * either Windows separator. */
    return value == '/' || value == '\\';
#else
    /* A backslash is an ordinary POSIX filename byte. Treating it as a
     * separator can redirect an external PPtr to the wrong sibling. */
    return value == '/';
#endif
}

#ifdef _WIN32
static bool ascii_is_drive_letter(char value) {
    unsigned char byte = (unsigned char)value;
    return isalpha(byte) != 0;
}
#endif

static size_t bounded_string_length(const char* value, size_t maximum) {
    if (!value) return maximum + 1U;
    size_t length = 0U;
    while (length <= maximum && value[length] != '\0') ++length;
    return length;
}

static void normalized_path_dispose(NormalizedPath* path) {
    if (!path) return;
    free(path->text);
    path->text = NULL;
    path->absolute = false;
}

static bool component_is(const char* component, size_t length,
                         const char* expected) {
    size_t expected_length = strlen(expected);
    return length == expected_length &&
           memcmp(component, expected, length) == 0;
}

static bool normalize_path(const char* input, NormalizedPath* result) {
    if (!input || !result) return false;
    memset(result, 0, sizeof(*result));

    size_t input_length = bounded_string_length(
        input, UNITY_PPTR_MAX_LOCATOR_BYTES);
    if (input_length == 0U || input_length > UNITY_PPTR_MAX_LOCATOR_BYTES) {
        return false;
    }
    if (input_length > SIZE_MAX - 4U ||
        input_length > (SIZE_MAX / sizeof(size_t)) - 1U) {
        return false;
    }

    char* output = (char*)malloc(input_length + 4U);
    size_t* component_marks = (size_t*)malloc(
        (input_length + 1U) * sizeof(*component_marks));
    bool* component_is_parent = (bool*)malloc(input_length + 1U);
    if (!output || !component_marks || !component_is_parent) {
        free(output);
        free(component_marks);
        free(component_is_parent);
        return false;
    }

    size_t input_at = 0U;
    size_t output_at = 0U;
    size_t component_count = 0U;
    bool absolute = false;

#ifdef _WIN32
    if (input_length >= 2U && ascii_is_drive_letter(input[0]) &&
        input[1] == ':') {
        output[output_at++] = (char)toupper((unsigned char)input[0]);
        output[output_at++] = ':';
        input_at = 2U;
        if (input_at < input_length && is_separator(input[input_at])) {
            absolute = true;
            output[output_at++] = '/';
            while (input_at < input_length &&
                   is_separator(input[input_at])) {
                ++input_at;
            }
        }
    } else
#endif
    if (is_separator(input[0])) {
        absolute = true;
        output[output_at++] = '/';
        while (input_at < input_length && is_separator(input[input_at])) {
            ++input_at;
        }
    }

    while (input_at < input_length) {
        while (input_at < input_length && is_separator(input[input_at])) {
            ++input_at;
        }
        if (input_at == input_length) break;

        size_t component_start = input_at;
        while (input_at < input_length && !is_separator(input[input_at])) {
            ++input_at;
        }
        size_t component_length = input_at - component_start;
        if (component_is(input + component_start, component_length, ".")) {
            continue;
        }
        if (component_is(input + component_start, component_length, "..")) {
            if (component_count != 0U &&
                !component_is_parent[component_count - 1U]) {
                output_at = component_marks[--component_count];
                continue;
            }
            if (absolute) {
                free(output);
                free(component_marks);
                free(component_is_parent);
                return false;
            }
        }

        size_t mark = output_at;
        if (output_at != 0U && output[output_at - 1U] != '/' &&
            output[output_at - 1U] != ':') {
            output[output_at++] = '/';
        }
        component_marks[component_count++] = mark;
        component_is_parent[component_count - 1U] = component_is(
            input + component_start, component_length, "..");
        memcpy(output + output_at, input + component_start,
               component_length);
        output_at += component_length;
    }

    if (output_at == 0U) {
        output[output_at++] = '.';
    } else if (output_at == 2U && output[1] == ':') {
        output[output_at++] = '.';
    }
    output[output_at] = '\0';
    free(component_marks);
    free(component_is_parent);

    result->text = output;
    result->absolute = absolute;
    return true;
}

static bool normalized_equal(const NormalizedPath* left,
                             const NormalizedPath* right) {
    return left && right && left->text && right->text &&
           left->absolute == right->absolute &&
           strcmp(left->text, right->text) == 0;
}

static bool path_is_within(const NormalizedPath* path,
                           const NormalizedPath* root) {
    if (!path || !root || !path->text || !root->text ||
        path->absolute != root->absolute) {
        return false;
    }
    if (strcmp(path->text, root->text) == 0) return true;
    if (!root->absolute && strcmp(root->text, ".") == 0) {
        return strcmp(path->text, "..") != 0 &&
               strncmp(path->text, "../", 3U) != 0;
    }
    size_t root_length = strlen(root->text);
    if (strncmp(path->text, root->text, root_length) != 0) return false;
    if (root_length != 0U && root->text[root_length - 1U] == '/') {
        return true;
    }
    return path->text[root_length] == '/';
}

static bool parent_path(const NormalizedPath* input, NormalizedPath* result) {
    if (!input || !input->text || !result) return false;
    memset(result, 0, sizeof(*result));

    size_t length = strlen(input->text);
    size_t root_length = input->absolute && length >= 3U &&
        input->text[1] == ':' ? 3U : (input->absolute ? 1U : 0U);
    if (length <= root_length) {
        return normalize_path(input->text, result);
    }

    const char* slash = strrchr(input->text, '/');
    size_t parent_length;
    if (!slash) {
        parent_length = 1U;
    } else {
        parent_length = (size_t)(slash - input->text);
        if (parent_length < root_length) parent_length = root_length;
    }

    char* copy;
    if (!slash) {
        copy = (char*)malloc(2U);
        if (!copy) return false;
        copy[0] = '.';
        copy[1] = '\0';
    } else {
        copy = (char*)malloc(parent_length + 1U);
        if (!copy) return false;
        memcpy(copy, input->text, parent_length);
        copy[parent_length] = '\0';
    }
    result->text = copy;
    result->absolute = input->absolute;
    return true;
}

static bool join_path(const NormalizedPath* base, const char* relative,
                      NormalizedPath* result) {
    if (!base || !base->text || !relative || !result) return false;
    NormalizedPath relative_path;
    if (!normalize_path(relative, &relative_path)) return false;
    if (relative_path.absolute
#ifdef _WIN32
        || (strlen(relative_path.text) >= 2U &&
            relative_path.text[1] == ':')
#endif
        ) {
        normalized_path_dispose(&relative_path);
        return false;
    }

    size_t base_length = strlen(base->text);
    size_t relative_length = strlen(relative_path.text);
    bool base_is_dot = !base->absolute && strcmp(base->text, ".") == 0;
    bool needs_separator = !base_is_dot && base_length != 0U &&
        base->text[base_length - 1U] != '/';
    size_t prefix_length = base_is_dot ? 0U : base_length;
    if (prefix_length > SIZE_MAX - (needs_separator ? 1U : 0U) ||
        prefix_length + (needs_separator ? 1U : 0U) >
            SIZE_MAX - relative_length - 1U) {
        normalized_path_dispose(&relative_path);
        return false;
    }

    size_t combined_length = prefix_length +
        (needs_separator ? 1U : 0U) + relative_length;
    char* combined = (char*)malloc(combined_length + 1U);
    if (!combined) {
        normalized_path_dispose(&relative_path);
        return false;
    }
    size_t at = 0U;
    if (prefix_length != 0U) {
        memcpy(combined, base->text, prefix_length);
        at = prefix_length;
    }
    if (needs_separator) combined[at++] = '/';
    memcpy(combined + at, relative_path.text, relative_length + 1U);
    normalized_path_dispose(&relative_path);

    bool ok = normalize_path(combined, result);
    free(combined);
    return ok;
}

static bool guid_is_zero(const uint8_t guid[16]) {
    static const uint8_t zero[16] = {0};
    return guid && memcmp(guid, zero, sizeof(zero)) == 0;
}

static bool guid_matches_node(const AssetFileExternal* external,
                              const UnityPPtrSourceNode* node) {
    if (guid_is_zero(external->guid)) return true;
    return node->has_asset_guid &&
           memcmp(external->guid, node->asset_guid, 16U) == 0;
}

static bool file_structure_valid(const SerializedFile* file) {
    return file && file->object_count >= 0 && file->external_count >= 0 &&
           (file->object_count == 0 || file->objects != NULL) &&
           (file->external_count == 0 || file->externals != NULL);
}

static const AssetObjectInfo* find_object(const SerializedFile* file,
                                          int64_t path_id) {
    if (!file_structure_valid(file)) return NULL;
    for (int index = 0; index < file->object_count; ++index) {
        if (file->objects[index].path_id == path_id) {
            return &file->objects[index];
        }
    }
    return NULL;
}

static void finish_target(UnityPPtrResolveResult* result,
                          const UnityPPtrResolverGraph* graph,
                          size_t target_index, int64_t path_id,
                          int32_t expected_class_id,
                          UnityPPtrResolveStatus success_status) {
    result->target_index = target_index;
    const AssetObjectInfo* object = find_object(
        graph->nodes[target_index].file, path_id);
    if (!object) {
        result->status = UNITY_PPTR_RESOLVE_TARGET_MISSING;
        return;
    }
    result->object = object;
    if (expected_class_id != UNITY_PPTR_TARGET_CLASS_ANY &&
        object->type_id != expected_class_id) {
        result->status = UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS;
        return;
    }
    result->status = success_status;
}

static bool has_archive_prefix(const char* value) {
    return value && strncmp(value, "archive:/", 9U) == 0;
}

static bool archive_locator(const AssetFileExternal* external,
                            char** locator) {
    if (!external || !locator) return false;
    *locator = NULL;
    const char* virtual_path = external->virtual_path;
    const char* path_name = external->path_name;
    bool virtual_archive = has_archive_prefix(virtual_path);
    bool path_archive = has_archive_prefix(path_name);
    if (!virtual_archive && !path_archive) return false;

    const char* virtual_payload = virtual_archive ? virtual_path + 9U : "";
    const char* path_payload = path_archive ? path_name + 9U : path_name;
    if (!path_payload) return false;

    size_t virtual_length = strlen(virtual_payload);
    size_t path_length = strlen(path_payload);
    if (virtual_length == 0U) {
        if (path_length == 0U || path_length > UNITY_PPTR_MAX_LOCATOR_BYTES) {
            return false;
        }
        *locator = (char*)malloc(path_length + 1U);
        if (!*locator) return false;
        memcpy(*locator, path_payload, path_length + 1U);
        return true;
    }
    if (path_archive) {
        /* Two complete archive URIs are only valid when they agree. */
        NormalizedPath left;
        NormalizedPath right;
        memset(&left, 0, sizeof(left));
        memset(&right, 0, sizeof(right));
        bool ok = normalize_path(virtual_payload, &left) &&
                  normalize_path(path_payload, &right) &&
                  normalized_equal(&left, &right);
        if (ok) {
            size_t length = strlen(left.text);
            *locator = (char*)malloc(length + 1U);
            if (*locator) memcpy(*locator, left.text, length + 1U);
            ok = *locator != NULL;
        }
        normalized_path_dispose(&left);
        normalized_path_dispose(&right);
        return ok;
    }

    if (path_length == 0U ||
        virtual_length > SIZE_MAX - path_length - 2U ||
        virtual_length + path_length + 1U >
            UNITY_PPTR_MAX_LOCATOR_BYTES) {
        return false;
    }
    size_t total = virtual_length + 1U + path_length;
    *locator = (char*)malloc(total + 1U);
    if (!*locator) return false;
    memcpy(*locator, virtual_payload, virtual_length);
    (*locator)[virtual_length] = '/';
    memcpy(*locator + virtual_length + 1U, path_payload,
           path_length + 1U);
    return true;
}

static bool normalized_node_scope(const UnityPPtrSourceNode* node,
                                  NormalizedPath* root,
                                  NormalizedPath* outer) {
    if (!node || !node->scope_root || !node->outer_path ||
        !normalize_path(node->scope_root, root)) {
        return false;
    }
    if (!normalize_path(node->outer_path, outer)) {
        normalized_path_dispose(root);
        return false;
    }
    if (!path_is_within(outer, root)) {
        normalized_path_dispose(root);
        normalized_path_dispose(outer);
        return false;
    }
    return true;
}

static bool node_shares_scope(const UnityPPtrSourceNode* node,
                              const NormalizedPath* source_root,
                              NormalizedPath* candidate_outer) {
    NormalizedPath candidate_root;
    if (!normalized_node_scope(node, &candidate_root, candidate_outer)) {
        return false;
    }
    bool same = normalized_equal(source_root, &candidate_root);
    normalized_path_dispose(&candidate_root);
    if (!same) normalized_path_dispose(candidate_outer);
    return same;
}

static BuiltinContract builtin_contract(
    const AssetFileExternal* external, const char** target_locator) {
    static const uint8_t extra_guid[16] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x0fU, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    };
    static const uint8_t default_resources_guid[16] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x0eU, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    };
    if (target_locator) *target_locator = NULL;
    if (!external || !external->path_name || !external->virtual_path ||
        external->virtual_path[0] != '\0') {
        return BUILTIN_CONTRACT_NONE;
    }

    if (strcmp(external->path_name,
               "Resources/unity_builtin_extra") == 0) {
        if (external->type != 0 ||
            memcmp(external->guid, extra_guid, 16U) != 0) {
            return BUILTIN_CONTRACT_INVALID;
        }
        if (target_locator) {
            *target_locator = "Resources/unity_builtin_extra";
        }
        return BUILTIN_CONTRACT_EXTRA;
    }
    if (strcmp(external->path_name,
               "Library/unity default resources") == 0) {
        if (external->type != 0 ||
            memcmp(external->guid, default_resources_guid, 16U) != 0) {
            return BUILTIN_CONTRACT_INVALID;
        }
        if (target_locator) {
            *target_locator = "Resources/unity default resources";
        }
        return BUILTIN_CONTRACT_DEFAULT_RESOURCES;
    }
    return BUILTIN_CONTRACT_NONE;
}

static bool resolve_archive(const UnityPPtrResolverGraph* graph,
                            size_t source_index,
                            const AssetFileExternal* external,
                            int64_t path_id, int32_t expected_class_id,
                            UnityPPtrResolveResult* result) {
    char* locator = NULL;
    if (!archive_locator(external, &locator)) {
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }
    const UnityPPtrSourceNode* source = &graph->nodes[source_index];
    if (!source->is_bundle_member || !source->outer_path ||
        !source->scope_root) {
        free(locator);
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }

    NormalizedPath wanted_member;
    NormalizedPath source_root;
    NormalizedPath source_outer;
    memset(&wanted_member, 0, sizeof(wanted_member));
    memset(&source_root, 0, sizeof(source_root));
    memset(&source_outer, 0, sizeof(source_outer));
    bool valid = normalize_path(locator, &wanted_member) &&
                 normalized_node_scope(source, &source_root, &source_outer) &&
                 !wanted_member.absolute;
    free(locator);
    if (!valid) {
        normalized_path_dispose(&wanted_member);
        normalized_path_dispose(&source_root);
        normalized_path_dispose(&source_outer);
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }

    size_t match = SIZE_MAX;
    size_t matches = 0U;
    for (size_t index = 0U; index < graph->node_count; ++index) {
        const UnityPPtrSourceNode* candidate = &graph->nodes[index];
        if (!candidate->is_bundle_member || !candidate->member_name ||
            !guid_matches_node(external, candidate)) {
            continue;
        }
        NormalizedPath candidate_outer;
        if (!node_shares_scope(candidate, &source_root, &candidate_outer)) {
            continue;
        }
        bool same_outer = normalized_equal(&candidate_outer, &source_outer);
        normalized_path_dispose(&candidate_outer);
        if (!same_outer) continue;

        NormalizedPath candidate_member;
        if (!normalize_path(candidate->member_name, &candidate_member)) {
            continue;
        }
        bool same_member = normalized_equal(&candidate_member, &wanted_member);
        normalized_path_dispose(&candidate_member);
        if (same_member) {
            match = index;
            ++matches;
        }
    }
    normalized_path_dispose(&wanted_member);
    normalized_path_dispose(&source_root);
    normalized_path_dispose(&source_outer);

    if (matches == 0U) {
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
    } else if (matches != 1U) {
        result->status = UNITY_PPTR_RESOLVE_AMBIGUOUS;
    } else {
        finish_target(result, graph, match, path_id, expected_class_id,
                      UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    }
    return true;
}

static bool resolve_loose(const UnityPPtrResolverGraph* graph,
                          size_t source_index,
                          const AssetFileExternal* external,
                          const char* locator, bool is_builtin,
                          int64_t path_id, int32_t expected_class_id,
                          UnityPPtrResolveResult* result) {
    const UnityPPtrSourceNode* source = &graph->nodes[source_index];
    NormalizedPath source_root;
    NormalizedPath source_outer;
    NormalizedPath source_directory;
    NormalizedPath normalized_locator;
    memset(&source_root, 0, sizeof(source_root));
    memset(&source_outer, 0, sizeof(source_outer));
    memset(&source_directory, 0, sizeof(source_directory));
    memset(&normalized_locator, 0, sizeof(normalized_locator));
    bool valid = normalized_node_scope(source, &source_root, &source_outer) &&
                 parent_path(&source_outer, &source_directory) &&
                 normalize_path(locator, &normalized_locator) &&
                 !normalized_locator.absolute &&
                 !(strlen(normalized_locator.text) >= 2U &&
                   normalized_locator.text[1] == ':');
    normalized_path_dispose(&source_outer);
    normalized_path_dispose(&normalized_locator);
    if (!valid) {
        normalized_path_dispose(&source_root);
        normalized_path_dispose(&source_directory);
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }

    while (path_is_within(&source_directory, &source_root)) {
        NormalizedPath wanted;
        memset(&wanted, 0, sizeof(wanted));
        if (!join_path(&source_directory, locator, &wanted)) {
            normalized_path_dispose(&source_root);
            normalized_path_dispose(&source_directory);
            result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
            return true;
        }
        if (path_is_within(&wanted, &source_root)) {
            size_t match = SIZE_MAX;
            size_t matches = 0U;
            for (size_t index = 0U; index < graph->node_count; ++index) {
                const UnityPPtrSourceNode* candidate = &graph->nodes[index];
                if (candidate->is_bundle_member ||
                    (!is_builtin && !guid_matches_node(external, candidate))) {
                    continue;
                }
                NormalizedPath candidate_outer;
                if (!node_shares_scope(candidate, &source_root,
                                       &candidate_outer)) {
                    continue;
                }
                bool same = normalized_equal(&candidate_outer, &wanted);
                normalized_path_dispose(&candidate_outer);
                if (same) {
                    match = index;
                    ++matches;
                }
            }
            normalized_path_dispose(&wanted);
            if (matches != 0U) {
                normalized_path_dispose(&source_root);
                normalized_path_dispose(&source_directory);
                if (matches != 1U) {
                    result->status = UNITY_PPTR_RESOLVE_AMBIGUOUS;
                } else {
                    finish_target(
                        result, graph, match, path_id, expected_class_id,
                        is_builtin
                            ? UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT
                            : UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
                }
                return true;
            }
        } else {
            normalized_path_dispose(&wanted);
        }

        if (normalized_equal(&source_directory, &source_root)) break;
        NormalizedPath parent;
        if (!parent_path(&source_directory, &parent) ||
            !path_is_within(&parent, &source_root)) {
            normalized_path_dispose(&parent);
            break;
        }
        normalized_path_dispose(&source_directory);
        source_directory = parent;
    }

    normalized_path_dispose(&source_root);
    normalized_path_dispose(&source_directory);
    result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
    return true;
}

void unity_pptr_resolve_result_init(UnityPPtrResolveResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
    result->source_index = SIZE_MAX;
    result->target_index = SIZE_MAX;
    result->external_index = SIZE_MAX;
}

bool unity_pptr_resolve(const UnityPPtrResolverGraph* graph,
                        size_t source_index, const AssetPPtr* pointer,
                        int32_t expected_target_class_id,
                        UnityPPtrResolveResult* result) {
    if (result) unity_pptr_resolve_result_init(result);
    if (!graph || !pointer || !result || !graph->nodes ||
        source_index >= graph->node_count ||
        !file_structure_valid(graph->nodes[source_index].file)) {
        return false;
    }
    result->source_index = source_index;

    if (pointer->path_id == 0) {
        result->status = UNITY_PPTR_RESOLVE_NULL;
        return true;
    }
    if (pointer->file_id < 0) {
        result->status = UNITY_PPTR_RESOLVE_INVALID_FILE_ID;
        return true;
    }
    if (pointer->file_id == 0) {
        finish_target(result, graph, source_index, pointer->path_id,
                      expected_target_class_id, UNITY_PPTR_RESOLVE_LOCAL);
        return true;
    }

    const SerializedFile* source_file = graph->nodes[source_index].file;
    if (pointer->file_id > source_file->external_count) {
        result->status = UNITY_PPTR_RESOLVE_INVALID_FILE_ID;
        return true;
    }
    size_t external_index = (size_t)(pointer->file_id - 1);
    const AssetFileExternal* external =
        &source_file->externals[external_index];
    result->external_index = external_index;
    result->external = external;
    if (!external->path_name || !external->virtual_path ||
        external->path_name[0] == '\0') {
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }

    if (has_archive_prefix(external->virtual_path) ||
        has_archive_prefix(external->path_name)) {
        return resolve_archive(graph, source_index, external,
                               pointer->path_id, expected_target_class_id,
                               result);
    }
    if (external->virtual_path[0] != '\0') {
        /* Ignoring a non-empty, non-archive virtual locator would discard
         * serialized identity evidence and reduce resolution to a basename. */
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }

    const char* builtin_target = NULL;
    BuiltinContract builtin = builtin_contract(external, &builtin_target);
    if (builtin == BUILTIN_CONTRACT_INVALID) {
        result->status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        return true;
    }
    if (builtin != BUILTIN_CONTRACT_NONE) {
        return resolve_loose(graph, source_index, external, builtin_target,
                             true, pointer->path_id,
                             expected_target_class_id, result);
    }
    return resolve_loose(graph, source_index, external, external->path_name,
                         false, pointer->path_id,
                         expected_target_class_id, result);
}

bool unity_pptr_resolve_status_is_success(UnityPPtrResolveStatus status) {
    return status == UNITY_PPTR_RESOLVE_LOCAL ||
           status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT ||
           status == UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT;
}

const char* unity_pptr_resolve_status_name(UnityPPtrResolveStatus status) {
    switch (status) {
        case UNITY_PPTR_RESOLVE_LOCAL: return "local";
        case UNITY_PPTR_RESOLVE_EXTERNAL_EXACT: return "external-exact";
        case UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT:
            return "external-builtin-contract";
        case UNITY_PPTR_RESOLVE_NULL: return "null";
        case UNITY_PPTR_RESOLVE_OUT_OF_SCOPE: return "out-of-scope";
        case UNITY_PPTR_RESOLVE_AMBIGUOUS: return "ambiguous";
        case UNITY_PPTR_RESOLVE_INVALID_FILE_ID: return "invalid-file-id";
        case UNITY_PPTR_RESOLVE_TARGET_MISSING: return "target-missing";
        case UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS:
            return "target-wrong-class";
        default: return "unknown";
    }
}
