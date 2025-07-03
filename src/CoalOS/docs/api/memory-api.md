# Memory Management API

This document describes the memory management APIs available in Coal OS kernel.

## Overview

Coal OS provides a layered memory management system:
- **Physical Memory**: Frame allocator with bitmap tracking
- **Virtual Memory**: Page tables with demand paging
- **Heap Allocators**: Buddy, slab, and general purpose allocators

## Physical Memory Management

### Frame Allocation

```c
/**
 * @brief Allocate a physical frame
 * @return Physical address of frame or 0 on failure
 */
uintptr_t frame_alloc(void);

/**
 * @brief Free a physical frame
 * @param frame Physical address of frame to free
 */
void frame_free(uintptr_t frame);

/**
 * @brief Get total physical memory size
 * @return Total memory in bytes
 */
size_t frame_get_total_memory(void);

/**
 * @brief Get free physical memory
 * @return Free memory in bytes
 */
size_t frame_get_free_memory(void);
```

### Frame Statistics

```c
typedef struct {
    size_t total_frames;      // Total physical frames
    size_t free_frames;       // Currently free frames
    size_t reserved_frames;   // Reserved by kernel
    size_t user_frames;       // Allocated to user space
} frame_stats_t;

/**
 * @brief Get frame allocator statistics
 * @param stats Pointer to statistics structure
 */
void frame_get_stats(frame_stats_t *stats);
```

## Virtual Memory Management

### Page Mapping

```c
/**
 * @brief Map a virtual page to physical frame
 * @param vaddr Virtual address (page-aligned)
 * @param paddr Physical address (page-aligned)
 * @param flags Page flags (PAGE_PRESENT, PAGE_WRITABLE, etc.)
 * @return 0 on success, negative error code on failure
 */
int paging_map_page(uintptr_t vaddr, uintptr_t paddr, uint32_t flags);

/**
 * @brief Unmap a virtual page
 * @param vaddr Virtual address to unmap
 */
void paging_unmap_page(uintptr_t vaddr);

/**
 * @brief Get physical address for virtual address
 * @param vaddr Virtual address
 * @return Physical address or 0 if not mapped
 */
uintptr_t paging_get_physical(uintptr_t vaddr);
```

### Page Flags

```c
#define PAGE_PRESENT    0x001   // Page is present in memory
#define PAGE_WRITABLE   0x002   // Page is writable
#define PAGE_USER       0x004   // Accessible from user mode
#define PAGE_WRITE_THROUGH 0x008 // Write-through caching
#define PAGE_CACHE_DISABLE 0x010 // Disable caching
#define PAGE_ACCESSED   0x020   // Page has been accessed
#define PAGE_DIRTY      0x040   // Page has been written
#define PAGE_HUGE       0x080   // 4MB page (PSE)
#define PAGE_GLOBAL     0x100   // Global page (PGE)
#define PAGE_COW        0x200   // Copy-on-write (software flag)
#define PAGE_NX         0x8000000000000000ULL // No execute
```

### Page Tables

```c
/**
 * @brief Create new page table for process
 * @return Page directory physical address
 */
page_directory_t* paging_create_directory(void);

/**
 * @brief Clone page directory (for fork)
 * @param src Source page directory
 * @return Cloned page directory or NULL on failure
 */
page_directory_t* paging_clone_directory(page_directory_t *src);

/**
 * @brief Free page directory
 * @param dir Page directory to free
 */
void paging_free_directory(page_directory_t *dir);

/**
 * @brief Switch to page directory
 * @param dir Page directory to switch to
 */
void paging_switch_directory(page_directory_t *dir);
```

## Heap Management

### Buddy Allocator

For large allocations (4KB to 16MB):

```c
/**
 * @brief Initialize buddy allocator
 * @param start Start address of buddy heap
 * @param size Size of buddy heap
 */
void buddy_init(void *start, size_t size);

/**
 * @brief Allocate from buddy allocator
 * @param size Size to allocate (will be rounded up to power of 2)
 * @return Allocated memory or NULL
 */
void* buddy_alloc(size_t size);

/**
 * @brief Free buddy allocation
 * @param ptr Pointer to free
 */
void buddy_free(void *ptr);

/**
 * @brief Get buddy allocator statistics
 * @param stats Statistics structure to fill
 */
void buddy_get_stats(buddy_stats_t *stats);
```

### Slab Allocator

For fixed-size object caching:

```c
/**
 * @brief Create a slab cache
 * @param name Cache name for debugging
 * @param size Object size
 * @param align Object alignment
 * @param ctor Constructor function (optional)
 * @param dtor Destructor function (optional)
 * @return Slab cache or NULL on failure
 */
slab_cache_t* slab_cache_create(const char *name, size_t size, 
                                size_t align, 
                                void (*ctor)(void *),
                                void (*dtor)(void *));

/**
 * @brief Destroy slab cache
 * @param cache Cache to destroy
 */
void slab_cache_destroy(slab_cache_t *cache);

/**
 * @brief Allocate from slab cache
 * @param cache Cache to allocate from
 * @return Object pointer or NULL
 */
void* slab_alloc(slab_cache_t *cache);

/**
 * @brief Free to slab cache
 * @param cache Cache to free to
 * @param ptr Object to free
 */
void slab_free(slab_cache_t *cache, void *ptr);

/**
 * @brief Shrink slab caches (reclaim memory)
 * @return Number of pages reclaimed
 */
size_t slab_shrink_caches(void);
```

### General Purpose Allocator

```c
/**
 * @brief Allocate kernel memory
 * @param size Size to allocate
 * @return Allocated memory or NULL
 */
void* kmalloc(size_t size);

/**
 * @brief Allocate aligned kernel memory
 * @param size Size to allocate
 * @param align Alignment requirement
 * @return Allocated memory or NULL
 */
void* kmalloc_aligned(size_t size, size_t align);

/**
 * @brief Allocate and zero kernel memory
 * @param size Size to allocate
 * @return Allocated memory or NULL
 */
void* kzalloc(size_t size);

/**
 * @brief Reallocate kernel memory
 * @param ptr Original pointer
 * @param size New size
 * @return Reallocated memory or NULL
 */
void* krealloc(void *ptr, size_t size);

/**
 * @brief Free kernel memory
 * @param ptr Memory to free
 */
void kfree(void *ptr);

/**
 * @brief Get allocation size
 * @param ptr Allocation pointer
 * @return Size of allocation
 */
size_t kmalloc_size(void *ptr);
```

## User Memory Access

### Safe Copy Functions

```c
/**
 * @brief Copy from user space
 * @param to Kernel buffer
 * @param from User buffer
 * @param n Number of bytes
 * @return 0 on success, -EFAULT on failure
 */
int copy_from_user(void *to, const void __user *from, size_t n);

/**
 * @brief Copy to user space
 * @param to User buffer
 * @param from Kernel buffer
 * @param n Number of bytes
 * @return 0 on success, -EFAULT on failure
 */
int copy_to_user(void __user *to, const void *from, size_t n);

/**
 * @brief Copy string from user space
 * @param to Kernel buffer
 * @param from User string
 * @param n Maximum bytes to copy
 * @return Number of bytes copied or negative error
 */
ssize_t strncpy_from_user(char *to, const char __user *from, size_t n);

/**
 * @brief Get string length in user space
 * @param str User string
 * @param max_len Maximum length to check
 * @return String length or negative error
 */
ssize_t strnlen_user(const char __user *str, size_t max_len);
```

### User Memory Validation

```c
/**
 * @brief Check if user pointer is valid
 * @param ptr User pointer
 * @param size Size to check
 * @param write True if checking for write access
 * @return True if valid, false otherwise
 */
bool validate_user_pointer(const void __user *ptr, size_t size, bool write);

/**
 * @brief Check if user buffer is valid
 * @param ptr Buffer pointer
 * @param size Buffer size
 * @param write True if checking for write access
 * @return True if valid, false otherwise
 */
bool validate_user_buffer(const void __user *ptr, size_t size, bool write);
```

## Memory Information

### System Memory Info

```c
typedef struct {
    size_t total_memory;      // Total system memory
    size_t free_memory;       // Currently free memory
    size_t kernel_memory;     // Memory used by kernel
    size_t user_memory;       // Memory used by user processes
    size_t cache_memory;      // Memory used for caches
    size_t buffer_memory;     // Memory used for buffers
} meminfo_t;

/**
 * @brief Get system memory information
 * @param info Memory info structure to fill
 */
void get_meminfo(meminfo_t *info);
```

### Process Memory Info

```c
typedef struct {
    size_t virt_size;        // Virtual memory size
    size_t rss;              // Resident set size
    size_t shared;           // Shared memory
    size_t text;             // Text segment size
    size_t data;             // Data segment size
    size_t stack;            // Stack size
} proc_meminfo_t;

/**
 * @brief Get process memory information
 * @param pid Process ID
 * @param info Memory info structure to fill
 * @return 0 on success, negative error code on failure
 */
int get_proc_meminfo(pid_t pid, proc_meminfo_t *info);
```

## Memory Debugging

### Debug Functions

```c
/**
 * @brief Dump memory region
 * @param addr Start address
 * @param size Size to dump
 */
void mem_dump(void *addr, size_t size);

/**
 * @brief Check for memory leaks
 * @return Number of leaked allocations
 */
size_t mem_check_leaks(void);

/**
 * @brief Enable allocation tracking
 */
void mem_enable_tracking(void);

/**
 * @brief Print allocation statistics
 */
void mem_print_stats(void);
```

### Memory Barriers

```c
/**
 * @brief Memory barrier
 */
static inline void mb(void) {
    asm volatile("mfence" ::: "memory");
}

/**
 * @brief Read memory barrier
 */
static inline void rmb(void) {
    asm volatile("lfence" ::: "memory");
}

/**
 * @brief Write memory barrier
 */
static inline void wmb(void) {
    asm volatile("sfence" ::: "memory");
}
```

## Usage Examples

### Basic Allocation

```c
// Allocate memory
void *buffer = kmalloc(1024);
if (!buffer) {
    return -ENOMEM;
}

// Use buffer...

// Free memory
kfree(buffer);
```

### Slab Cache Usage

```c
// Create cache for process structures
slab_cache_t *process_cache = slab_cache_create("process",
                                                sizeof(process_t),
                                                8, NULL, NULL);

// Allocate process
process_t *proc = slab_alloc(process_cache);
if (!proc) {
    return -ENOMEM;
}

// Initialize and use process...

// Free process
slab_free(process_cache, proc);
```

### Page Mapping

```c
// Allocate physical frame
uintptr_t phys = frame_alloc();
if (!phys) {
    return -ENOMEM;
}

// Map to virtual address
int result = paging_map_page(virt_addr, phys, 
                            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
if (result < 0) {
    frame_free(phys);
    return result;
}
```

### Safe User Access

```c
// Copy string from user space
char kernel_buf[256];
ssize_t len = strncpy_from_user(kernel_buf, user_string, sizeof(kernel_buf));
if (len < 0) {
    return len;  // Error
}

// Process string...
```

## Performance Tips

1. **Use appropriate allocator**:
   - kmalloc for general purpose
   - Slab for fixed-size objects
   - Buddy for large allocations

2. **Minimize allocations**: Reuse buffers when possible

3. **Batch operations**: Allocate multiple objects at once

4. **Cache alignment**: Align frequently accessed data

5. **NUMA awareness**: Consider memory locality (future)

## Error Handling

Common error codes:
- `-ENOMEM`: Out of memory
- `-EINVAL`: Invalid parameter
- `-EFAULT`: Bad address
- `-EACCES`: Permission denied