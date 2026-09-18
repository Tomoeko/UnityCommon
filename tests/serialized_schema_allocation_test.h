#ifndef SERIALIZED_SCHEMA_ALLOCATION_TEST_H
#define SERIALIZED_SCHEMA_ALLOCATION_TEST_H

#include <stddef.h>
#include <stdlib.h>

/* Forced into only the unit's separate product-source object, after standard
 * allocation declarations. Schema uses malloc/free, never calloc/realloc. */
void* schema_test_allocate(size_t size);
void schema_test_release(void* allocation);

#define malloc schema_test_allocate
#define free schema_test_release

#endif
