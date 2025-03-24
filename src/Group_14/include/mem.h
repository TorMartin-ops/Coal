#ifndef MEM_H
#define MEM_H

#include <libc/stdint.h>
#include <libc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * init_kernel_memory
 *
 * Called early in the kernel, passing the address of 'end'
 * from your linker script to define where the kernel heap begins.
 */
void init_kernel_memory(uint32_t* kernel_end);

/**
 * malloc
 *
 * Allocates 'size' bytes from the kernel heap, returning a pointer.
 * Uses a naive bump pointer approach. Does not support real free-lists.
 */
void* malloc(size_t size);

/**
 * free
 *
 * A no-op in this naive bump allocator. 
 * For real deallocation, you'd implement a free-list or buddy system.
 */
void free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif // MEM_H
