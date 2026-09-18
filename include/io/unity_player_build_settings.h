// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_PLAYER_BUILD_SETTINGS_H
#define UNITY_PLAYER_BUILD_SETTINGS_H

#include "io/serialized_file.h"

#define UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID 141
#define UNITY_PLAYER_BUILD_SETTINGS_LAYOUT_AUTHORITY \
    "unity-2021.3.35f1-player-class141-575bf9902bd61f97e962a3a175b5c67a"

/*
 * Exact player BuildSettings (ClassID 141) recovery boundary.
 *
 * This decoder is deliberately independent of a TypeTree because player
 * globalgamemanagers normally disables it.  Admission is instead pinned to
 * SerializedFile v22, Unity 2021.3.35f1, the exact ClassID 141 serialized
 * type hash, and the complete wire order recovered from that engine's
 * BuildSettings::Transfer implementation.  The complete object payload must
 * be consumed.
 *
 * m_GraphicsAPIs is serialized build metadata.  It does not encode the
 * UnityShaderCompiler validAPIs mask or either platform-capability word, and
 * callers must not derive those compiler-request fields from this result.
 */

typedef enum {
    UNITY_PLAYER_BUILD_SETTINGS_OK = 0,
    UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT,
    UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE,
    UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_FILE_VERSION,
    UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_UNITY_VERSION,
    UNITY_PLAYER_BUILD_SETTINGS_MISSING,
    UNITY_PLAYER_BUILD_SETTINGS_DUPLICATE,
    UNITY_PLAYER_BUILD_SETTINGS_OBJECT_RANGE_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_TYPE_INDEX_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_TYPE_RECORD_MISMATCH,
    UNITY_PLAYER_BUILD_SETTINGS_TYPE_IDENTITY_UNSUPPORTED,
    UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED,
    UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_STRING_LENGTH_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED,
    UNITY_PLAYER_BUILD_SETTINGS_STRING_CONTAINS_NUL,
    UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_ALIGNMENT_INVALID,
    UNITY_PLAYER_BUILD_SETTINGS_VERSION_MISMATCH,
    UNITY_PLAYER_BUILD_SETTINGS_ALLOCATION_FAILED,
    UNITY_PLAYER_BUILD_SETTINGS_OBJECT_BYTES_NOT_EXHAUSTED,
} UnityPlayerBuildSettingsStatus;

typedef struct {
    bool has_pro_version;
    bool is_no_watermark_build;
    bool is_prototyping_build;
    bool is_educational_build;
    bool is_embedded;
    bool is_trial;
    bool has_publishing_rights;
    bool has_shadows;
    bool has_soft_shadows;
    bool has_local_light_shadows;
    bool has_advanced_version;
    bool enable_dynamic_batching;
    bool is_debug_build;
    bool uses_on_mouse_events;
    bool has_cluster_rendering;
} UnityPlayerBuildSettingsFlags;

typedef struct {
    uint32_t serialized_file_version;
    uint32_t target_platform;
    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;

    uint32_t scene_count;
    uint32_t preloaded_plugin_count;
    uint32_t enabled_vr_device_count;
    uint32_t build_tag_count;
    uint8_t build_guid[16];
    UnityPlayerBuildSettingsFlags flags;

    char* unity_version;
    int32_t* graphics_apis;
    size_t graphics_api_count;
    bool decoded;
} UnityPlayerBuildSettings;

void unity_player_build_settings_init(UnityPlayerBuildSettings* settings);
void unity_player_build_settings_dispose(UnityPlayerBuildSettings* settings);

/*
 * Strong output guarantee: destination must be initialized. Failure leaves
 * it unchanged; success replaces it with an owning result.
 */
UnityPlayerBuildSettingsStatus unity_player_build_settings_decode(
    UnityPlayerBuildSettings* destination, const SerializedFile* file);

/* Opens a complete standalone SerializedFile v22 byte range in metadata-only
 * mode, then applies the exact ClassID 141 decoder above. */
UnityPlayerBuildSettingsStatus
unity_player_build_settings_decode_serialized_bytes(
    UnityPlayerBuildSettings* destination,
    const uint8_t* bytes, size_t size);

const char* unity_player_build_settings_status_name(
    UnityPlayerBuildSettingsStatus status);

#endif /* UNITY_PLAYER_BUILD_SETTINGS_H */
