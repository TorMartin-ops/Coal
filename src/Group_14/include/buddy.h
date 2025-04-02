#ifndef BUDDY_H
#define BUDDY_H


#include "types.h" 
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the buddy allocator over a region [heap .. heap + size).
 * 
 * @param heap pointer to start of free region
 * @param size total size in bytes (prefer a power of two)
 */
void buddy_init(void *heap, size_t size);

/**
 * Allocates 'size' bytes from the buddy system (rounded up to next power of two).
 *
 * @param size number of bytes requested
 * @return pointer to allocated block, or NULL if out of memory
 */
void *buddy_alloc(size_t size);

/**
 * Frees a block previously allocated by buddy_alloc.
 *
 * @param ptr pointer from buddy_alloc
 * @param size original requested size
 */
void buddy_free(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // BUDDY_H
