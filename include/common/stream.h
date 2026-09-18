// SPDX-License-Identifier: GPL-3.0-only

#ifndef STREAM_H
#define STREAM_H

#include "common/common.h"

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t position;
    bool big_endian;
} ByteStream;

void stream_init(ByteStream* stream, const uint8_t* data, size_t size);
void stream_set_endian(ByteStream* stream, bool big_endian);
size_t stream_remaining(const ByteStream* stream);
bool stream_seek(ByteStream* stream, size_t position);
bool stream_align(ByteStream* stream, size_t alignment);
bool stream_skip(ByteStream* stream, size_t count);

bool stream_read_bytes(ByteStream* stream, uint8_t* dest, size_t count);
bool stream_read_uint8(ByteStream* stream, uint8_t* val);
bool stream_read_uint16(ByteStream* stream, uint16_t* val);
bool stream_read_uint32(ByteStream* stream, uint32_t* val);
bool stream_read_uint64(ByteStream* stream, uint64_t* val);

bool stream_read_int8(ByteStream* stream, int8_t* val);
bool stream_read_int16(ByteStream* stream, int16_t* val);
bool stream_read_int32(ByteStream* stream, int32_t* val);
bool stream_read_int64(ByteStream* stream, int64_t* val);

bool stream_read_float(ByteStream* stream, float* val);
bool stream_read_double(ByteStream* stream, double* val);

// Allocates and reads a null-terminated string.
// Memory must be freed using mem_free (stores size in *out_len if not NULL).
char* stream_read_string_alloc(ByteStream* stream, size_t* out_len);

#endif // STREAM_H
