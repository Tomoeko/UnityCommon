// SPDX-License-Identifier: GPL-3.0-only

#include "common/stream.h"

// Define global memory tracking variables
atomic_size_t g_allocated_bytes = ATOMIC_VAR_INIT(0);
atomic_size_t g_allocations_count = ATOMIC_VAR_INIT(0);

void stream_init(ByteStream* stream, const uint8_t* data, size_t size) {
    stream->data = data;
    stream->size = size;
    stream->position = 0;
    stream->big_endian = false; // Default to little endian
}

void stream_set_endian(ByteStream* stream, bool big_endian) {
    stream->big_endian = big_endian;
}

size_t stream_remaining(const ByteStream* stream) {
    if (!stream || stream->position > stream->size) return 0;
    return stream->size - stream->position;
}

bool stream_seek(ByteStream* stream, size_t position) {
    if (!stream || position > stream->size) return false;
    stream->position = position;
    return true;
}

bool stream_align(ByteStream* stream, size_t alignment) {
    if (alignment == 0) return true;
    size_t rem = stream->position % alignment;
    if (rem != 0) {
        size_t pad = alignment - rem;
        return stream_skip(stream, pad);
    }
    return true;
}

bool stream_skip(ByteStream* stream, size_t count) {
    if (!stream || count > stream_remaining(stream)) {
        return false;
    }
    stream->position += count;
    return true;
}

bool stream_read_bytes(ByteStream* stream, uint8_t* dest, size_t count) {
    if (!stream || (!dest && count != 0) || count > stream_remaining(stream)) {
        return false;
    }
    memcpy(dest, &stream->data[stream->position], count);
    stream->position += count;
    return true;
}

bool stream_read_uint8(ByteStream* stream, uint8_t* val) {
    return stream_read_bytes(stream, val, 1);
}

bool stream_read_uint16(ByteStream* stream, uint16_t* val) {
    uint16_t raw;
    if (!stream_read_bytes(stream, (uint8_t*)&raw, 2)) return false;
    *val = stream->big_endian ? read_be16(raw) : read_le16(raw);
    return true;
}

bool stream_read_uint32(ByteStream* stream, uint32_t* val) {
    uint32_t raw;
    if (!stream_read_bytes(stream, (uint8_t*)&raw, 4)) return false;
    *val = stream->big_endian ? read_be32(raw) : read_le32(raw);
    return true;
}

bool stream_read_uint64(ByteStream* stream, uint64_t* val) {
    uint64_t raw;
    if (!stream_read_bytes(stream, (uint8_t*)&raw, 8)) return false;
    *val = stream->big_endian ? read_be64(raw) : read_le64(raw);
    return true;
}

bool stream_read_int8(ByteStream* stream, int8_t* val) {
    return stream_read_uint8(stream, (uint8_t*)val);
}

bool stream_read_int16(ByteStream* stream, int16_t* val) {
    return stream_read_uint16(stream, (uint16_t*)val);
}

bool stream_read_int32(ByteStream* stream, int32_t* val) {
    return stream_read_uint32(stream, (uint32_t*)val);
}

bool stream_read_int64(ByteStream* stream, int64_t* val) {
    return stream_read_uint64(stream, (uint64_t*)val);
}

bool stream_read_float(ByteStream* stream, float* val) {
    uint32_t bits;
    if (!stream_read_uint32(stream, &bits)) return false;
    memcpy(val, &bits, sizeof(bits));
    return true;
}

bool stream_read_double(ByteStream* stream, double* val) {
    uint64_t bits;
    if (!stream_read_uint64(stream, &bits)) return false;
    memcpy(val, &bits, sizeof(bits));
    return true;
}

char* stream_read_string_alloc(ByteStream* stream, size_t* out_len) {
    size_t start = stream->position;
    size_t len = 0;
    while (start + len < stream->size && stream->data[start + len] != '\0') {
        len++;
    }
    
    // Check if we hit the end of the stream without a null terminator
    if (start + len >= stream->size) {
        return NULL;
    }
    
    char* str = (char*)mem_alloc(len + 1);
    if (!str) return NULL;
    
    memcpy(str, &stream->data[start], len);
    str[len] = '\0';
    
    stream->position = start + len + 1; // skip null terminator
    
    if (out_len) {
        *out_len = len + 1; // Include the null terminator in allocated size
    }
    
    return str;
}
