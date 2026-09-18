// SPDX-License-Identifier: GPL-3.0-only

#include "io/unity_player_build_settings.h"

#include "common/stream.h"

#include <limits.h>
#include <string.h>

enum {
    UNITY_PLAYER_BUILD_SETTINGS_SERIALIZED_FILE_VERSION = 22,
    UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_COUNT = 15,
};

static const char k_supported_unity_version[] = "2021.3.35f1";
static const uint8_t k_build_settings_type_hash[16] = {
    0x57U, 0x5bU, 0xf9U, 0x90U, 0x2bU, 0xd6U, 0x1fU, 0x97U,
    0xe9U, 0x62U, 0xa3U, 0xa1U, 0x75U, 0xb5U, 0xc6U, 0x7aU,
};

typedef struct {
    ByteStream stream;
    UnityPlayerBuildSettingsStatus status;
} BuildSettingsReader;

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) return false;
    }
    return true;
}

static bool source_file_is_structurally_valid(const SerializedFile* file) {
    return file && file->raw_data && file->unity_version &&
        file->file_size == (uint64_t)file->raw_size &&
        file->data_offset <= file->file_size && file->type_count >= 0 &&
        file->object_count >= 0 &&
        (file->type_count == 0 || file->types) &&
        (file->object_count == 0 || file->objects);
}

static bool object_range_is_valid(const SerializedFile* file,
                                  const AssetObjectInfo* object) {
    const uint64_t data_size = file->file_size - file->data_offset;
    return object->byte_offset <= data_size &&
        (uint64_t)object->byte_size <= data_size - object->byte_offset;
}

static void reader_fail(BuildSettingsReader* reader,
                        UnityPlayerBuildSettingsStatus status) {
    if (reader->status == UNITY_PLAYER_BUILD_SETTINGS_OK) {
        reader->status = status;
    }
}

static bool reader_i32(BuildSettingsReader* reader, int32_t* value) {
    if (reader->status != UNITY_PLAYER_BUILD_SETTINGS_OK) return false;
    if (!stream_read_int32(&reader->stream, value)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_bytes(BuildSettingsReader* reader, uint8_t* destination,
                         size_t size) {
    if (reader->status != UNITY_PLAYER_BUILD_SETTINGS_OK) return false;
    if (!stream_read_bytes(&reader->stream, destination, size)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_align4_zero(BuildSettingsReader* reader) {
    if (reader->status != UNITY_PLAYER_BUILD_SETTINGS_OK) return false;
    const size_t remainder = reader->stream.position & 3U;
    const size_t padding = remainder == 0U ? 0U : 4U - remainder;
    if (padding > stream_remaining(&reader->stream)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED);
        return false;
    }
    if (!bytes_are_zero(reader->stream.data + reader->stream.position,
                        padding)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_ALIGNMENT_INVALID);
        return false;
    }
    return stream_skip(&reader->stream, padding);
}

static bool reader_string_view(BuildSettingsReader* reader,
                               const uint8_t** bytes, size_t* size) {
    *bytes = NULL;
    *size = 0U;
    int32_t signed_size = 0;
    if (!reader_i32(reader, &signed_size)) {
        if (reader->status ==
            UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED) {
            reader->status =
                UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED;
        }
        return false;
    }
    if (signed_size < 0) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_STRING_LENGTH_INVALID);
        return false;
    }
    const size_t string_size = (size_t)signed_size;
    if (string_size > stream_remaining(&reader->stream)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED);
        return false;
    }
    const uint8_t* string_bytes =
        reader->stream.data + reader->stream.position;
    if (memchr(string_bytes, 0, string_size)) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_STRING_CONTAINS_NUL);
        return false;
    }
    if (!stream_skip(&reader->stream, string_size) ||
        !reader_align4_zero(reader)) {
        if (reader->status ==
            UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED) {
            reader->status =
                UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED;
        }
        return false;
    }
    *bytes = string_bytes;
    *size = string_size;
    return true;
}

static bool reader_string_array(BuildSettingsReader* reader,
                                uint32_t* out_count) {
    *out_count = 0U;
    int32_t count = 0;
    if (!reader_i32(reader, &count)) return false;
    /* Each string requires at least its signed 32-bit byte count. */
    if (count < 0 ||
        (size_t)count > stream_remaining(&reader->stream) / 4U) {
        reader_fail(reader, UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID);
        return false;
    }
    for (int32_t index = 0; index < count; ++index) {
        const uint8_t* ignored_bytes = NULL;
        size_t ignored_size = 0U;
        if (!reader_string_view(reader, &ignored_bytes, &ignored_size)) {
            return false;
        }
    }
    *out_count = (uint32_t)count;
    return true;
}

static bool reader_boolean(BuildSettingsReader* reader, bool* value) {
    uint8_t serialized = 0U;
    if (!reader_bytes(reader, &serialized, sizeof(serialized))) return false;
    if (serialized > 1U) {
        reader_fail(reader,
                    UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_INVALID);
        return false;
    }
    *value = serialized != 0U;
    return true;
}

static UnityPlayerBuildSettingsStatus validate_type_identity(
    const SerializedFile* file, const AssetObjectInfo* object) {
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return UNITY_PLAYER_BUILD_SETTINGS_TYPE_INDEX_INVALID;
    }
    const TypeTreeType* type = &file->types[object->type_id_or_index];
    if (type->type_id != UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID ||
        type->is_ref_type ||
        type->script_type_index != object->script_type_index) {
        return UNITY_PLAYER_BUILD_SETTINGS_TYPE_RECORD_MISMATCH;
    }
    if (type->is_stripped || type->script_type_index != UINT16_MAX ||
        !bytes_are_zero(type->script_id_hash,
                        sizeof(type->script_id_hash)) ||
        memcmp(type->type_hash, k_build_settings_type_hash,
               sizeof(k_build_settings_type_hash)) != 0) {
        return UNITY_PLAYER_BUILD_SETTINGS_TYPE_IDENTITY_UNSUPPORTED;
    }
    return UNITY_PLAYER_BUILD_SETTINGS_OK;
}

void unity_player_build_settings_init(UnityPlayerBuildSettings* settings) {
    if (settings) memset(settings, 0, sizeof(*settings));
}

void unity_player_build_settings_dispose(UnityPlayerBuildSettings* settings) {
    if (!settings) return;
    if (settings->unity_version) {
        mem_free(settings->unity_version,
                 strlen(settings->unity_version) + 1U);
    }
    if (settings->graphics_apis) {
        mem_free(settings->graphics_apis,
                 settings->graphics_api_count *
                     sizeof(*settings->graphics_apis));
    }
    unity_player_build_settings_init(settings);
}

static UnityPlayerBuildSettingsStatus decode_payload(
    UnityPlayerBuildSettings* result, const SerializedFile* file,
    const AssetObjectInfo* object) {
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return UNITY_PLAYER_BUILD_SETTINGS_OBJECT_RANGE_INVALID;
    }
    UnityPlayerBuildSettingsStatus status =
        validate_type_identity(file, object);
    if (status != UNITY_PLAYER_BUILD_SETTINGS_OK) return status;

    const uint64_t absolute_offset = file->data_offset + object->byte_offset;
    BuildSettingsReader reader;
    stream_init(&reader.stream, file->raw_data + absolute_offset,
                object->byte_size);
    stream_set_endian(&reader.stream, file->big_endian);
    reader.status = UNITY_PLAYER_BUILD_SETTINGS_OK;

    result->serialized_file_version = file->version;
    result->target_platform = file->target_platform;
    result->path_id = object->path_id;
    result->byte_offset = object->byte_offset;
    result->byte_size = object->byte_size;

    /* GlobalGameManager/Object contributes no wire fields in this exact
     * engine schema. The following offsets (+72, +152, +192, +232) are the
     * native in-memory fields, not serialized padding. */
    if (!reader_string_array(&reader, &result->scene_count) ||
        !reader_string_array(&reader,
                             &result->preloaded_plugin_count) ||
        !reader_string_array(&reader,
                             &result->enabled_vr_device_count) ||
        !reader_string_array(&reader, &result->build_tag_count) ||
        !reader_bytes(&reader, result->build_guid,
                      sizeof(result->build_guid))) {
        return reader.status;
    }

    UnityPlayerBuildSettingsFlags* flags = &result->flags;
    bool* ordered_flags[UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_COUNT] = {
        &flags->has_pro_version,
        &flags->is_no_watermark_build,
        &flags->is_prototyping_build,
        &flags->is_educational_build,
        &flags->is_embedded,
        &flags->is_trial,
        &flags->has_publishing_rights,
        &flags->has_shadows,
        &flags->has_soft_shadows,
        &flags->has_local_light_shadows,
        &flags->has_advanced_version,
        &flags->enable_dynamic_batching,
        &flags->is_debug_build,
        &flags->uses_on_mouse_events,
        &flags->has_cluster_rendering,
    };
    for (size_t index = 0U;
         index < UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_COUNT; ++index) {
        if (!reader_boolean(&reader, ordered_flags[index])) {
            return reader.status;
        }
    }
    if (!reader_align4_zero(&reader)) return reader.status;

    const uint8_t* version_bytes = NULL;
    size_t version_size = 0U;
    if (!reader_string_view(&reader, &version_bytes, &version_size)) {
        return reader.status;
    }
    const size_t expected_version_size = strlen(file->unity_version);
    if (version_size != expected_version_size ||
        memcmp(version_bytes, file->unity_version,
               expected_version_size) != 0) {
        return UNITY_PLAYER_BUILD_SETTINGS_VERSION_MISMATCH;
    }
    if (dxbc_size_add_overflows(version_size, 1U)) {
        return UNITY_PLAYER_BUILD_SETTINGS_STRING_LENGTH_INVALID;
    }
    result->unity_version = (char*)mem_alloc(version_size + 1U);
    if (!result->unity_version) {
        return UNITY_PLAYER_BUILD_SETTINGS_ALLOCATION_FAILED;
    }
    memcpy(result->unity_version, version_bytes, version_size);
    result->unity_version[version_size] = '\0';

    int32_t graphics_api_count = 0;
    if (!reader_i32(&reader, &graphics_api_count)) return reader.status;
    if (graphics_api_count < 0 ||
        (size_t)graphics_api_count >
            stream_remaining(&reader.stream) / sizeof(int32_t)) {
        return UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID;
    }
    result->graphics_api_count = (size_t)graphics_api_count;
    if (result->graphics_api_count != 0U) {
        if (dxbc_size_multiply_overflows(
                result->graphics_api_count,
                sizeof(*result->graphics_apis))) {
            return UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID;
        }
        const size_t allocation_size = result->graphics_api_count *
            sizeof(*result->graphics_apis);
        result->graphics_apis = (int32_t*)mem_alloc(allocation_size);
        if (!result->graphics_apis) {
            return UNITY_PLAYER_BUILD_SETTINGS_ALLOCATION_FAILED;
        }
        for (size_t index = 0U; index < result->graphics_api_count;
             ++index) {
            if (!reader_i32(&reader, &result->graphics_apis[index])) {
                return reader.status;
            }
        }
    }
    if (!reader_align4_zero(&reader)) return reader.status;
    if (reader.stream.position != reader.stream.size) {
        return UNITY_PLAYER_BUILD_SETTINGS_OBJECT_BYTES_NOT_EXHAUSTED;
    }
    result->decoded = true;
    return UNITY_PLAYER_BUILD_SETTINGS_OK;
}

UnityPlayerBuildSettingsStatus unity_player_build_settings_decode(
    UnityPlayerBuildSettings* destination, const SerializedFile* file) {
    if (!destination || !file) {
        return UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT;
    }
    if (!source_file_is_structurally_valid(file)) {
        return UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE;
    }
    if (file->version !=
        UNITY_PLAYER_BUILD_SETTINGS_SERIALIZED_FILE_VERSION) {
        return UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_FILE_VERSION;
    }
    if (strcmp(file->unity_version, k_supported_unity_version) != 0) {
        return UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_UNITY_VERSION;
    }

    const AssetObjectInfo* build_settings_object = NULL;
    for (int index = 0; index < file->object_count; ++index) {
        if (file->objects[index].type_id !=
            UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID) {
            continue;
        }
        if (build_settings_object) {
            return UNITY_PLAYER_BUILD_SETTINGS_DUPLICATE;
        }
        build_settings_object = &file->objects[index];
    }
    if (!build_settings_object) return UNITY_PLAYER_BUILD_SETTINGS_MISSING;

    UnityPlayerBuildSettings temporary;
    unity_player_build_settings_init(&temporary);
    UnityPlayerBuildSettingsStatus status = decode_payload(
        &temporary, file, build_settings_object);
    if (status != UNITY_PLAYER_BUILD_SETTINGS_OK) {
        unity_player_build_settings_dispose(&temporary);
        return status;
    }
    unity_player_build_settings_dispose(destination);
    *destination = temporary;
    return UNITY_PLAYER_BUILD_SETTINGS_OK;
}

UnityPlayerBuildSettingsStatus
unity_player_build_settings_decode_serialized_bytes(
    UnityPlayerBuildSettings* destination,
    const uint8_t* bytes, size_t size) {
    if (!destination || !bytes || size == 0U) {
        return UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT;
    }
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, bytes, size)) {
        return UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE;
    }
    UnityPlayerBuildSettingsStatus status =
        unity_player_build_settings_decode(destination, &file);
    serialized_file_close(&file);
    return status;
}

const char* unity_player_build_settings_status_name(
    UnityPlayerBuildSettingsStatus status) {
    switch (status) {
        case UNITY_PLAYER_BUILD_SETTINGS_OK: return "ok";
        case UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE:
            return "invalid-serialized-file";
        case UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_FILE_VERSION:
            return "unsupported-file-version";
        case UNITY_PLAYER_BUILD_SETTINGS_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case UNITY_PLAYER_BUILD_SETTINGS_MISSING:
            return "build-settings-missing";
        case UNITY_PLAYER_BUILD_SETTINGS_DUPLICATE:
            return "build-settings-duplicate";
        case UNITY_PLAYER_BUILD_SETTINGS_OBJECT_RANGE_INVALID:
            return "object-range-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_TYPE_INDEX_INVALID:
            return "type-index-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_TYPE_RECORD_MISMATCH:
            return "type-record-mismatch";
        case UNITY_PLAYER_BUILD_SETTINGS_TYPE_IDENTITY_UNSUPPORTED:
            return "type-identity-unsupported";
        case UNITY_PLAYER_BUILD_SETTINGS_PAYLOAD_TRUNCATED:
            return "payload-truncated";
        case UNITY_PLAYER_BUILD_SETTINGS_COUNT_INVALID:
            return "count-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_STRING_LENGTH_INVALID:
            return "string-length-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_STRING_TRUNCATED:
            return "string-truncated";
        case UNITY_PLAYER_BUILD_SETTINGS_STRING_CONTAINS_NUL:
            return "string-contains-nul";
        case UNITY_PLAYER_BUILD_SETTINGS_BOOLEAN_INVALID:
            return "boolean-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_ALIGNMENT_INVALID:
            return "alignment-invalid";
        case UNITY_PLAYER_BUILD_SETTINGS_VERSION_MISMATCH:
            return "version-mismatch";
        case UNITY_PLAYER_BUILD_SETTINGS_ALLOCATION_FAILED:
            return "allocation-failed";
        case UNITY_PLAYER_BUILD_SETTINGS_OBJECT_BYTES_NOT_EXHAUSTED:
            return "object-bytes-not-exhausted";
        default: return "unknown";
    }
}
