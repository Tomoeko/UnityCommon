#include "io/unity_pptr_resolver.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    SerializedFile file;
    AssetObjectInfo objects[4];
    AssetFileExternal externals[4];
    UnityPPtrSourceNode node;
} SourceFixture;

static void fixture_init(SourceFixture* fixture, const char* outer_path,
                         const char* member_name, bool is_bundle_member,
                         const char* scope_root) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->file.objects = fixture->objects;
    fixture->file.externals = fixture->externals;
    fixture->node.file = &fixture->file;
    fixture->node.outer_path = outer_path;
    fixture->node.member_name = member_name;
    fixture->node.is_bundle_member = is_bundle_member;
    fixture->node.scope_root = scope_root;
}

static void fixture_object(SourceFixture* fixture, size_t index,
                           int64_t path_id, int32_t class_id) {
    CHECK(index < 4U);
    fixture->objects[index].path_id = path_id;
    fixture->objects[index].type_id = class_id;
    if ((int)(index + 1U) > fixture->file.object_count) {
        fixture->file.object_count = (int)(index + 1U);
    }
}

static void fixture_external(SourceFixture* fixture, size_t index,
                             const char* virtual_path,
                             const char* path_name) {
    CHECK(index < 4U);
    fixture->externals[index].virtual_path = (char*)virtual_path;
    fixture->externals[index].path_name = (char*)path_name;
    if ((int)(index + 1U) > fixture->file.external_count) {
        fixture->file.external_count = (int)(index + 1U);
    }
}

static UnityPPtrResolveResult resolve(const UnityPPtrSourceNode* nodes,
                                     size_t node_count, size_t source_index,
                                     int32_t file_id, int64_t path_id,
                                     int32_t expected_class) {
    UnityPPtrResolverGraph graph = {nodes, node_count};
    AssetPPtr pointer = {file_id, path_id};
    UnityPPtrResolveResult result;
    bool ok = unity_pptr_resolve(&graph, source_index, &pointer,
                                 expected_class, &result);
    CHECK(ok);
    return result;
}

static void test_local_null_and_file_id_contract(void) {
    SourceFixture source;
    fixture_init(&source, "/game/Main_Data/sharedassets0.assets", NULL,
                 false, "/game");
    fixture_object(&source, 0U, 17, 48);
    fixture_external(&source, 0U, "", "sharedassets1.assets");

    UnityPPtrResolveResult result = resolve(
        &source.node, 1U, 0U, 0, 17, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_LOCAL);
    CHECK(result.source_index == 0U && result.target_index == 0U);
    CHECK(result.external_index == SIZE_MAX && result.external == NULL);
    CHECK(result.object == &source.objects[0]);
    CHECK(unity_pptr_resolve_status_is_success(result.status));

    result = resolve(&source.node, 1U, 0U, 0, 99, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_TARGET_MISSING);
    CHECK(result.target_index == 0U && result.object == NULL);

    result = resolve(&source.node, 1U, 0U, 0, 17, 21);
    CHECK(result.status == UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS);
    CHECK(result.object == &source.objects[0]);

    /* PathID zero is Unity's null representation even if FileID is garbage. */
    result = resolve(&source.node, 1U, 0U, 200, 0, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_NULL);
    CHECK(result.target_index == SIZE_MAX);

    result = resolve(&source.node, 1U, 0U, -1, 17, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_INVALID_FILE_ID);
    result = resolve(&source.node, 1U, 0U, 2, 17, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_INVALID_FILE_ID);
}

static void test_external_exact_and_expected_class(void) {
    SourceFixture source;
    SourceFixture target;
    fixture_init(&source, "/game/Main_Data/levels/level0.assets", NULL,
                 false, "/game");
    fixture_init(&target,
                 "/game/Main_Data/levels/sharedassets0.assets", NULL,
                 false, "/game");
    fixture_external(&source, 0U, "", "sharedassets0.assets");
    fixture_object(&target, 0U, 40, 48);
    UnityPPtrSourceNode nodes[] = {source.node, target.node};

    UnityPPtrResolveResult result = resolve(nodes, 2U, 0U, 1, 40, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 1U);
    CHECK(result.external_index == 0U);
    CHECK(result.external == &source.externals[0]);
    CHECK(result.object == &target.objects[0]);

    result = resolve(nodes, 2U, 0U, 1, 41, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_TARGET_MISSING);
    CHECK(result.target_index == 1U);
    result = resolve(nodes, 2U, 0U, 1, 40, 21);
    CHECK(result.status == UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS);

    source.externals[0].virtual_path = "not-an-archive";
    nodes[0] = source.node;
    result = resolve(nodes, 2U, 0U, 1, 40, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);
}

static void test_guid_authority(void) {
    SourceFixture source;
    SourceFixture target;
    fixture_init(&source, "/game/Main_Data/source.assets", NULL, false,
                 "/game");
    fixture_init(&target, "/game/Main_Data/target.assets", NULL, false,
                 "/game");
    fixture_external(&source, 0U, "", "target.assets");
    source.externals[0].guid[3] = 0x42U;
    fixture_object(&target, 0U, 9, 48);
    UnityPPtrSourceNode nodes[] = {source.node, target.node};

    UnityPPtrResolveResult result = resolve(nodes, 2U, 0U, 1, 9, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);

    nodes[1].has_asset_guid = true;
    nodes[1].asset_guid[3] = 0x41U;
    result = resolve(nodes, 2U, 0U, 1, 9, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);

    nodes[1].asset_guid[3] = 0x42U;
    result = resolve(nodes, 2U, 0U, 1, 9, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
}

static void test_nearest_depth_and_ambiguity(void) {
    SourceFixture source;
    SourceFixture near;
    SourceFixture far;
    fixture_init(&source, "/game/Main_Data/level/level0.assets", NULL,
                 false, "/game");
    fixture_init(&near,
                 "/game/Main_Data/level/sharedassets0.assets", NULL,
                 false, "/game");
    fixture_init(&far, "/game/Main_Data/sharedassets0.assets", NULL,
                 false, "/game");
    fixture_external(&source, 0U, "", "sharedassets0.assets");
    fixture_object(&near, 0U, 1, 48);
    fixture_object(&far, 0U, 1, 21);
    UnityPPtrSourceNode nodes[] = {source.node, far.node, near.node};

    UnityPPtrResolveResult result = resolve(nodes, 3U, 0U, 1, 1, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 2U);

    UnityPPtrSourceNode duplicate_nodes[] = {
        source.node, near.node, near.node, far.node,
    };
    result = resolve(duplicate_nodes, 4U, 0U, 1, 1, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_AMBIGUOUS);
    CHECK(result.target_index == SIZE_MAX);
}

static void test_normalized_locator_spelling(void) {
#ifdef _WIN32
    SourceFixture source;
    SourceFixture target;
    fixture_init(&source, "c:\\game\\Data\\levels\\.\\level.assets",
                 NULL, false, "C:/game//");
    fixture_init(&target, "C:/game/Data/sharedassets0.assets", NULL,
                 false, "c:\\game");
    fixture_external(&source, 0U, "", "..\\sharedassets0.assets");
    fixture_object(&target, 0U, 32, 48);
    UnityPPtrSourceNode nodes[] = {source.node, target.node};

    UnityPPtrResolveResult result = resolve(nodes, 2U, 0U, 1, 32, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 1U);
#else
    /* Backslash is a literal filename byte on POSIX. The source's parent is
     * /game, not /game/container, so the external resolves beside it. */
    SourceFixture source;
    SourceFixture target;
    fixture_init(&source, "/game/container\\source.assets", NULL, false,
                 "/game");
    fixture_init(&target, "/game/target.assets", NULL, false, "/game");
    fixture_external(&source, 0U, "", "target.assets");
    fixture_object(&target, 0U, 32, 48);
    UnityPPtrSourceNode nodes[] = {source.node, target.node};

    UnityPPtrResolveResult result = resolve(nodes, 2U, 0U, 1, 32, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 1U);
#endif
}

static void test_parallel_install_same_basename_isolation(void) {
    SourceFixture main_source;
    SourceFixture voice_source;
    SourceFixture main_target;
    SourceFixture voice_target;
    fixture_init(&main_source,
                 "/install/MainGame_Data/sharedassets3.assets", NULL,
                 false, "/install");
    fixture_init(&voice_source,
                 "/install/Mod Tools/ModTools_Data/"
                 "sharedassets3.assets", NULL, false, "/install");
    fixture_init(&main_target,
                 "/install/MainGame_Data/sharedassets0.assets", NULL,
                 false, "/install");
    fixture_init(&voice_target,
                 "/install/Mod Tools/ModTools_Data/"
                 "sharedassets0.assets", NULL, false, "/install");
    fixture_external(&main_source, 0U, "", "sharedassets0.assets");
    fixture_external(&voice_source, 0U, "", "sharedassets0.assets");
    fixture_object(&main_target, 0U, 90, 48);
    fixture_object(&voice_target, 0U, 90, 21);
    UnityPPtrSourceNode nodes[] = {
        main_source.node, voice_source.node,
        main_target.node, voice_target.node,
    };

    UnityPPtrResolveResult result = resolve(nodes, 4U, 0U, 1, 90, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 2U);

    result = resolve(nodes, 4U, 1U, 1, 90, 21);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 3U);
}

static void test_scope_boundary(void) {
    SourceFixture source;
    SourceFixture outside;
    SourceFixture different_scope;
    fixture_init(&source, "/game/Main_Data/source.assets", NULL, false,
                 "/game");
    fixture_init(&outside, "/other/target.assets", NULL, false, "/other");
    fixture_init(&different_scope, "/game/Main_Data/target.assets", NULL,
                 false, "/game/Main_Data");
    fixture_external(&source, 0U, "", "../../other/target.assets");
    fixture_object(&outside, 0U, 2, 48);
    fixture_object(&different_scope, 0U, 2, 48);
    UnityPPtrSourceNode nodes[] = {
        source.node, outside.node, different_scope.node,
    };

    UnityPPtrResolveResult result = resolve(nodes, 3U, 0U, 1, 2, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);

    source.externals[0].path_name = "target.assets";
    nodes[0] = source.node;
    result = resolve(nodes, 3U, 0U, 1, 2, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);
}

static void test_filesystem_root_scope(void) {
    SourceFixture source;
    SourceFixture target;
#ifdef _WIN32
    fixture_init(&source, "C:\\game\\source.assets", NULL, false,
                 "C:\\");
    fixture_init(&target, "C:\\target.assets", NULL, false, "C:\\");
    fixture_external(&source, 0U, "", "..\\target.assets");
#else
    fixture_init(&source, "/game/source.assets", NULL, false, "/");
    fixture_init(&target, "/target.assets", NULL, false, "/");
    fixture_external(&source, 0U, "", "../target.assets");
#endif
    fixture_object(&target, 0U, 23, 48);
    UnityPPtrSourceNode nodes[] = {source.node, target.node};
    UnityPPtrResolveResult result = resolve(nodes, 2U, 0U, 1, 23, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 1U);
}

static void test_same_bundle_archive_member(void) {
    SourceFixture source;
    SourceFixture target;
    SourceFixture other_bundle;
    fixture_init(&source, "/game/bundles/world.bundle", "CAB-source", true,
                 "/game");
    fixture_init(&target, "/game/bundles/world.bundle", "CAB-target", true,
                 "/game");
    fixture_init(&other_bundle, "/game/bundles/other.bundle", "CAB-target",
                 true, "/game");
    fixture_external(&source, 0U, "archive:/", "CAB-target");
    fixture_object(&target, 0U, 77, 48);
    fixture_object(&other_bundle, 0U, 77, 21);
    UnityPPtrSourceNode nodes[] = {
        source.node, other_bundle.node, target.node,
    };

    UnityPPtrResolveResult result = resolve(nodes, 3U, 0U, 1, 77, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(result.target_index == 2U);

    UnityPPtrSourceNode duplicates[] = {
        source.node, target.node, target.node,
    };
    result = resolve(duplicates, 3U, 0U, 1, 77, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_AMBIGUOUS);

    nodes[0].is_bundle_member = false;
    result = resolve(nodes, 3U, 0U, 1, 77, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);
}

static void test_builtin_contracts(void) {
    SourceFixture source;
    SourceFixture extra;
    SourceFixture defaults;
    fixture_init(&source, "/game/MainGame_Data/sharedassets3.assets",
                 NULL, false, "/game");
    fixture_init(&extra,
                 "/game/MainGame_Data/Resources/unity_builtin_extra",
                 NULL, false, "/game");
    fixture_init(&defaults,
                 "/game/MainGame_Data/Resources/"
                 "unity default resources", NULL, false, "/game");
    fixture_object(&extra, 0U, 103, 48);
    fixture_object(&defaults, 0U, 104, 48);
    fixture_external(&source, 0U, "", "Resources/unity_builtin_extra");
    source.externals[0].guid[8] = 0x0fU;
    fixture_external(&source, 1U, "", "Library/unity default resources");
    source.externals[1].guid[8] = 0x0eU;
    UnityPPtrSourceNode nodes[] = {source.node, extra.node, defaults.node};

    UnityPPtrResolveResult result = resolve(nodes, 3U, 0U, 1, 103, 48);
    CHECK(result.status ==
          UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT);
    CHECK(result.target_index == 1U);

    result = resolve(nodes, 3U, 0U, 2, 104, 48);
    CHECK(result.status ==
          UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT);
    CHECK(result.target_index == 2U);

    source.externals[0].guid[8] = 0x0eU;
    nodes[0] = source.node;
    result = resolve(nodes, 3U, 0U, 1, 103, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);

    source.externals[0].guid[8] = 0x0fU;
    source.externals[0].type = 1;
    nodes[0] = source.node;
    result = resolve(nodes, 3U, 0U, 1, 103, 48);
    CHECK(result.status == UNITY_PPTR_RESOLVE_OUT_OF_SCOPE);
}

static void test_api_and_status_names(void) {
    UnityPPtrResolveResult result;
    AssetPPtr pointer = {0, 1};
    CHECK(!unity_pptr_resolve(NULL, 0U, &pointer, 48, &result));
    CHECK(result.source_index == SIZE_MAX);
    CHECK(strcmp(unity_pptr_resolve_status_name(
                     UNITY_PPTR_RESOLVE_EXTERNAL_BUILTIN_CONTRACT),
                 "external-builtin-contract") == 0);
    CHECK(strcmp(unity_pptr_resolve_status_name(
                     UNITY_PPTR_RESOLVE_TARGET_WRONG_CLASS),
                 "target-wrong-class") == 0);
    CHECK(strcmp(unity_pptr_resolve_status_name(
                     (UnityPPtrResolveStatus)999),
                 "unknown") == 0);
    CHECK(!unity_pptr_resolve_status_is_success(
        UNITY_PPTR_RESOLVE_TARGET_MISSING));
}

int main(void) {
    test_local_null_and_file_id_contract();
    test_external_exact_and_expected_class();
    test_guid_authority();
    test_nearest_depth_and_ambiguity();
    test_normalized_locator_spelling();
    test_parallel_install_same_basename_isolation();
    test_scope_boundary();
    test_filesystem_root_scope();
    test_same_bundle_archive_member();
    test_builtin_contracts();
    test_api_and_status_names();
    if (failures != 0) {
        fprintf(stderr, "%d unity PPtr resolver check(s) failed\n", failures);
        return 1;
    }
    puts("UnityCommon PPtr resolver unit tests passed");
    return 0;
}
