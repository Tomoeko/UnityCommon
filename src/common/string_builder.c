// SPDX-License-Identifier: GPL-3.0-only

#include "common/string_builder.h"

void sb_init(StringBuilder* sb) {
    sb->buf = NULL;
    sb->len = 0;
    sb->capacity = 0;
    sb->failed = false;
}

void sb_init_with_capacity(StringBuilder* sb, size_t capacity) {
    sb->buf = NULL;
    sb->len = 0;
    sb->capacity = 0;
    sb->failed = false;
    if (capacity > 0) {
        sb->buf = (char*)mem_alloc(capacity);
        if (sb->buf) {
            sb->buf[0] = '\0';
            sb->capacity = capacity;
        }
    }
}

void sb_free(StringBuilder* sb) {
    if (sb->buf) {
        mem_free(sb->buf, sb->capacity);
        sb->buf = NULL;
    }
    sb->len = 0;
    sb->failed = false;
    sb->capacity = 0;
}

void sb_clear(StringBuilder* sb) {
    sb->len = 0;
    if (sb->buf) {
        sb->buf[0] = '\0';
    }
}
static bool sb_ensure_capacity(StringBuilder* sb, size_t additional) {
    if (!sb || sb->failed || sb->len == SIZE_MAX ||
        additional > SIZE_MAX - sb->len - 1) {
        if (sb) sb->failed = true;
        return false;
    }
    size_t required = sb->len + additional + 1; // +1 for null terminator
    if (required <= sb->capacity) return true;

    size_t new_cap = sb->capacity == 0 ? 32 : sb->capacity * 2;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }

    if (sb->buf == NULL) {
        sb->buf = (char*)mem_alloc(new_cap);
        if (sb->buf) {
            sb->buf[0] = '\0';
            sb->capacity = new_cap;
        }
    } else {
        void* new_ptr = mem_realloc(sb->buf, sb->capacity, new_cap);
        if (new_ptr) {
            sb->buf = (char*)new_ptr;
            sb->capacity = new_cap;
        }
    }
    if (!sb->buf || sb->capacity < required) {
        sb->failed = true;
        return false;
    }
    return true;
}

void sb_append(StringBuilder* sb, const char* str) {
    if (!str) return;
    sb_append_len(sb, str, strlen(str));
}

void sb_append_len(StringBuilder* sb, const char* str, size_t len) {
    if (!str || len == 0) return;
    if (!sb_ensure_capacity(sb, len)) return;
    memcpy(sb->buf + sb->len, str, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

void sb_append_char(StringBuilder* sb, char c) {
    if (!sb_ensure_capacity(sb, 1)) return;
    sb->buf[sb->len] = c;
    sb->len += 1;
    sb->buf[sb->len] = '\0';
}

void sb_appendf(StringBuilder* sb, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sb_appendfv(sb, fmt, args);
    va_end(args);
}

void sb_appendfv(StringBuilder* sb, const char* fmt, va_list args) {
    if (!sb || sb->failed) return;
    if (!fmt) {
        sb->failed = true;
        return;
    }

    va_list args_copy;
    va_copy(args_copy, args);

    // Dry run to get formatted string length
    int formatted_len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (formatted_len < 0) {
        sb->failed = true;
        return;
    }
    if (formatted_len == 0) return;

    if (!sb_ensure_capacity(sb, (size_t)formatted_len)) return;

    int written = vsnprintf(
        sb->buf + sb->len, (size_t)formatted_len + 1U, fmt, args);
    if (written != formatted_len) {
        /* The sizing and formatting calls must agree.  A negative result is
         * an encoding error; a different non-negative result can occur if
         * formatting authority changes between the two calls (for example,
         * a concurrent locale change).  In either case retain the previous
         * valid prefix and make the failure observable to every caller. */
        sb->buf[sb->len] = '\0';
        sb->failed = true;
        return;
    }
    sb->len += (size_t)formatted_len;
}

bool sb_ok(const StringBuilder* sb) {
    return sb != NULL && !sb->failed;
}
char* sb_detach(StringBuilder* sb) {
    char* res = sb->buf;
    if (!res) {
        res = (char*)mem_alloc(1);
        if (res) res[0] = '\0';
        return res;
    }
    // We adjust the allocation size to match exactly the string length + 1
    size_t actual_size = sb->len + 1;
    void* new_ptr = mem_realloc(res, sb->capacity, actual_size);
    if (new_ptr) {
        res = (char*)new_ptr;
        mem_untrack_allocation(actual_size);
    } else {
        /* If realloc fails, retain the original allocation and transfer its
         * ownership using the original tracked capacity. */
        mem_untrack_allocation(sb->capacity);
    }
    
    sb_init(sb);
    return res;
}
