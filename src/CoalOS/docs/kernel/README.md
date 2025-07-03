# Kernel Subsystems

Coal OS kernel is organized into several major subsystems, each designed following SOLID principles for maintainability and extensibility.

## Overview

The kernel subsystems work together to provide a complete operating system environment:

```
┌─────────────────────────────────────────────────────────┐
│                    System Calls                         │
├─────────────┬─────────────┬─────────────┬──────────────┤
│   Memory    │   Process   │ File System │   Drivers    │
│ Management  │ Management  │    (VFS)    │              │
├─────────────┴─────────────┴─────────────┴──────────────┤
│              Hardware Abstraction Layer (HAL)           │
└─────────────────────────────────────────────────────────┘
```

## Major Subsystems

### 1. [Memory Management](memory.md)
Handles all aspects of memory allocation and virtual memory:
- **Paging**: Virtual to physical address translation
- **Frame Allocator**: Physical memory management
- **Buddy System**: Large contiguous allocations
- **Slab Allocator**: Efficient fixed-size allocations
- **kmalloc**: General purpose kernel allocator

### 2. [Process Management](process.md)
Manages processes and scheduling:
- **Process Creation**: Fork/exec model
- **Scheduler**: O(1) priority-based scheduling
- **Context Switching**: Efficient task switching
- **Signal Handling**: POSIX-style signals
- **IPC**: Inter-process communication

### 3. [File System](filesystem.md)
Provides file and directory operations:
- **VFS Layer**: Virtual filesystem abstraction
- **FAT Driver**: FAT16/32 implementation
- **Page Cache**: File data caching
- **Buffer Cache**: Disk block caching
- **Mount System**: Multiple filesystem support

### 4. [Device Drivers](drivers.md)
Hardware device support:
- **Keyboard Driver**: PS/2 keyboard input
- **Timer (PIT)**: System timing and scheduling
- **Serial Port**: Debug output and communication
- **Display**: VGA text mode support
- **Block Devices**: Disk access abstraction

### 5. [System Calls](syscalls.md)
User-kernel interface:
- **Linux Compatibility**: Compatible syscall numbers
- **Security**: Comprehensive validation
- **Categories**:
  - Process management (fork, exec, exit)
  - File I/O (open, read, write, close)
  - Directory operations (mkdir, rmdir, readdir)
  - Memory management (brk, mmap)

## Design Principles

### Modularity
Each subsystem is self-contained with clear interfaces:
```c
// Example: VFS driver interface
typedef struct vfs_driver {
    const char *fs_name;
    void *(*mount)(const char *device);
    int (*unmount)(void *fs_context);
    vnode_t *(*open)(void *fs_context, const char *path, int flags);
    int (*read)(file_t *file, void *buf, size_t len);
    int (*write)(file_t *file, const void *buf, size_t len);
    // ... more operations
} vfs_driver_t;
```

### Error Handling
Consistent error codes across subsystems:
```c
// Common error codes
#define E_SUCCESS       0
#define E_INVAL        -1  // Invalid argument
#define E_NOMEM        -2  // Out of memory
#define E_NOTFOUND     -3  // Not found
#define E_BUSY         -4  // Resource busy
#define E_PERM         -5  // Permission denied
```

### Synchronization
Thread-safe operations using:
- **Spinlocks**: Short critical sections
- **Interrupt Disabling**: Atomic operations
- **Lock-free algorithms**: Where applicable

### Security
Every subsystem implements:
- Input validation
- Bounds checking
- User/kernel separation
- Resource limits

## Inter-Subsystem Communication

### Dependency Graph
```
System Calls
    ├── Process Management
    │   ├── Memory Management
    │   └── Scheduler
    ├── File System
    │   ├── VFS
    │   ├── Page Cache
    │   └── Buffer Cache
    └── Device Drivers
        └── HAL
```

### Common Interfaces

#### Memory Allocation
```c
void* kmalloc(size_t size);
void kfree(void* ptr);
void* kmalloc_aligned(size_t size, size_t align);
```

#### Process Control
```c
pcb_t* process_create(const char* path);
void process_exit(int code);
int process_wait(pid_t pid);
```

#### File Operations
```c
file_t* vfs_open(const char* path, int flags);
ssize_t vfs_read(file_t* file, void* buf, size_t len);
ssize_t vfs_write(file_t* file, const void* buf, size_t len);
int vfs_close(file_t* file);
```

## Initialization Order

The kernel initializes subsystems in a specific order:

1. **CPU Setup**: GDT, IDT, TSS
2. **Memory Management**: Paging, allocators
3. **Device Drivers**: Timer, keyboard, serial
4. **File System**: VFS, mount root
5. **Process Management**: Scheduler, init process

## Performance Considerations

### Optimizations
- O(1) scheduler with bitmap operations
- Page cache for file I/O
- Slab allocator for common objects
- Lock-free data structures where possible

### Benchmarking
Key metrics tracked:
- Context switch time
- System call overhead
- Memory allocation performance
- File I/O throughput

## Debugging Support

### Serial Output
All subsystems use serial port for debug:
```c
serial_printf("[SubSystem] Operation: %s\n", details);
```

### Assertions
Extensive use of assertions:
```c
KERNEL_ASSERT(condition, "Error message");
```

### Statistics
Runtime statistics available:
- Memory usage
- Process counts
- Cache hit rates
- Scheduler statistics