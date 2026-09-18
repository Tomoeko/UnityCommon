#include "io/typetree.h"

#include "common/file_io.h"
#include "common/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OFFICIAL_TABLE_BYTES = 1170,
    CHECKED_OFFSET_COUNT = 8192
};

static const char expected_sha256[] =
    "4a6ece766a82003fcb86398159b54e5ae84de95b33752950c89b34ea6465444e";

static const char* expected_string(const CommonFileBytes* table, size_t offset) {
    if (offset >= table->size || (offset && table->data[offset - 1U] != 0U) ||
        !memchr(table->data + offset, 0, table->size - offset)) {
        return NULL;
    }
    return (const char*)table->data + offset;
}

static bool check_offset(const CommonFileBytes* table, uint32_t offset) {
    const char* expected = expected_string(table, offset);
    const char* actual = typetree_resolve_string(NULL, 0U, UINT32_C(0x80000000) | offset);
    if ((actual == NULL) != (expected == NULL) ||
        (expected && actual && strcmp(actual, expected))) {
        fprintf(stderr,
            "Common string offset %u: expected %s, observed %s\n",
            offset,
            expected ? expected : "<absent>",
            actual ? actual : "<absent>");
        return false;
    }
    return true;
}

static bool check_table(const CommonFileBytes* table) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(table->data, table->size, digest);
    common_sha256_digest_to_hex(digest, hex);
    if (table->size != OFFICIAL_TABLE_BYTES || strcmp(hex, expected_sha256)) {
        fprintf(stderr, "Official common-string fixture size/hash differs\n");
        return false;
    }

    bool passed = true;
    for (uint32_t offset = 0U; offset < CHECKED_OFFSET_COUNT; ++offset) {
        if (!check_offset(table, offset)) {
            passed = false;
        }
    }
    if (!check_offset(table, UINT32_C(0x7fffffff))) {
        passed = false;
    }

    /* The literal's final extra NUL is inside Unity's declared buffer. It
     * cannot be replaced by a name appended from a different Unity version. */
    const char* terminal =
        typetree_resolve_string(NULL, 0U, UINT32_C(0x80000000) | (OFFICIAL_TABLE_BYTES - 1U));
    if (!terminal || terminal[0] != '\0') {
        fprintf(stderr, "The official final common string must be empty\n");
        passed = false;
    }

    /* Correcting the shared table must not restrict names stored in a file's
     * local table. Their meaning is a separate schema-validation question. */
    static const uint8_t local_names[] = "RenderingLayerMask\0";
    const char* local = typetree_resolve_string(local_names, sizeof(local_names), 0U);
    if (!local || strcmp(local, "RenderingLayerMask")) {
        fprintf(stderr, "Local string resolution changed\n");
        passed = false;
    }
    return passed;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OFFICIAL_TABLE\n", argv[0]);
        return EXIT_FAILURE;
    }

    CommonFileBytes table = {0};
    const CommonFileStatus status = common_file_read_regular(argv[1], OFFICIAL_TABLE_BYTES, &table);
    if (status != COMMON_FILE_OK) {
        fprintf(stderr,
            "Cannot read official common-string fixture: %s\n",
            common_file_status_name(status));
        return EXIT_FAILURE;
    }
    const bool passed = check_table(&table);
    common_file_bytes_dispose(&table);
    if (!passed) {
        return EXIT_FAILURE;
    }
    printf(
        "common_offsets=%u terminal_empty=1 local_names_preserved=1\n", CHECKED_OFFSET_COUNT + 1U);
    return fflush(stdout) == 0 && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}
