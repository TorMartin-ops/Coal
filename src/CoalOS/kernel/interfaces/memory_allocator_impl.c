/**
 * @file memory_allocator_impl.c
 * @brief Memory allocator interface implementations and dependency injection
 */

#include <kernel/interfaces/memory_allocator.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/slab.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/memory/paging.h>

// Global allocator instances (dependency injection)
memory_allocator_interface_t* g_kernel_allocator = NULL;
memory_allocator_interface_t* g_user_allocator = NULL;
memory_allocator_interface_t* g_dma_allocator = NULL;

// Statistics tracking
static alloc_stats_t kmalloc_stats = {0};
static alloc_stats_t buddy_stats = {0};
static spinlock_t stats_lock = {0};

// Kmalloc-based allocator implementation
static void* kmalloc_alloc(size_t size, alloc_flags_t flags) {
    void* ptr = NULL;
    
    if (flags & ALLOC_FLAG_ZERO) {
        ptr = kzalloc(size);
    } else {
        ptr = kmalloc(size);
    }
    
    if (ptr) {
        uintptr_t irq_flags = spinlock_acquire_irqsave(&stats_lock);
        kmalloc_stats.total_allocated += size;
        kmalloc_stats.current_usage += size;
        if (kmalloc_stats.current_usage > kmalloc_stats.peak_usage) {
            kmalloc_stats.peak_usage = kmalloc_stats.current_usage;
        }
        kmalloc_stats.allocation_count++;
        spinlock_release_irqrestore(&stats_lock, irq_flags);
    }
    
    return ptr;
}

static void kmalloc_free(void* ptr) {
    if (!ptr) return;
    
    // Note: We can't easily get the size from kmalloc, so we approximate
    size_t estimated_size = 64; // This is a rough estimate
    
    kfree(ptr);
    
    uintptr_t flags = spinlock_acquire_irqsave(&stats_lock);
    kmalloc_stats.total_freed += estimated_size;
    if (kmalloc_stats.current_usage >= estimated_size) {
        kmalloc_stats.current_usage -= estimated_size;
    }
    kmalloc_stats.free_count++;
    spinlock_release_irqrestore(&stats_lock, flags);
}

static void* kmalloc_realloc(void* ptr, size_t new_size, alloc_flags_t flags) {
    // Simple implementation - allocate new, copy, free old
    void* new_ptr = kmalloc_alloc(new_size, flags);
    if (new_ptr && ptr) {
        // We don't know the old size, so we copy a reasonable amount
        size_t copy_size = new_size > 512 ? 512 : new_size;
        memcpy(new_ptr, ptr, copy_size);
        kmalloc_free(ptr);
    }
    return new_ptr;
}

static size_t kmalloc_get_size(void* ptr) {
    // Kmalloc doesn't provide size information
    (void)ptr;
    return 0;
}

static void kmalloc_get_stats(alloc_stats_t* stats) {
    if (!stats) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&stats_lock);
    *stats = kmalloc_stats;
    spinlock_release_irqrestore(&stats_lock, flags);
}

static error_t kmalloc_init_wrapper(void) {
    spinlock_init(&stats_lock);
    // The actual kmalloc_init is called elsewhere
    return E_SUCCESS;
}

static void kmalloc_cleanup(void) {
    // Nothing to cleanup for kmalloc
}

// Buddy allocator wrapper implementation
static void* buddy_alloc_wrapper(size_t size, alloc_flags_t flags) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void* ptr = buddy_alloc(pages);
    
    if (ptr && (flags & ALLOC_FLAG_ZERO)) {
        memset(ptr, 0, pages * PAGE_SIZE);
    }
    
    if (ptr) {
        uintptr_t irq_flags = spinlock_acquire_irqsave(&stats_lock);
        buddy_stats.total_allocated += pages * PAGE_SIZE;
        buddy_stats.current_usage += pages * PAGE_SIZE;
        if (buddy_stats.current_usage > buddy_stats.peak_usage) {
            buddy_stats.peak_usage = buddy_stats.current_usage;
        }
        buddy_stats.allocation_count++;
        spinlock_release_irqrestore(&stats_lock, irq_flags);
    }
    
    return ptr;
}

static void buddy_free_wrapper(void* ptr) {
    if (!ptr) return;
    
    // For buddy allocator, we need to know the size to free properly
    // This is a limitation of the current interface
    buddy_free(ptr);
    
    uintptr_t flags = spinlock_acquire_irqsave(&stats_lock);
    buddy_stats.total_freed += PAGE_SIZE;
    if (buddy_stats.current_usage >= PAGE_SIZE) {
        buddy_stats.current_usage -= PAGE_SIZE;
    }
    buddy_stats.free_count++;
    spinlock_release_irqrestore(&stats_lock, flags);
}

static void buddy_get_stats_wrapper(alloc_stats_t* stats) {
    if (!stats) return;
    
    buddy_stats_t buddy_info;
    buddy_get_stats(&buddy_info);
    
    // Convert buddy_stats_t to alloc_stats_t
    stats->total_allocated = buddy_info.total_bytes - buddy_info.free_bytes;
    stats->current_usage = buddy_info.total_bytes - buddy_info.free_bytes;
    stats->peak_usage = stats->current_usage; // Buddy doesn't track peak
    stats->allocation_count = buddy_info.alloc_count;
    stats->free_count = buddy_info.free_count;
    // stats->failed_allocations = buddy_info.failed_alloc_count; // Not in alloc_stats_t
}

static error_t buddy_init_wrapper(void) {
    // Buddy allocator is initialized elsewhere
    return E_SUCCESS;
}

// Kmalloc allocator interface
static memory_allocator_interface_t kmalloc_allocator = {
    .alloc = kmalloc_alloc,
    .free = kmalloc_free,
    .realloc = kmalloc_realloc,
    .get_size = kmalloc_get_size,
    .get_stats = kmalloc_get_stats,
    .init = kmalloc_init_wrapper,
    .cleanup = kmalloc_cleanup,
    .name = "kmalloc",
    .min_size = 1,
    .max_size = 1024 * 1024, // 1MB limit for kmalloc
    .alignment = sizeof(void*),
};

// Buddy allocator interface
static memory_allocator_interface_t buddy_allocator = {
    .alloc = buddy_alloc_wrapper,
    .free = buddy_free_wrapper,
    .realloc = NULL, // Buddy allocator doesn't support realloc
    .get_size = NULL, // Buddy allocator doesn't track sizes per allocation
    .get_stats = buddy_get_stats_wrapper,
    .init = buddy_init_wrapper,
    .cleanup = NULL,
    .name = "buddy",
    .min_size = PAGE_SIZE,
    .max_size = SIZE_MAX,
    .alignment = PAGE_SIZE,
};

void memory_set_kernel_allocator(memory_allocator_interface_t* allocator) {
    g_kernel_allocator = allocator;
    if (g_kernel_allocator && g_kernel_allocator->init) {
        g_kernel_allocator->init();
    }
}

void memory_set_user_allocator(memory_allocator_interface_t* allocator) {
    g_user_allocator = allocator;
    if (g_user_allocator && g_user_allocator->init) {
        g_user_allocator->init();
    }
}

void memory_set_dma_allocator(memory_allocator_interface_t* allocator) {
    g_dma_allocator = allocator;
    if (g_dma_allocator && g_dma_allocator->init) {
        g_dma_allocator->init();
    }
}

// Convenience functions to get specific allocator implementations
memory_allocator_interface_t* memory_get_kmalloc_impl(void) {
    return &kmalloc_allocator;
}

memory_allocator_interface_t* memory_get_buddy_impl(void) {
    return &buddy_allocator;
}

// Initialize default allocators
void memory_init_allocators(void) {
    // Set up default allocators
    memory_set_kernel_allocator(&kmalloc_allocator);
    memory_set_dma_allocator(&buddy_allocator);
    // User allocator will be set up when user space is implemented
}