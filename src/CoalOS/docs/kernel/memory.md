# Memory Management

Coal OS implements a comprehensive memory management system with virtual memory support, multiple allocators, and efficient memory usage tracking.

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    kmalloc API                      │
├─────────────────────────────────────────────────────┤
│              Slab Allocator (Fixed Sizes)           │
├─────────────────────────────────────────────────────┤
│            Buddy Allocator (Large Blocks)           │
├─────────────────────────────────────────────────────┤
│              Frame Allocator (4KB Pages)            │
├─────────────────────────────────────────────────────┤
│                  Paging (MMU)                       │
├─────────────────────────────────────────────────────┤
│              Physical Memory (RAM)                  │
└─────────────────────────────────────────────────────┘
```

## Components

### 1. Paging System

The paging system provides virtual memory management:

#### Features
- 4KB page size
- Two-level page tables (x86 32-bit)
- Higher-half kernel mapping
- Demand paging support
- Copy-on-write (COW) pages

#### Key Functions
```c
// Enable paging
void paging_enable(void);

// Map virtual to physical address
int paging_map_page(uint32_t vaddr, uint32_t paddr, uint32_t flags);

// Unmap a virtual address
void paging_unmap_page(uint32_t vaddr);

// Create new page directory for process
page_directory_t* paging_create_directory(void);
```

#### Page Flags
```c
#define PAGE_PRESENT    0x001  // Page is present in memory
#define PAGE_WRITABLE   0x002  // Page is writable
#define PAGE_USER       0x004  // User-mode accessible
#define PAGE_ACCESSED   0x020  // Page has been accessed
#define PAGE_DIRTY      0x040  // Page has been written to
```

### 2. Frame Allocator

Manages physical memory frames (4KB blocks):

#### Implementation
- Bitmap-based allocation
- O(n) allocation (n = bitmap size)
- Tracks used/free frames
- Supports contiguous allocations

#### API
```c
// Allocate a single frame
uint32_t frame_alloc(void);

// Allocate contiguous frames
uint32_t frame_alloc_contiguous(size_t count);

// Free a frame
void frame_free(uint32_t frame);

// Get memory statistics
void frame_get_stats(frame_stats_t* stats);
```

### 3. Buddy Allocator

Handles large memory allocations efficiently:

#### Algorithm
- Binary buddy system
- Splits/merges blocks in powers of 2
- Reduces fragmentation
- O(log n) allocation/deallocation

#### Features
- Minimum block size: 4KB
- Maximum block size: 16MB
- Automatic coalescing of free blocks
- Internal fragmentation tracking

#### Usage
```c
// Allocate power-of-2 sized block
void* buddy_alloc(size_t size);

// Free a buddy block
void buddy_free(void* ptr);

// Get buddy statistics
buddy_stats_t buddy_get_stats(void);
```

### 4. Slab Allocator

Optimized for fixed-size allocations:

#### Design
- Pre-allocated object caches
- Eliminates fragmentation for fixed sizes
- Very fast allocation/deallocation
- Magazine layer for per-CPU caching

#### Slab Sizes
- 16, 32, 64, 128, 256, 512, 1024, 2048 bytes

#### API
```c
// Allocate from slab
void* slab_alloc(size_t size);

// Free to slab
void slab_free(void* ptr);

// Create custom slab cache
slab_cache_t* slab_cache_create(size_t object_size, const char* name);
```

### 5. kmalloc - General Purpose Allocator

The main kernel allocation interface:

#### Strategy
- Uses slab allocator for small allocations (≤ 2KB)
- Uses buddy allocator for large allocations (> 2KB)
- Tracks allocation metadata
- Supports alignment requirements

#### API
```c
// Allocate memory
void* kmalloc(size_t size);

// Allocate aligned memory
void* kmalloc_aligned(size_t size, size_t align);

// Free memory
void kfree(void* ptr);

// Get allocation size
size_t kmalloc_size(void* ptr);
```

## Memory Layout

### Virtual Memory Map
```
0xFFFFFFFF ┌─────────────────┐
           │ Kernel Reserved │
0xFFC00000 ├─────────────────┤ <- Recursive Page Directory
           │  Kernel Heap    │
0xFF000000 ├─────────────────┤
           │ Kernel Stacks   │
0xFE000000 ├─────────────────┤
           │  Device Memory  │
0xFD000000 ├─────────────────┤
           │  Kernel Code    │
           │  Kernel Data    │
0xC0000000 ├─────────────────┤ <- KERNEL_BASE
           │                 │
           │   User Space    │
           │                 │
0x00100000 ├─────────────────┤ <- User programs start
           │    Reserved     │
0x00000000 └─────────────────┘
```

### Physical Memory Zones
- **Low Memory** (0-1MB): BIOS, real mode
- **Normal Memory** (1MB-896MB): Kernel and user
- **High Memory** (>896MB): Special mapping required

## Memory Safety Features

### 1. Guard Pages
- Unmapped pages between allocations
- Catches buffer overflows
- Stack overflow detection

### 2. Validation
```c
// Validate user pointer
bool is_user_pointer(const void* ptr);

// Validate kernel pointer
bool is_kernel_pointer(const void* ptr);

// Safe memory access
int copy_from_user(void* dst, const void* src, size_t n);
int copy_to_user(void* dst, const void* src, size_t n);
```

### 3. Bounds Checking
- All allocations track size
- Detects out-of-bounds access
- Debug mode includes red zones

## Performance Optimizations

### 1. TLB Management
- Selective TLB invalidation
- Batched updates
- INVLPG instruction usage

### 2. Cache Coloring
- Slab allocator uses cache coloring
- Reduces cache conflicts
- Improves cache utilization

### 3. NUMA Awareness
- Per-CPU memory pools
- Local allocation preference
- Reduced lock contention

## Debugging Support

### Memory Tracking
```c
typedef struct {
    size_t total_memory;      // Total physical memory
    size_t used_memory;       // Currently allocated
    size_t free_memory;       // Available memory
    size_t kernel_memory;     // Kernel allocations
    size_t user_memory;       // User allocations
    size_t cache_memory;      // Page/buffer cache
} memory_stats_t;

void mm_get_stats(memory_stats_t* stats);
void mm_dump_allocations(void);
```

### Leak Detection
- Track allocation call sites
- Report unreferenced memory
- Allocation age tracking

### Common Issues and Solutions

#### Page Faults
```c
// Page fault handler
void page_fault_handler(isr_frame_t* frame) {
    uint32_t fault_addr = read_cr2();
    uint32_t error_code = frame->error_code;
    
    if (error_code & PAGE_PRESENT) {
        // Protection violation
    } else {
        // Page not present - handle demand paging
    }
}
```

#### Out of Memory
- Emergency memory reserve
- OOM killer for user processes
- Kernel panic for critical allocations

## Future Enhancements

1. **Huge Pages**: 2MB/4MB page support
2. **Memory Compression**: Compressed swap
3. **KSM**: Kernel same-page merging
4. **ASLR**: Address space layout randomization
5. **Memory Hotplug**: Dynamic memory addition