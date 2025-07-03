# Coal OS Architecture Overview

## System Architecture

Coal OS is a monolithic kernel operating system designed for the x86 architecture. It follows a traditional Unix-like design with modern improvements.

```
┌─────────────────────────────────────────────────────────────┐
│                      User Space                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐      │
│  │  Shell  │  │  Hello  │  │   App   │  │   App   │      │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘      │
│       │            │            │            │             │
├───────┴────────────┴────────────┴────────────┴─────────────┤
│                    System Call Interface                     │
├─────────────────────────────────────────────────────────────┤
│                      Kernel Space                           │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                    VFS Layer                         │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐            │  │
│  │  │   FAT   │  │  Page   │  │  Buffer │            │  │
│  │  │  Driver │  │  Cache  │  │  Cache  │            │  │
│  │  └─────────┘  └─────────┘  └─────────┘            │  │
│  └─────────────────────────────────────────────────────┘  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              Process Management                      │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐            │  │
│  │  │Scheduler│  │ Process │  │  Signal │            │  │
│  │  │   O(1)  │  │ Creation│  │ Handler │            │  │
│  │  └─────────┘  └─────────┘  └─────────┘            │  │
│  └─────────────────────────────────────────────────────┘  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              Memory Management                       │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐            │  │
│  │  │  Paging │  │  Buddy  │  │  Slab   │            │  │
│  │  │         │  │Allocator│  │Allocator│            │  │
│  │  └─────────┘  └─────────┘  └─────────┘            │  │
│  └─────────────────────────────────────────────────────┘  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                Device Drivers                        │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐            │  │
│  │  │Keyboard │  │  Timer  │  │  Serial │            │  │
│  │  │         │  │  (PIT)  │  │  Port   │            │  │
│  │  └─────────┘  └─────────┘  └─────────┘            │  │
│  └─────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                         Hardware Layer
```

## Boot Process

1. **Bootloader (Limine)**: Loads kernel and sets up initial environment
2. **Early Boot**: Assembly code sets up stack and calls kernel main
3. **Kernel Initialization**:
   - CPU initialization (GDT, IDT, TSS)
   - Memory management setup
   - Device driver initialization
   - File system mounting
   - Process management initialization
   - Scheduler start

## Memory Layout

### Virtual Memory Map
```
0xFFFFFFFF ┌─────────────────┐
           │  Kernel Space   │
           │                 │
0xC0000000 ├─────────────────┤ <- KERNEL_BASE
           │                 │
           │   User Space    │
           │                 │
0x00000000 └─────────────────┘
```

### Physical Memory Management
- **Frame Allocator**: Manages physical pages
- **Buddy System**: Handles large contiguous allocations
- **Slab Allocator**: Efficient allocation for fixed-size objects

## Key Design Decisions

### 1. SOLID Principles
Every major subsystem is designed following SOLID principles:
- **Single Responsibility**: Each module has one clear purpose
- **Open/Closed**: Extensible through interfaces, not modification
- **Liskov Substitution**: Consistent interfaces throughout
- **Interface Segregation**: Small, focused interfaces
- **Dependency Inversion**: Dependencies on abstractions, not concretions

### 2. Modular Architecture
The kernel is organized into clear modules:
- `kernel/core/`: Core kernel functionality
- `kernel/cpu/`: CPU-specific code (interrupts, syscalls)
- `kernel/memory/`: Memory management
- `kernel/process/`: Process and scheduling
- `kernel/fs/`: File system layer
- `kernel/drivers/`: Device drivers
- `kernel/sync/`: Synchronization primitives

### 3. Security Features
- Comprehensive user pointer validation
- Buffer overflow protection in all syscalls
- Strict bounds checking
- Separation of user/kernel space

### 4. Performance Optimizations
- O(1) scheduler with bitmap-based priority tracking
- Page cache for file system operations
- Buffer cache for disk blocks
- Efficient memory allocators

## Inter-module Communication

Modules communicate through well-defined interfaces:
- VFS provides abstraction for file systems
- HAL (Hardware Abstraction Layer) for platform-specific code
- Standardized error codes across subsystems
- Event-driven architecture for drivers