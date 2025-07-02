/**
 * @file memory_allocator.h
 * @brief Abstract memory allocator interface for Coal OS
 * 
 * This interface follows the Dependency Inversion Principle by providing
 * an abstraction for memory allocation that can be implemented by different
 * allocators (buddy, slab, kmalloc, etc.)
 */

#ifndef COAL_INTERFACES_MEMORY_ALLOCATOR_H
#define COAL_INTERFACES_MEMORY_ALLOCATOR_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Memory allocation flags
 */
typedef enum {
    ALLOC_FLAG_ZERO     = (1 << 0),  // Zero the allocated memory
    ALLOC_FLAG_ATOMIC   = (1 << 1),  // Atomic allocation (no sleep)
    ALLOC_FLAG_DMA      = (1 << 2),  // DMA-compatible memory
    ALLOC_FLAG_KERNEL   = (1 << 3),  // Kernel memory
    ALLOC_FLAG_USER     = (1 << 4),  // User memory
} alloc_flags_t;

/**
 * @brief Memory allocation statistics
 */
typedef struct {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t allocation_count;
    size_t free_count;
} alloc_stats_t;

/**
 * @brief Abstract memory allocator interface
 * 
 * Different allocators (buddy, slab, heap) can implement this interface
 */
typedef struct memory_allocator_interface {
    /**
     * @brief Allocate memory
     * @param size Size in bytes
     * @param flags Allocation flags
     * @return Pointer to allocated memory or NULL on failure
     */
    void* (*alloc)(size_t size, alloc_flags_t flags);
    
    /**
     * @brief Free memory
     * @param ptr Pointer to memory to free
     */
    void (*free)(void* ptr);
    
    /**
     * @brief Reallocate memory
     * @param ptr Existing pointer
     * @param new_size New size
     * @param flags Allocation flags
     * @return Pointer to reallocated memory or NULL on failure
     */
    void* (*realloc)(void* ptr, size_t new_size, alloc_flags_t flags);
    
    /**
     * @brief Get allocation size
     * @param ptr Pointer to allocated memory
     * @return Size of allocation or 0 if invalid
     */
    size_t (*get_size)(void* ptr);
    
    /**
     * @brief Get allocator statistics
     * @param stats Output statistics structure
     */
    void (*get_stats)(alloc_stats_t* stats);
    
    /**
     * @brief Initialize the allocator
     * @return E_SUCCESS or error code
     */
    error_t (*init)(void);
    
    /**
     * @brief Cleanup allocator
     */
    void (*cleanup)(void);
    
    /**
     * @brief Allocator name/identifier
     */
    const char* name;
    
    /**
     * @brief Minimum allocation size
     */
    size_t min_size;
    
    /**
     * @brief Maximum allocation size
     */
    size_t max_size;
    
    /**
     * @brief Alignment requirements
     */
    size_t alignment;
    
} memory_allocator_interface_t;

/**
 * @brief Global allocator instances (dependency injection points)
 */
extern memory_allocator_interface_t* g_kernel_allocator;
extern memory_allocator_interface_t* g_user_allocator;
extern memory_allocator_interface_t* g_dma_allocator;

/**
 * @brief Set allocator implementations
 */
void memory_set_kernel_allocator(memory_allocator_interface_t* allocator);
void memory_set_user_allocator(memory_allocator_interface_t* allocator);
void memory_set_dma_allocator(memory_allocator_interface_t* allocator);

/**
 * @brief Convenience functions that use injected allocators
 */
static inline void* kmalloc(size_t size) {
    if (g_kernel_allocator && g_kernel_allocator->alloc) {
        return g_kernel_allocator->alloc(size, ALLOC_FLAG_KERNEL);
    }
    return NULL;
}

static inline void* kzalloc(size_t size) {
    if (g_kernel_allocator && g_kernel_allocator->alloc) {
        return g_kernel_allocator->alloc(size, ALLOC_FLAG_KERNEL | ALLOC_FLAG_ZERO);
    }
    return NULL;
}

static inline void kfree(void* ptr) {
    if (g_kernel_allocator && g_kernel_allocator->free && ptr) {
        g_kernel_allocator->free(ptr);
    }
}

static inline void* krealloc(void* ptr, size_t new_size) {
    if (g_kernel_allocator && g_kernel_allocator->realloc) {
        return g_kernel_allocator->realloc(ptr, new_size, ALLOC_FLAG_KERNEL);
    }
    return NULL;
}

static inline void* umalloc(size_t size) {
    if (g_user_allocator && g_user_allocator->alloc) {
        return g_user_allocator->alloc(size, ALLOC_FLAG_USER);
    }
    return NULL;
}

static inline void ufree(void* ptr) {
    if (g_user_allocator && g_user_allocator->free && ptr) {
        g_user_allocator->free(ptr);
    }
}

static inline void* dma_alloc(size_t size) {
    if (g_dma_allocator && g_dma_allocator->alloc) {
        return g_dma_allocator->alloc(size, ALLOC_FLAG_DMA | ALLOC_FLAG_KERNEL);
    }
    return NULL;
}

static inline void dma_free(void* ptr) {
    if (g_dma_allocator && g_dma_allocator->free && ptr) {
        g_dma_allocator->free(ptr);
    }
}

#endif // COAL_INTERFACES_MEMORY_ALLOCATOR_H