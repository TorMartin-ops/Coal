#ifndef PERCPU_ALLOC_H
#define PERCPU_ALLOC_H

#include "types.h" 

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes per-CPU slab caches for small allocations up to 4096 bytes.
 * Must be called once during system startup.
 */
void percpu_kmalloc_init(void);

/**
 * Allocates 'size' bytes from the per-CPU caches for 'cpu_id' if <= 4096,
 * else buddy fallback. If cpu_id is invalid, fallback to buddy too.
 *
 * @param size   number of bytes
 * @param cpu_id which CPU is requesting
 * @return pointer or NULL
 */
void *percpu_kmalloc(size_t size, int cpu_id);

/**
 * Frees memory previously allocated by percpu_kmalloc.
 *
 * @param ptr   pointer from percpu_kmalloc
 * @param size  original size
 * @param cpu_id same CPU that allocated
 */
void percpu_kfree(void *ptr, size_t size, int cpu_id);

#ifdef __cplusplus
}
#endif

#endif // PERCPU_ALLOC_H
