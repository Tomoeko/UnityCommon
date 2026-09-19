// SPDX-License-Identifier: GPL-3.0-only
#include "io/typetree_value_digest.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Check failed at %d: %s\n", __LINE__, #x);                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    uint8_t baseline[32], changed[32];
    TypeTreeValue scalar = {
        .name = "value", .type_str = "SInt64", .type = VAL_TYPE_INT, .int_val = -7};
    CHECK(typetree_value_digest(&scalar, baseline));
    /* Golden bytes generated independently from the documented v1 encoding. */
    const char *golden = "eae46d749a850c99d1144f39e2c4656a9957a5717f85f1f59e237f18263e22f2";
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(baseline, hex);
    CHECK(strcmp(hex, golden) == 0);
    scalar.integer_is_unsigned = true;
    scalar.uint_val = (uint64_t)-7;
    CHECK(typetree_value_digest(&scalar, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    scalar.type = VAL_TYPE_FLOAT;
    scalar.type_str = "double";
    scalar.float_val = 0.0;
    CHECK(typetree_value_digest(&scalar, baseline));
    scalar.float_val = -0.0;
    CHECK(typetree_value_digest(&scalar, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    uint64_t nan_bits = UINT64_C(0x7ff8000000000001);
    memcpy(&scalar.float_val, &nan_bits, 8);
    CHECK(typetree_value_digest(&scalar, baseline));
    nan_bits++;
    memcpy(&scalar.float_val, &nan_bits, 8);
    CHECK(typetree_value_digest(&scalar, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);

    const uint8_t bytes[] = {0, 255};
    TypeTreeValue elements[2] = {
        {.name = "data",
         .type_str = "UInt8",
         .type = VAL_TYPE_INT,
         .integer_is_unsigned = true,
         .uint_val = 0},
        {.name = "data",
         .type_str = "UInt8",
         .type = VAL_TYPE_INT,
         .integer_is_unsigned = true,
         .uint_val = 255},
    };
    TypeTreeValue array = {
        .name = "bytes",
        .type_str = "vector",
        .type = VAL_TYPE_ARRAY,
        .array_val = {.count = 2, .storage = TYPETREE_ARRAY_PACKED_BYTES, .packed_bytes = bytes}};
    CHECK(typetree_value_digest(&array, baseline));
    array.array_val.storage = TYPETREE_ARRAY_VALUES;
    array.array_val.elements = elements;
    CHECK(typetree_value_digest(&array, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);
    elements[1].uint_val--;
    CHECK(typetree_value_digest(&array, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    array.array_val.count = 0;
    CHECK(typetree_value_digest(&array, baseline));
    array.array_val.storage = TYPETREE_ARRAY_PACKED_BYTES;
    CHECK(typetree_value_digest(&array, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);
    array.array_val.count = 1048576;
    CHECK(!typetree_value_digest(&array, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);

    TypeTreeValue record = {.name = "state",
                            .type_str = "State",
                            .type = VAL_TYPE_STRUCT,
                            .struct_val = {.count = 2, .members = elements}};
    CHECK(typetree_value_digest(&record, baseline));
    TypeTreeValue swap = elements[0];
    elements[0] = elements[1];
    elements[1] = swap;
    CHECK(typetree_value_digest(&record, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    memcpy(baseline, changed, 32);
    record.struct_val.members = &record;
    record.struct_val.count = 1;
    CHECK(!typetree_value_digest(&record, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);
    record.struct_val.count = -1;
    CHECK(!typetree_value_digest(&record, changed));
    TypeTreeValue text = {.name = "name",
                          .type_str = "string",
                          .type = VAL_TYPE_STRING,
                          .string_val = "abc",
                          .string_length = 2};
    CHECK(!typetree_value_digest(&text, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);
    text.string_length = 3;
    CHECK(typetree_value_digest(&text, changed));
    CHECK(!typetree_value_digest(NULL, changed));
    CHECK(!typetree_value_digest(&text, NULL));
    puts("TypeTree value digests passed");
    return 0;
}
