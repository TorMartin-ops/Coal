# Coal OS API Reference

This section provides detailed API documentation for kernel developers and system programmers working with Coal OS.

## API Categories

### 1. [Memory Management API](memory-api.md)
- Physical memory allocation
- Virtual memory management
- Heap allocation (kmalloc)
- Slab allocator interface

### 2. [Process Management API](process-api.md)
- Process creation and control
- Thread management
- Scheduling functions
- Signal handling

### 3. [File System API](filesystem-api.md)
- VFS operations
- File I/O functions
- Directory operations
- Mount management

### 4. [Synchronization API](sync-api.md)
- Spinlocks
- Mutexes
- Semaphores
- Wait queues

### 5. [Driver API](driver-api.md)
- Device registration
- Interrupt handling
- DMA operations
- Port I/O

### 6. [System Call API](syscall-api.md)
- System call implementation
- User space access
- Security validation

## Common Data Types

### Basic Types
```c
// Integer types
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

// Size types
typedef uint32_t size_t;
typedef int32_t  ssize_t;
typedef uint32_t uintptr_t;
typedef int32_t  intptr_t;

// Process types
typedef int32_t  pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

// File types
typedef uint32_t mode_t;
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef int64_t  off_t;
```

### Error Codes
```c
// Success
#define E_SUCCESS       0

// Common errors
#define E_INVAL        -1   // Invalid argument
#define E_NOMEM        -2   // Out of memory
#define E_NOTFOUND     -3   // Not found
#define E_BUSY         -4   // Resource busy
#define E_PERM         -5   // Permission denied
#define E_EXIST        -6   // Already exists
#define E_FAULT        -7   // Bad address
#define E_IO           -8   // I/O error
#define E_NOSYS        -9   // Not implemented
#define E_AGAIN        -10  // Try again

// Error type
typedef int32_t error_t;
```

## Kernel Subsystem APIs

### Core Kernel
```c
// Panic and assertions
void kernel_panic(const char* fmt, ...) __attribute__((noreturn));
void kernel_assert(bool condition, const char* msg);

// Logging
void kprintf(const char* fmt, ...);
void klog(log_level_t level, const char* fmt, ...);

// Time
uint64_t get_system_ticks(void);
void delay_ms(uint32_t ms);
```

### Memory Management
```c
// Page allocation
void* page_alloc(void);
void page_free(void* page);

// Kernel heap
void* kmalloc(size_t size);
void* kmalloc_aligned(size_t size, size_t align);
void kfree(void* ptr);

// Virtual memory
int vm_map(uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
void vm_unmap(uintptr_t vaddr);
```

### Process Management
```c
// Current process
process_t* current_process(void);
thread_t* current_thread(void);

// Process control
pid_t process_create(const char* path, char* const argv[]);
void process_exit(int status);
int process_wait(pid_t pid);

// Scheduling
void yield(void);
void sleep(uint32_t ms);
```

### File System
```c
// File operations
int vfs_open(const char* path, int flags);
ssize_t vfs_read(int fd, void* buf, size_t count);
ssize_t vfs_write(int fd, const void* buf, size_t count);
int vfs_close(int fd);

// Directory operations
int vfs_mkdir(const char* path, mode_t mode);
int vfs_rmdir(const char* path);
```

## Coding Conventions

### Naming Conventions
- **Functions**: `lowercase_with_underscores`
- **Types**: `lowercase_t` suffix for typedefs
- **Constants**: `UPPERCASE_WITH_UNDERSCORES`
- **Struct members**: `lowercase_with_underscores`

### Function Prefixes
- `k*` - Kernel internal functions
- `sys_*` - System call implementations
- `vfs_*` - Virtual file system
- `mm_*` - Memory management
- `sched_*` - Scheduler
- `irq_*` - Interrupt handling

### Error Handling
```c
// Return negative error codes
int function_that_can_fail(void) {
    if (error_condition) {
        return -E_INVAL;
    }
    return E_SUCCESS;
}

// Check for errors
int result = function_that_can_fail();
if (result < 0) {
    // Handle error
}
```

### Memory Allocation
```c
// Always check allocation results
void* ptr = kmalloc(size);
if (!ptr) {
    return -E_NOMEM;
}

// Always free allocated memory
kfree(ptr);
```

## Header File Organization

### Include Guards
```c
#ifndef KERNEL_MODULE_H
#define KERNEL_MODULE_H

// Header content

#endif /* KERNEL_MODULE_H */
```

### Include Order
1. System headers (`<libc/*.h>`)
2. Kernel headers (`<kernel/*.h>`)
3. Local headers (`"module.h"`)

### Forward Declarations
```c
// Prefer forward declarations to includes
struct process;  // Instead of #include <kernel/process.h>
typedef struct process process_t;
```

## Debugging Support

### Debug Macros
```c
#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) \
    kprintf("[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) ((void)0)
#endif

// Assertion with message
#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kernel_panic("Assertion failed: %s at %s:%d", \
                        #cond, __FILE__, __LINE__); \
        } \
    } while (0)
```

### Debug Functions
```c
// Memory debugging
void mem_dump(void* addr, size_t len);
void mem_check_leaks(void);

// Process debugging
void process_dump_info(process_t* proc);
void thread_backtrace(thread_t* thread);

// Statistics
void mm_print_stats(void);
void sched_print_stats(void);
```

## Performance Guidelines

### Optimization Hints
```c
// Likely/unlikely branch hints
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// Usage
if (likely(common_case)) {
    // Fast path
} else {
    // Slow path
}
```

### Cache Alignment
```c
// Align to cache line
struct my_struct {
    // ... fields ...
} __attribute__((aligned(64)));

// Prevent false sharing
struct per_cpu_data {
    uint32_t counter;
    char padding[60];  // Pad to cache line
} __attribute__((packed));
```

## Thread Safety

### Locking Rules
1. Always disable interrupts when taking spinlocks
2. Never sleep while holding a spinlock
3. Acquire locks in consistent order
4. Use reader-writer locks for read-heavy data

### Example
```c
// Correct spinlock usage
void update_shared_data(void) {
    unsigned long flags;
    spin_lock_irqsave(&data_lock, flags);
    
    // Critical section
    shared_data++;
    
    spin_unlock_irqrestore(&data_lock, flags);
}
```