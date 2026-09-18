#ifndef TEST_SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_H
#define TEST_SERIALIZED_FILE_MANAGED_VALUES_ALLOCATION_H

#include <stddef.h>
#include <stdlib.h>

/* Force into the separate managed-values, schema and index objects only. */
void* managed_values_test_allocate(size_t size);
void managed_values_test_release(void* allocation);

#ifndef MANAGED_VALUES_ALLOCATION_DECLARATIONS_ONLY
#define malloc managed_values_test_allocate
#define free managed_values_test_release
#endif

#endif
