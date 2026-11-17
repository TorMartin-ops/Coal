/**
 * @file kmalloc.h
 * @brief General-purpose kernel allocator.
 *
 * Requests up to 1 KiB come from per-CPU slab caches (16..1024 bytes, powers
 * of two); larger ones are whole pages from the buddy allocator. There is no
 * per-allocation header: kfree() tells the two apart by alignment (slab
 * objects are never page aligned). Memory is not cleared.
 */
#ifndef KMALLOC_H
#define KMALLOC_H

#include <kernel/core/types.h>

/** Create the slab caches. Called once by memory_init(). */
void kmalloc_init(void);

/** Allocate at least size bytes, 8-byte aligned (page aligned above 1 KiB). NULL for size 0 or on failure. */
void *kmalloc(size_t size);

/** Free a pointer from kmalloc(); NULL is ignored. */
void kfree(void *ptr);

#endif /* KMALLOC_H */
