#pragma once
#ifndef BUDDY_H
#define BUDDY_H

#include "libc/stddef.h"   // For size_t and NULL
#include "libc/stdint.h"   // For uint32_t, etc.
#include "libc/stdbool.h"  // For bool, true, false

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the buddy allocator over a given heap region.
 *
 * The heap region must be a power of two in size and at least as large as the minimum block size.
 *
 * @param heap  A pointer to the start of the heap region.
 * @param size  The total size of the heap region.
 */
void buddy_init(void *heap, size_t size);

/**
 * @brief Allocates a memory block from the buddy system.
 *
 * The requested size is rounded up to the next power of two. If a block of the appropriate
 * size is available (or can be split from a larger block), a pointer to the block is returned.
 *
 * @param size The number of bytes requested.
 * @return A pointer to the allocated block, or NULL if allocation fails.
 */
void *buddy_alloc(size_t size);

/**
 * @brief Frees a previously allocated block.
 *
 * The original requested size must be provided so that the buddy system can determine
 * the block's order and attempt to merge it with its buddy.
 *
 * @param ptr  The pointer returned by buddy_alloc.
 * @param size The original requested size.
 */
void buddy_free(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // BUDDY_H
