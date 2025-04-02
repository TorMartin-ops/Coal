#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h" 

#ifdef __cplusplus
extern "C" {
#endif

/**
 * kmalloc_init
 *
 * Initializes the kernel allocator. If USE_PERCPU_ALLOC is defined, 
 * calls percpu_kmalloc_init(); otherwise creates global slab caches.
 */
void kmalloc_init(void);

/**
 * kmalloc
 *
 * Allocates 'size' bytes. If size <= 4096 => slab or per-CPU slab. 
 * Otherwise => buddy. 
 *
 * @param size the requested size in bytes
 * @return pointer or NULL
 */
void *kmalloc(size_t size);

/**
 * kfree
 *
 * Frees memory previously allocated by kmalloc. 
 * The 'size' must match the original requested size.
 *
 * @param ptr  pointer from kmalloc
 * @param size same size used in kmalloc
 */
void kfree(void *ptr, size_t size);

/**
 * In global mode, you can optionally track usage stats. 
 * If that’s the case:
 */
void kmalloc_get_usage(uint32_t *out_alloc, uint32_t *out_free);

#ifdef __cplusplus
}
#endif

#endif // KMALLOC_H
