#include "io/unity_player_build_settings.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t payload[512];
    size_t size;
    bool big_endian;
    size_t first_string_data_offset;
    size_t first_alignment_offset;
    size_t first_boolean_offset;
    size_t version_data_offset;
    size_t graphics_count_offset;
    TypeTreeType type;
    AssetObjectInfo objects[2];
    SerializedFile file;
} SyntheticBuildSettings;

static const uint8_t k_expected_type_hash[16] = {
    0x57U, 0x5bU, 0xf9U, 0x90U, 0x2bU, 0xd6U, 0x1fU, 0x97U,
    0xe9U, 0x62U, 0xa3U, 0xa1U, 0x75U, 0xb5U, 0xc6U, 0x7aU,
};

static void append_u8(SyntheticBuildSettings* fixture, uint8_t value) {
    fixture->payload[fixture->size++] = value;
}

static void append_i32(SyntheticBuildSettings* fixture, int32_t value) {
    const uint32_t bits = (uint32_t)value;
    if (fixture->big_endian) {
        append_u8(fixture, (uint8_t)(bits >> 24U));
        append_u8(fixture, (uint8_t)(bits >> 16U));
        append_u8(fixture, (uint8_t)(bits >> 8U));
        append_u8(fixture, (uint8_t)bits);
    } else {
        append_u8(fixture, (uint8_t)bits);
        append_u8(fixture, (uint8_t)(bits >> 8U));
        append_u8(fixture, (uint8_t)(bits >> 16U));
        append_u8(fixture, (uint8_t)(bits >> 24U));
    }
}

static void store_i32(SyntheticBuildSettings* fixture, size_t offset,
                      int32_t value) {
    const size_t saved_size = fixture->size;
    fixture->size = offset;
    append_i32(fixture, value);
    fixture->size = saved_size;
}

static size_t append_alignment(SyntheticBuildSettings* fixture) {
    const size_t first_padding = fixture->size;
    while ((fixture->size & 3U) != 0U) append_u8(fixture, 0U);
    return first_padding;
}

static void append_string(SyntheticBuildSettings* fixture,
                          const char* value, size_t* out_data_offset,
                          size_t* out_alignment_offset) {
    const size_t size = strlen(value);
    append_i32(fixture, (int32_t)size);
    if (out_data_offset) *out_data_offset = fixture->size;
    memcpy(fixture->payload + fixture->size, value, size);
    fixture->size += size;
    const size_t alignment = append_alignment(fixture);
    if (out_alignment_offset) *out_alignment_offset = alignment;
}

static void append_string_array(SyntheticBuildSettings* fixture,
                                const char* value,
                                size_t* out_data_offset,
                                size_t* out_alignment_offset) {
    append_i32(fixture, value ? 1 : 0);
    if (value) {
        append_string(fixture, value, out_data_offset,
                      out_alignment_offset);
    }
}

static void make_fixture(SyntheticBuildSettings* fixture,
                         bool big_endian) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->big_endian = big_endian;

    append_string_array(fixture, "Scene",
                        &fixture->first_string_data_offset,
                        &fixture->first_alignment_offset);
    append_string_array(fixture, "Plugin", NULL, NULL);
    append_string_array(fixture, NULL, NULL, NULL);
    append_string_array(fixture, "tag", NULL, NULL);
    for (uint8_t value = 0U; value < 16U; ++value) {
        append_u8(fixture, value);
    }

    fixture->first_boolean_offset = fixture->size;
    static const uint8_t flags[15] = {
        1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U,
        1U, 0U, 1U, 0U, 1U, 0U, 1U,
    };
    memcpy(fixture->payload + fixture->size, flags, sizeof(flags));
    fixture->size += sizeof(flags);
    (void)append_alignment(fixture);

    append_i32(fixture, 11);
    fixture->version_data_offset = fixture->size;
    memcpy(fixture->payload + fixture->size, "2021.3.35f1", 11U);
    fixture->size += 11U;
    (void)append_alignment(fixture);

    fixture->graphics_count_offset = fixture->size;
    append_i32(fixture, 2);
    append_i32(fixture, 2);
    append_i32(fixture, 17);
    (void)append_alignment(fixture);

    fixture->type.type_id = UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID;
    fixture->type.script_type_index = UINT16_MAX;
    memcpy(fixture->type.type_hash, k_expected_type_hash,
           sizeof(k_expected_type_hash));

    fixture->objects[0].path_id = 11;
    fixture->objects[0].byte_offset = 0U;
    fixture->objects[0].byte_size = (uint32_t)fixture->size;
    fixture->objects[0].type_id_or_index = 0;
    fixture->objects[0].type_id = UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID;
    fixture->objects[0].script_type_index = UINT16_MAX;

    fixture->file.version = 22U;
    fixture->file.file_size = fixture->size;
    fixture->file.data_offset = 0U;
    fixture->file.big_endian = big_endian;
    fixture->file.unity_version = (char*)"2021.3.35f1";
    fixture->file.target_platform = 19U;
    fixture->file.type_count = 1;
    fixture->file.types = &fixture->type;
    fixture->file.object_count = 1;
    fixture->file.objects = fixture->objects;
    fixture->file.raw_data = fixture->payload;
    fixture->file.raw_size = fixture->size;
}

static int check_valid_decode(bool big_endian) {
    SyntheticBuildSettings fixture;
    make_fixture(&fixture, big_endian);
    UnityPlayerBuildSettings result;
    unity_player_build_settings_init(&result);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_OK);
    CHECK(result.decoded);
    CHECK(result.serialized_file_version == 22U);
    CHECK(result.target_platform == 19U);
    CHECK(result.path_id == 11);
    CHECK(result.byte_offset == 0U);
    CHECK(result.byte_size == fixture.size);
    CHECK(result.scene_count == 1U);
    CHECK(result.preloaded_plugin_count == 1U);
    CHECK(result.enabled_vr_device_count == 0U);
    CHECK(result.build_tag_count == 1U);
    for (uint8_t value = 0U; value < 16U; ++value) {
        CHECK(result.build_guid[value] == value);
    }
    CHECK(result.flags.has_pro_version);
    CHECK(!result.flags.is_no_watermark_build);
    CHECK(result.flags.is_prototyping_build);
    CHECK(!result.flags.is_educational_build);
    CHECK(result.flags.is_embedded);
    CHECK(!result.flags.is_trial);
    CHECK(result.flags.has_publishing_rights);
    CHECK(!result.flags.has_shadows);
    CHECK(result.flags.has_soft_shadows);
    CHECK(!result.flags.has_local_light_shadows);
    CHECK(result.flags.has_advanced_version);
    CHECK(!result.flags.enable_dynamic_batching);
    CHECK(result.flags.is_debug_build);
    CHECK(!result.flags.uses_on_mouse_events);
    CHECK(result.flags.has_cluster_rendering);
    CHECK(result.unity_version != NULL);
    CHECK(strcmp(result.unity_version, "2021.3.35f1") == 0);
    CHECK(result.graphics_api_count == 2U);
    CHECK(result.graphics_apis != NULL);
    CHECK(result.graphics_apis[0] == 2);
    CHECK(result.graphics_apis[1] == 17);
    unity_player_build_settings_dispose(&result);
    return 0;
}

int main(void) {
    const size_t initial_allocations = atomic_load_explicit(
        &g_allocations_count, memory_order_relaxed);
    const size_t initial_bytes = atomic_load_explicit(
        &g_allocated_bytes, memory_order_relaxed);
    CHECK(check_valid_decode(false) == 0);
    CHECK(check_valid_decode(true) == 0);

    SyntheticBuildSettings fixture;
    make_fixture(&fixture, false);
    UnityPlayerBuildSettings result;
    unity_player_build_settings_init(&result);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_OK);
    char* retained_version = result.unity_version;

    fixture.file.object_count = 0;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_MISSING);
    CHECK(result.unity_version == retained_version);
    CHECK(strcmp(result.unity_version, "2021.3.35f1") == 0);
    fixture.file.object_count = 1;

    fixture.objects[1] = fixture.objects[0];
    fixture.objects[1].path_id = 12;
    fixture.file.object_count = 2;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_DUPLICATE);
    fixture.file.object_count = 1;

    fixture.file.version = 21U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_FILE_VERSION);
    fixture.file.version = 22U;
    fixture.file.unity_version = (char*)"2021.3.34f1";
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_UNITY_VERSION);
    fixture.file.unity_version = (char*)"2021.3.35f1";

    fixture.objects[0].byte_offset = fixture.size + 1U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_OBJECT_RANGE_INVALID);
    fixture.objects[0].byte_offset = 0U;
    fixture.objects[0].type_id_or_index = 1;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_TYPE_INDEX_INVALID);
    fixture.objects[0].type_id_or_index = 0;

    fixture.type.type_id = 48;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_TYPE_RECORD_MISMATCH);
    fixture.type.type_id = UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID;
    fixture.type.type_hash[0] ^= 1U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_TYPE_IDENTITY_UNSUPPORTED);
    fixture.type.type_hash[0] ^= 1U;

    store_i32(&fixture, 0U, -1);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID);
    store_i32(&fixture, 0U, 1);

    store_i32(&fixture, 4U, -1);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_STRING_LENGTH_INVALID);
    store_i32(&fixture, 4U, INT32_MAX);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED);
    store_i32(&fixture, 4U, 5);

    fixture.payload[fixture.first_string_data_offset + 1U] = 0U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_STRING_CONTAINS_NUL);
    fixture.payload[fixture.first_string_data_offset + 1U] = (uint8_t)'c';

    fixture.payload[fixture.first_alignment_offset] = 1U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_ALIGNMENT_INVALID);
    fixture.payload[fixture.first_alignment_offset] = 0U;

    fixture.payload[fixture.first_boolean_offset] = 2U;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_INVALID);
    fixture.payload[fixture.first_boolean_offset] = 1U;

    fixture.payload[fixture.version_data_offset] = (uint8_t)'3';
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_VERSION_MISMATCH);
    fixture.payload[fixture.version_data_offset] = (uint8_t)'2';

    store_i32(&fixture, fixture.graphics_count_offset, -1);
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID);
    store_i32(&fixture, fixture.graphics_count_offset, 2);

    fixture.payload[fixture.size] = 0U;
    ++fixture.size;
    fixture.file.file_size = fixture.size;
    fixture.file.raw_size = fixture.size;
    fixture.objects[0].byte_size = (uint32_t)fixture.size;
    CHECK(unity_player_build_settings_decode(&result, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_OBJECT_BYTES_NOT_EXHAUSTED);
    --fixture.size;
    fixture.file.file_size = fixture.size;
    fixture.file.raw_size = fixture.size;
    fixture.objects[0].byte_size = (uint32_t)fixture.size;

    CHECK(unity_player_build_settings_decode(NULL, &fixture.file) ==
          UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT);
    CHECK(unity_player_build_settings_decode_serialized_bytes(
              &result, fixture.payload, fixture.size) ==
          UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE);
    CHECK(strcmp(unity_player_build_settings_status_name(
                     UNITY_PLAYER_BUILD_SETTINGS_DUPLICATE),
                 "build-settings-duplicate") == 0);
    CHECK(strcmp(unity_player_build_settings_status_name(
                     UNITY_PLAYER_BUILD_SETTINGS_OBJECT_BYTES_NOT_EXHAUSTED),
                 "object-bytes-not-exhausted") == 0);

    unity_player_build_settings_dispose(&result);
    CHECK(atomic_load_explicit(&g_allocations_count,
                               memory_order_relaxed) ==
          initial_allocations);
    CHECK(atomic_load_explicit(&g_allocated_bytes,
                               memory_order_relaxed) == initial_bytes);
    puts("UnityCommon player BuildSettings unit tests passed.");
    return 0;
}
