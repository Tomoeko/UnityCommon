// SPDX-License-Identifier: GPL-3.0-only

#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include "common/common.h"
#include <stdarg.h>

typedef struct {
    char* buf;
    size_t len;
    size_t capacity;
    bool failed;
} StringBuilder;

void sb_init(StringBuilder* sb);
void sb_init_with_capacity(StringBuilder* sb, size_t capacity);
void sb_free(StringBuilder* sb);
void sb_clear(StringBuilder* sb);
void sb_append(StringBuilder* sb, const char* str);
void sb_append_len(StringBuilder* sb, const char* str, size_t len);
void sb_append_char(StringBuilder* sb, char c);
void sb_appendf(StringBuilder* sb, const char* fmt, ...);
void sb_appendfv(StringBuilder* sb, const char* fmt, va_list args);
bool sb_ok(const StringBuilder* sb);

// Detaches the internal buffer and returns it (memory ownership transferred to caller).
// StringBuilder is reset back to empty state.
char* sb_detach(StringBuilder* sb);

#endif // STRING_BUILDER_H
