#ifndef SERIALIZED_VALUES_ALLOCATION_TEST_H
#define SERIALIZED_VALUES_ALLOCATION_TEST_H

#include <stddef.h>
#include <stdlib.h>

/* Force into the unit's separate values-source object only. The allocator
 * definitions include declarations without redirecting their own malloc/free. */
void* values_test_allocate(size_t size);
void values_test_release(void* allocation);

#ifndef SERIALIZED_VALUES_ALLOCATION_DECLARATIONS_ONLY
#define malloc values_test_allocate
#define free values_test_release
#endif

#endif
