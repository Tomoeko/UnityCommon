// SPDX-License-Identifier: GPL-3.0-only

#include "io/typetree_schema_profile.h"

#include <string.h>

typedef struct {
    const char* unity_version;
    int32_t class_id;
    uint8_t type_hash[16];
    uint8_t shape_digest[COMMON_SHA256_DIGEST_SIZE];
} KnownSchemaProfile;

/* Filled from schemas/unity-2021.3.35f1-shader.registry. */
static const KnownSchemaProfile k_known_profiles[] = {
    {
        "2021.3.35f1",
        21,
        {0xc6, 0x00, 0x98, 0xac, 0x66, 0xa2, 0x8b, 0x50,
         0xaa, 0x05, 0x80, 0xdb, 0x11, 0xbf, 0x01, 0x8c},
        {0x7c, 0x4d, 0x49, 0xa6, 0xd7, 0x8e, 0x0d, 0xd2,
         0x96, 0xae, 0x76, 0x32, 0x68, 0x13, 0x69, 0x82,
         0x85, 0x2f, 0x7a, 0x62, 0xae, 0x8c, 0xde, 0x85,
         0x39, 0x2e, 0xc0, 0xfa, 0x8d, 0xbb, 0x99, 0x8c},
    },
    {
        "2021.3.35f1",
        48,
        {0xf0, 0xc1, 0x84, 0x27, 0x2a, 0x05, 0xe5, 0x44,
         0x47, 0xe3, 0x63, 0x66, 0xec, 0x48, 0x76, 0xa6},
        {0xb7, 0xfe, 0xd8, 0x65, 0xd8, 0x64, 0xaa, 0xea,
         0xa7, 0x97, 0xdd, 0x36, 0x5f, 0x1c, 0x64, 0x6e,
         0x16, 0xaf, 0xcc, 0xbd, 0xc0, 0x45, 0x75, 0x5e,
         0x9f, 0x6a, 0x83, 0x79, 0xb3, 0xd8, 0x7b, 0xfb},
    },
    {
        "2021.3.35f1",
        142,
        {0x97, 0xda, 0x5f, 0x46, 0x88, 0xe4, 0x5a, 0x57,
         0xc8, 0xb4, 0x2d, 0x4f, 0x42, 0x49, 0x72, 0x97},
        {0x01, 0xe5, 0x6d, 0x4d, 0x77, 0xbc, 0xda, 0x8f,
         0x7e, 0x6c, 0x35, 0x72, 0x61, 0x8b, 0x5a, 0x06,
         0x0e, 0x01, 0x5c, 0xb3, 0x8f, 0xc4, 0x5d, 0x80,
         0x64, 0xc1, 0x33, 0xd2, 0x61, 0xa1, 0xd2, 0xd1},
    },
    {
        /* Unity 2021.3.35f1 player installations deliberately retain this
         * exact 2021.3.29f1 default-resource Shader schema. */
        "2021.3.29f1",
        48,
        {0xf0, 0xc1, 0x84, 0x27, 0x2a, 0x05, 0xe5, 0x44,
         0x47, 0xe3, 0x63, 0x66, 0xec, 0x48, 0x76, 0xa6},
        {0xb7, 0xfe, 0xd8, 0x65, 0xd8, 0x64, 0xaa, 0xea,
         0xa7, 0x97, 0xdd, 0x36, 0x5f, 0x1c, 0x64, 0x6e,
         0x16, 0xaf, 0xcc, 0xbd, 0xc0, 0x45, 0x75, 0x5e,
         0x9f, 0x6a, 0x83, 0x79, 0xb3, 0xd8, 0x7b, 0xfb},
    },
    {
        /* The pinned Unity 2021.3.29f1 default-resource SerializedFile
         * carries this exact Material type identity.  Its semantic shape is
         * byte-for-byte the authoritative 2021.3.35f1 ClassID 21 schema;
         * the registry still requires an explicit version binding. */
        "2021.3.29f1",
        21,
        {0xc6, 0x00, 0x98, 0xac, 0x66, 0xa2, 0x8b, 0x50,
         0xaa, 0x05, 0x80, 0xdb, 0x11, 0xbf, 0x01, 0x8c},
        {0x7c, 0x4d, 0x49, 0xa6, 0xd7, 0x8e, 0x0d, 0xd2,
         0x96, 0xae, 0x76, 0x32, 0x68, 0x13, 0x69, 0x82,
         0x85, 0x2f, 0x7a, 0x62, 0xae, 0x8c, 0xde, 0x85,
         0x39, 0x2e, 0xc0, 0xfa, 0x8d, 0xbb, 0x99, 0x8c},
    },
};

static void hash_u8(CommonSha256Context* context, uint8_t value) {
    common_sha256_update(context, &value, sizeof(value));
}

static void hash_u16(CommonSha256Context* context, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
    };
    common_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u32(CommonSha256Context* context, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    common_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(CommonSha256Context* context, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    common_sha256_update(context, bytes, sizeof(bytes));
}

static bool hash_string(CommonSha256Context* context, const char* value) {
    if (!context || !value) return false;
    size_t size = strlen(value);
    if (size > UINT32_MAX) return false;
    hash_u32(context, (uint32_t)size);
    common_sha256_update(context, value, size);
    return true;
}

bool typetree_schema_shape_digest(
    const TypeTreeType* schema,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    static const uint8_t domain[] =
        "DXBCSandbox.TypeTreeSemanticShape.v1";
    if (!schema || !digest || schema->node_count <= 0 || !schema->nodes ||
        schema->dependency_count < 0 ||
        (schema->dependency_count != 0 && !schema->dependencies)) {
        return false;
    }

    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, domain, sizeof(domain));
    hash_u32(&context, (uint32_t)schema->type_id);
    hash_u8(&context, schema->is_stripped ? 1U : 0U);
    hash_u16(&context, schema->script_type_index);
    common_sha256_update(&context, schema->script_id_hash,
                         sizeof(schema->script_id_hash));
    common_sha256_update(&context, schema->type_hash,
                         sizeof(schema->type_hash));
    hash_u32(&context, (uint32_t)schema->node_count);
    for (int index = 0; index < schema->node_count; ++index) {
        const TypeTreeNode* node = &schema->nodes[index];
        if (!node->type_str || !node->name_str) return false;
        hash_u16(&context, node->version);
        hash_u8(&context, node->level);
        hash_u8(&context, node->type_flags);
        hash_u32(&context, (uint32_t)node->byte_size);
        hash_u32(&context, node->index);
        hash_u32(&context, node->meta_flags);
        hash_u64(&context, node->ref_type_hash);
        if (!hash_string(&context, node->type_str) ||
            !hash_string(&context, node->name_str)) {
            return false;
        }
    }
    hash_u32(&context, (uint32_t)schema->dependency_count);
    for (int index = 0; index < schema->dependency_count; ++index) {
        hash_u32(&context, (uint32_t)schema->dependencies[index]);
    }
    hash_u8(&context, schema->is_ref_type ? 1U : 0U);
    if (schema->is_ref_type) {
        if (!hash_string(&context, schema->ref_class_name) ||
            !hash_string(&context, schema->ref_namespace) ||
            !hash_string(&context, schema->ref_asm_name)) {
            return false;
        }
    }
    common_sha256_final(&context, digest);
    return true;
}

TypeTreeSchemaProfileResult typetree_schema_validate_known_profile(
    const char* unity_version, size_t unity_version_size,
    int32_t class_id, const uint8_t type_hash[16],
    const TypeTreeType* schema) {
    if (!unity_version || !type_hash || !schema) {
        return TYPETREE_SCHEMA_PROFILE_UNKNOWN;
    }
    const KnownSchemaProfile* selected = NULL;
    for (size_t index = 0;
         index < sizeof(k_known_profiles) / sizeof(k_known_profiles[0]);
         ++index) {
        size_t known_version_size =
            strlen(k_known_profiles[index].unity_version);
        if (known_version_size == unity_version_size &&
            memcmp(k_known_profiles[index].unity_version, unity_version,
                   unity_version_size) == 0 &&
            k_known_profiles[index].class_id == class_id &&
            memcmp(k_known_profiles[index].type_hash, type_hash, 16U) == 0) {
            selected = &k_known_profiles[index];
            break;
        }
    }
    if (!selected) return TYPETREE_SCHEMA_PROFILE_UNKNOWN;

    uint8_t actual[COMMON_SHA256_DIGEST_SIZE];
    if (!typetree_schema_shape_digest(schema, actual) ||
        memcmp(actual, selected->shape_digest, sizeof(actual)) != 0) {
        return TYPETREE_SCHEMA_PROFILE_INVALID;
    }
    return TYPETREE_SCHEMA_PROFILE_VALID;
}
