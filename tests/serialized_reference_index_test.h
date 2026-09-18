#ifndef SERIALIZED_REFERENCE_INDEX_TEST_H
#define SERIALIZED_REFERENCE_INDEX_TEST_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Forced only into a separate product-source test object. Normal source has
 * no callback, exported setter, macro seam or environment-dependent behavior. */
void* reference_index_test_allocate(size_t size);
void reference_index_test_release(void* allocation);
int reference_index_test_compare(const void* left, const void* right, size_t size);

#define malloc reference_index_test_allocate
#define free reference_index_test_release
#define memcmp reference_index_test_compare

#endif
