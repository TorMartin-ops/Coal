#ifndef BUDDY_H
#define BUDDY_H

#include "types.h"

// Define configuration constants here so they are accessible via the header
#define MIN_ORDER 5  // Smallest block order (2^5 = 32 bytes)
#define MAX_ORDER 22 // Largest block order (2^22 = 4MB) - ADJUST AS NEEDED

#ifdef __cplusplus
extern "C" {
#endif

// Function declarations remain the same
void buddy_init(void *heap, size_t size);
void *buddy_alloc(size_t size);
void buddy_free(void *ptr, size_t size);
size_t buddy_free_space(void);

#ifdef __cplusplus
}
#endif

#endif // BUDDY_H