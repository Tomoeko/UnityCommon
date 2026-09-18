// SPDX-License-Identifier: GPL-3.0-only

#include "io/bundle_archive.h"
#include "common/stream.h"
#include "io/lz4_decompress.h"
#include "io/lzma/LzmaDec.h"

#include <limits.h>

enum {
    UNITYFS_SUPPORTED_ARCHIVE_VERSION = 8,
    UNITYFS_COMPRESSION_MASK = 0x3f,
    UNITYFS_BLOCKS_AND_DIRECTORY_INFO_COMBINED = 0x40,
    UNITYFS_BLOCK_INFO_AT_END = 0x80,
    UNITYFS_BLOCK_INFO_NEEDS_PADDING = 0x200,
    UNITYFS_SUPPORTED_ARCHIVE_FLAGS =
        UNITYFS_COMPRESSION_MASK |
        UNITYFS_BLOCKS_AND_DIRECTORY_INFO_COMBINED |
        UNITYFS_BLOCK_INFO_AT_END |
        UNITYFS_BLOCK_INFO_NEEDS_PADDING,
    UNITYFS_BLOCK_STREAMING = 0x40,
    UNITYFS_SUPPORTED_BLOCK_FLAGS =
        UNITYFS_COMPRESSION_MASK | UNITYFS_BLOCK_STREAMING
};

static const char UNITYFS_SUPPORTED_GENERATION[] = "5.x.x";
static const char UNITYFS_SUPPORTED_ENGINE_VERSION[] = "2021.3.35f1";

static bool unityfs_compression_is_supported(uint32_t flags) {
    return (flags & UNITYFS_COMPRESSION_MASK) <= 3u;
}

static bool bundle_name_has_suffix(const char* value, const char* suffix) {
    if (!value || !suffix) return false;
    size_t value_size = strlen(value);
    size_t suffix_size = strlen(suffix);
    return suffix_size <= value_size &&
           strcmp(value + value_size - suffix_size, suffix) == 0;
}

BundleMemberKind bundle_member_classify(const BundleDirectoryInfo* member) {
    if (!member || !member->name ||
        (member->flags & ~UNITYFS_NODE_KNOWN_FLAGS) != 0U) {
        return BUNDLE_MEMBER_INVALID;
    }
    if ((member->flags & UNITYFS_NODE_SERIALIZED_FILE) != 0U) {
        return (member->flags &
                (UNITYFS_NODE_DIRECTORY | UNITYFS_NODE_DELETED)) == 0U
            ? BUNDLE_MEMBER_SERIALIZED_FILE : BUNDLE_MEMBER_INVALID;
    }
    if ((member->flags & UNITYFS_NODE_DIRECTORY) != 0U) {
        return (member->flags & UNITYFS_NODE_DELETED) == 0U
            ? BUNDLE_MEMBER_DIRECTORY : BUNDLE_MEMBER_INVALID;
    }
    if ((member->flags & UNITYFS_NODE_DELETED) != 0U) {
        return BUNDLE_MEMBER_DELETED;
    }
    if (bundle_name_has_suffix(member->name, ".resS") ||
        bundle_name_has_suffix(member->name, ".resource") ||
        bundle_name_has_suffix(member->name, ".resources")) {
        return BUNDLE_MEMBER_RESOURCE;
    }
    return BUNDLE_MEMBER_INVALID;
}

static bool allocate_zeroed_array(void** out, size_t count,
                                  size_t element_size) {
    if (!out || element_size == 0 ||
        dxbc_size_multiply_overflows(count, element_size)) {
        return false;
    }
    *out = NULL;
    if (count == 0) return true;
    size_t allocation_size = count * element_size;
    void* values = mem_alloc(allocation_size);
    if (!values) return false;
    memset(values, 0, allocation_size);
    *out = values;
    return true;
}

static void* lzma_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}

static void lzma_free(ISzAllocPtr p, void* address) {
    (void)p;
    free(address);
}

static bool decompress_block(uint8_t comp_type, const uint8_t* src,
                             size_t src_len, uint8_t* dst,
                             size_t dst_len) {
    if ((!src && src_len != 0) || (!dst && dst_len != 0)) return false;

    if (comp_type == 0) { // Uncompressed
        if (src_len != dst_len) return false;
        if (dst_len != 0) memcpy(dst, src, dst_len);
        return true;
    } else if (comp_type == 1) { // LZMA
        if (src_len < 5) return false;
        ISzAlloc alloc = { lzma_alloc, lzma_free };
        SizeT out_len = dst_len;
        SizeT in_len = src_len - 5;
        const SizeT expected_in_len = in_len;
        ELzmaStatus status;
        uint8_t empty_output;
        uint8_t* output = dst_len == 0 ? &empty_output : dst;
        SRes res = LzmaDecode(
            output, &out_len,
            src + 5, &in_len,
            src, 5,
            LZMA_FINISH_END,
            &status, &alloc
        );
        return res == SZ_OK && out_len == dst_len &&
               in_len == expected_in_len &&
               (status == LZMA_STATUS_FINISHED_WITH_MARK ||
                status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK);
    } else if (comp_type == 2 || comp_type == 3) { // LZ4 / LZ4HC
        if (src_len > INT_MAX || dst_len > INT_MAX) return false;
        if (src_len == 0) return dst_len == 0;
        uint8_t empty_output;
        uint8_t* output = dst_len == 0 ? &empty_output : dst;
        int result = lz4_decompress_safe(src, output, (int)src_len,
                                         (int)dst_len);
        return result >= 0 && (size_t)result == dst_len;
    }
    return false;
}

bool bundle_open(BundleArchive* archive, const uint8_t* file_data, size_t file_size) {
    if (!archive) return false;
    memset(archive, 0, sizeof(BundleArchive));
    if (!file_data) return false;
    
    ByteStream stream;
    stream_init(&stream, file_data, file_size);
    stream_set_endian(&stream, true); // Archive headers are big-endian
    
    size_t sig_len = 0;
    archive->signature = stream_read_string_alloc(&stream, &sig_len);
    if (!archive->signature) {
        LOG_ERROR("Failed to read signature");
        return false;
    }
    
    if (strcmp(archive->signature, "UnityFS") != 0) {
        LOG_ERROR("Unsupported signature: %s", archive->signature);
        bundle_close(archive);
        return false;
    }
    
    if (!stream_read_uint32(&stream, &archive->version)) goto fail;
    
    size_t gen_len = 0;
    archive->generation_version = stream_read_string_alloc(&stream, &gen_len);
    if (!archive->generation_version) goto fail;
    size_t eng_len = 0;
    archive->engine_version = stream_read_string_alloc(&stream, &eng_len);
    if (!archive->engine_version) goto fail;

    if (archive->version != UNITYFS_SUPPORTED_ARCHIVE_VERSION ||
        strcmp(archive->generation_version,
               UNITYFS_SUPPORTED_GENERATION) != 0 ||
        strcmp(archive->engine_version,
               UNITYFS_SUPPORTED_ENGINE_VERSION) != 0) {
        LOG_ERROR("Unsupported UnityFS dialect");
        goto fail;
    }
    
    if (!stream_read_uint64(&stream, &archive->total_file_size) ||
        !stream_read_uint32(&stream, &archive->compressed_info_size) ||
        !stream_read_uint32(&stream, &archive->decompressed_info_size) ||
        !stream_read_uint32(&stream, &archive->flags)) {
        goto fail;
    }
    if ((archive->flags &
         UNITYFS_BLOCKS_AND_DIRECTORY_INFO_COMBINED) == 0U ||
        (archive->flags &
         ~(uint32_t)UNITYFS_SUPPORTED_ARCHIVE_FLAGS) != 0U ||
        !unityfs_compression_is_supported(archive->flags)) {
        LOG_ERROR("Unsupported UnityFS archive flags");
        goto fail;
    }
    if (archive->total_file_size != file_size ||
        archive->compressed_info_size == 0 ||
        archive->decompressed_info_size < 20) {
        LOG_ERROR("UnityFS declared size does not exactly match the input");
        goto fail;
    }
    
    if (!stream_align(&stream, 16)) goto fail;

    const size_t declared_file_size = (size_t)archive->total_file_size;
    if (stream.position > declared_file_size) {
        LOG_ERROR("UnityFS header exceeds declared file size");
        goto fail;
    }
    
    // Determine block info offset
    size_t block_info_offset = stream.position;
    if ((archive->flags & UNITYFS_BLOCK_INFO_AT_END) != 0) {
        if (archive->compressed_info_size > declared_file_size) {
            LOG_ERROR("Block info size exceeds declared file size");
            goto fail;
        }
        block_info_offset =
            declared_file_size - archive->compressed_info_size;
        if (block_info_offset < stream.position) {
            LOG_ERROR("Block info overlaps UnityFS header");
            goto fail;
        }
    }

    if (block_info_offset > declared_file_size ||
        archive->compressed_info_size >
            declared_file_size - block_info_offset) {
        LOG_ERROR("Block info offset out of bounds");
        goto fail;
    }
    
    // Decompress BlockInfo chunk
    uint8_t* decompressed_info = (uint8_t*)mem_alloc(archive->decompressed_info_size);
    if (!decompressed_info) {
        bundle_close(archive);
        return false;
    }
    
    uint8_t comp_type = archive->flags & UNITYFS_COMPRESSION_MASK;
    const uint8_t* compressed_info_ptr = file_data + block_info_offset;
    
    bool decompressed = decompress_block(
        comp_type,
        compressed_info_ptr, archive->compressed_info_size,
        decompressed_info, archive->decompressed_info_size
    );

    if (!decompressed) {
        LOG_ERROR("Failed to decompress block info chunk");
        mem_free(decompressed_info, archive->decompressed_info_size);
        bundle_close(archive);
        return false;
    }
    
    // Parse decompressed block info stream
    ByteStream info_stream;
    stream_init(&info_stream, decompressed_info, archive->decompressed_info_size);
    stream_set_endian(&info_stream, true);
    
    // Skip 16-byte hash
    if (!stream_skip(&info_stream, 16)) {
        mem_free(decompressed_info, archive->decompressed_info_size);
        goto fail;
    }
    
    uint32_t block_count;
    if (!stream_read_uint32(&info_stream, &block_count)) {
        mem_free(decompressed_info, archive->decompressed_info_size);
        bundle_close(archive);
        return false;
    }
    
    if (block_count > INT_MAX ||
        block_count > stream_remaining(&info_stream) / 10u) {
        LOG_ERROR("Block count exceeds block-info payload");
        mem_free(decompressed_info, archive->decompressed_info_size);
        goto fail;
    }
    archive->block_count = (int)block_count;
    if (block_count > 0) {
        if (!allocate_zeroed_array((void**)&archive->blocks,
                                   (size_t)block_count,
                                   sizeof(*archive->blocks))) {
            mem_free(decompressed_info, archive->decompressed_info_size);
            bundle_close(archive);
            return false;
        }
        
        for (uint32_t i = 0; i < block_count; i++) {
            if (!stream_read_uint32(&info_stream, &archive->blocks[i].decompressed_size) ||
                !stream_read_uint32(&info_stream, &archive->blocks[i].compressed_size) ||
                !stream_read_uint16(&info_stream, &archive->blocks[i].flags)) {
                LOG_ERROR("Failed to read block info details");
                mem_free(decompressed_info, archive->decompressed_info_size);
                bundle_close(archive);
                return false;
            }
            if (((uint32_t)archive->blocks[i].flags &
                 ~(uint32_t)UNITYFS_SUPPORTED_BLOCK_FLAGS) != 0U ||
                !unityfs_compression_is_supported(
                    archive->blocks[i].flags)) {
                LOG_ERROR("Unsupported UnityFS block flags");
                mem_free(decompressed_info,
                         archive->decompressed_info_size);
                goto fail;
            }
        }
    } else {
        archive->blocks = NULL;
    }
    
    uint32_t dir_count;
    if (!stream_read_uint32(&info_stream, &dir_count)) {
        mem_free(decompressed_info, archive->decompressed_info_size);
        bundle_close(archive);
        return false;
    }
    
    if (dir_count > INT_MAX ||
        dir_count > stream_remaining(&info_stream) / 21u) {
        LOG_ERROR("Directory count exceeds block-info payload");
        mem_free(decompressed_info, archive->decompressed_info_size);
        goto fail;
    }
    archive->directory_count = (int)dir_count;
    if (dir_count > 0) {
        if (!allocate_zeroed_array((void**)&archive->directories,
                                   (size_t)dir_count,
                                   sizeof(*archive->directories))) {
            mem_free(decompressed_info, archive->decompressed_info_size);
            bundle_close(archive);
            return false;
        }
        
        for (uint32_t i = 0; i < dir_count; i++) {
            if (!stream_read_uint64(&info_stream, &archive->directories[i].offset) ||
                !stream_read_uint64(&info_stream, &archive->directories[i].decompressed_size) ||
                !stream_read_uint32(&info_stream, &archive->directories[i].flags)) {
                LOG_ERROR("Failed to read directory info details");
                mem_free(decompressed_info, archive->decompressed_info_size);
                bundle_close(archive);
                return false;
            }
            size_t dir_name_len = 0;
            archive->directories[i].name = stream_read_string_alloc(&info_stream, &dir_name_len);
            if (!archive->directories[i].name) {
                LOG_ERROR("Failed to read directory name");
                mem_free(decompressed_info, archive->decompressed_info_size);
                bundle_close(archive);
                return false;
            }
        }
    } else {
        archive->directories = NULL;
    }

    if (stream_remaining(&info_stream) != 0) {
        LOG_ERROR("Trailing bytes in UnityFS block-info payload");
        mem_free(decompressed_info, archive->decompressed_info_size);
        goto fail;
    }
    
    mem_free(decompressed_info, archive->decompressed_info_size);
    
    // The compressed block stream occupies exactly the region between its
    // aligned start and either the block-info chunk or the declared EOF.
    size_t data_start_offset;
    size_t data_end_offset;
    if ((archive->flags & UNITYFS_BLOCK_INFO_AT_END) != 0) {
        data_start_offset = stream.position;
        data_end_offset = block_info_offset;
    } else {
        data_start_offset = block_info_offset;
        if (archive->compressed_info_size >
            declared_file_size - data_start_offset) {
            LOG_ERROR("UnityFS data offset overflow");
            goto fail;
        }
        data_start_offset += archive->compressed_info_size;
        data_end_offset = declared_file_size;
    }
    if ((archive->flags & UNITYFS_BLOCK_INFO_NEEDS_PADDING) != 0) {
        if (data_start_offset > SIZE_MAX - 15u) {
            LOG_ERROR("UnityFS aligned data offset overflow");
            goto fail;
        }
        data_start_offset = (data_start_offset + 15u) & ~(size_t)15u;
    }
    if (data_start_offset > data_end_offset) {
        LOG_ERROR("UnityFS data range overlaps block info or declared EOF");
        goto fail;
    }

    // Compute exact compressed and decompressed coverage before allocating.
    uint64_t total_payload_size = 0;
    size_t total_compressed_size = 0;
    for (int i = 0; i < archive->block_count; i++) {
        if (UINT64_MAX - total_payload_size <
            archive->blocks[i].decompressed_size) {
            LOG_ERROR("UnityFS decompressed payload size overflow");
            goto fail;
        }
        total_payload_size += archive->blocks[i].decompressed_size;
        if (archive->blocks[i].compressed_size >
            SIZE_MAX - total_compressed_size) {
            LOG_ERROR("UnityFS compressed payload size overflow");
            goto fail;
        }
        total_compressed_size += archive->blocks[i].compressed_size;
    }
    if (total_payload_size > SIZE_MAX) {
        LOG_ERROR("UnityFS decompressed payload exceeds address space");
        goto fail;
    }
    if (total_compressed_size != data_end_offset - data_start_offset) {
        LOG_ERROR("UnityFS blocks do not exactly cover declared data range");
        goto fail;
    }
    
    archive->payload_size = total_payload_size;
    if (total_payload_size > 0) {
        archive->payload = (uint8_t*)mem_alloc(total_payload_size);
        if (!archive->payload) {
            bundle_close(archive);
            return false;
        }
    } else {
        archive->payload = NULL;
    }
    
    // Loop through blocks and decompress payload
    size_t current_payload_offset = 0;
    size_t file_read_pos = data_start_offset;
    
    for (int i = 0; i < archive->block_count; i++) {
        BundleBlockInfo* block = &archive->blocks[i];
        if (file_read_pos > data_end_offset ||
            block->compressed_size > data_end_offset - file_read_pos ||
            current_payload_offset > archive->payload_size ||
            block->decompressed_size >
                archive->payload_size - current_payload_offset) {
            LOG_ERROR("UnityFS block range exceeds declared region");
            goto fail;
        }
        
        uint8_t block_comp = block->flags & UNITYFS_COMPRESSION_MASK;
        bool block_decompressed = decompress_block(
            block_comp,
            file_data + file_read_pos, block->compressed_size,
            archive->payload
                ? archive->payload + current_payload_offset
                : NULL,
            block->decompressed_size
        );

        if (!block_decompressed) {
            LOG_ERROR("Failed to decompress data block %d", i);
            goto fail;
        }
        
        file_read_pos += block->compressed_size;
        current_payload_offset += block->decompressed_size;
    }

    if (file_read_pos != data_end_offset ||
        current_payload_offset != archive->payload_size) {
        LOG_ERROR("UnityFS block coverage mismatch");
        goto fail;
    }

    /* Validate every directory extent before exposing the archive.  Delaying
     * this until bundle_get_member_view() allows an earlier valid member to
     * be visited and decoded before a later corrupt member is discovered. */
    for (int i = 0; i < archive->directory_count; ++i) {
        const BundleDirectoryInfo* directory = &archive->directories[i];
        if (directory->offset > archive->payload_size ||
            directory->decompressed_size >
                archive->payload_size - (size_t)directory->offset) {
            LOG_ERROR("UnityFS directory range exceeds payload");
            goto fail;
        }
    }

    return true;

fail:
    bundle_close(archive);
    return false;
}

void bundle_close(BundleArchive* archive) {
    if (archive->signature) {
        mem_free(archive->signature, strlen(archive->signature) + 1);
        archive->signature = NULL;
    }
    if (archive->generation_version) {
        mem_free(archive->generation_version, strlen(archive->generation_version) + 1);
        archive->generation_version = NULL;
    }
    if (archive->engine_version) {
        mem_free(archive->engine_version, strlen(archive->engine_version) + 1);
        archive->engine_version = NULL;
    }
    if (archive->blocks) {
        mem_free(archive->blocks, archive->block_count * sizeof(BundleBlockInfo));
        archive->blocks = NULL;
    }
    if (archive->directories) {
        for (int i = 0; i < archive->directory_count; i++) {
            if (archive->directories[i].name) {
                mem_free(archive->directories[i].name, strlen(archive->directories[i].name) + 1);
            }
        }
        mem_free(archive->directories, archive->directory_count * sizeof(BundleDirectoryInfo));
        archive->directories = NULL;
    }
    if (archive->payload) {
        mem_free(archive->payload, archive->payload_size);
        archive->payload = NULL;
    }
    archive->payload_size = 0;
}

bool bundle_get_member_view(const BundleArchive* archive, size_t member_index,
                            const uint8_t** out_data, size_t* out_size) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!archive || !out_data || !out_size ||
        archive->directory_count < 0 ||
        (archive->directory_count > 0 && !archive->directories) ||
        member_index >= (size_t)archive->directory_count) {
        return false;
    }
    const BundleDirectoryInfo* dir = &archive->directories[member_index];
    if (dir->offset > archive->payload_size ||
        dir->decompressed_size >
            archive->payload_size - (size_t)dir->offset) {
        LOG_ERROR("Directory offset/size out of payload bounds");
        return false;
    }
    if (dir->decompressed_size == 0U) return true;
    if (!archive->payload) {
        LOG_ERROR("Nonempty directory has no bundle payload");
        return false;
    }
    *out_data = archive->payload + (size_t)dir->offset;
    *out_size = (size_t)dir->decompressed_size;
    return true;
}

bool bundle_get_file_view(const BundleArchive* archive, const char* name,
                          const uint8_t** out_data, size_t* out_size) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!archive || !name || !out_data || !out_size ||
        archive->directory_count < 0 ||
        (archive->directory_count > 0 && !archive->directories)) {
        return false;
    }
    size_t match = SIZE_MAX;
    for (int i = 0; i < archive->directory_count; ++i) {
        const BundleDirectoryInfo* dir = &archive->directories[i];
        if (!dir->name || strcmp(dir->name, name) != 0) continue;
        if (match != SIZE_MAX) {
            LOG_ERROR("Duplicate UnityFS member name is ambiguous");
            return false;
        }
        match = (size_t)i;
    }
    if (match != SIZE_MAX) {
        return bundle_get_member_view(archive, match, out_data, out_size);
    }
    return false;
}

const uint8_t* bundle_get_file(BundleArchive* archive, const char* name,
                               size_t* out_size) {
    const uint8_t* data = NULL;
    size_t size = 0;
    if (!bundle_get_file_view(archive, name, &data, &size)) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    if (out_size) *out_size = size;
    if (size == 0) return NULL;
    return data;
}
