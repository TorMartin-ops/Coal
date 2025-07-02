# Coal OS Architecture

## Overview

Coal OS is a monolithic kernel operating system designed for the x86 architecture. It follows a traditional Unix-like design with a clear separation between kernel space and user space.

## Memory Architecture

### Memory Layout

```
0xFFFFFFFF ┌─────────────────┐
           │ Kernel Space    │
           │ (Higher Half)   │
0xC0000000 ├─────────────────┤ <- KERNEL_SPACE_VIRT_START
           │                 │
           │ User Space      │
           │                 │
0x00100000 ├─────────────────┤ <- User programs start here
           │ Low Memory      │
           │ (BIOS, etc.)    │
0x00000000 └─────────────────┘
```

### Memory Management Components

1. **Physical Memory Manager**
   - Frame allocator for 4KB pages
   - Tracks free/used physical frames
   - Multiboot memory map parsing

2. **Virtual Memory Manager**
   - x86 paging with 4KB pages
   - Page directory and page table management
   - Kernel mapped to higher half (0xC0000000+)

3. **Heap Allocators**
   - **Buddy Allocator**: Initial bootstrap allocator
   - **Slab Allocator**: Efficient for fixed-size objects
   - **Kmalloc**: General-purpose kernel allocator

## Process Architecture

### Process Control Block (PCB)

Each process maintains:
- Process ID (PID)
- Memory mappings (page directory)
- CPU state (registers, stack)
- File descriptors
- Scheduling information

### Context Switching

Context switches preserve:
- General purpose registers
- Segment registers
- Stack pointer
- Instruction pointer
- EFLAGS

## File System Architecture

### Virtual File System (VFS)

The VFS provides an abstraction layer between:
- System calls (open, read, write, etc.)
- Concrete file system implementations (FAT16)

### FAT16 Implementation

Features:
- Read/write support
- Long filename support (LFN)
- Directory operations
- File allocation and deallocation

## Device Driver Architecture

### Interrupt Handling

- **IDT**: Interrupt Descriptor Table setup
- **ISR**: Interrupt Service Routines
- **IRQ**: Hardware interrupt handling via 8259 PIC

### Supported Devices

1. **Timer (PIT)**
   - Programmable Interval Timer
   - Used for scheduling and timekeeping

2. **Keyboard**
   - PS/2 keyboard controller
   - Scancode to keycode translation
   - Support for multiple keyboard layouts

3. **Display**
   - VGA text mode terminal
   - Serial console for debugging

4. **Storage**
   - ATA/IDE disk driver
   - Block device abstraction
   - Buffer cache for performance

## Boot Process

1. **Stage 1: Bootloader (Limine)**
   - Loads kernel via Multiboot2 protocol
   - Sets up initial environment

2. **Stage 2: Early Kernel**
   - Multiboot info parsing
   - GDT/IDT setup
   - Initial memory mapping

3. **Stage 3: Kernel Initialization**
   - Memory management init
   - Device driver init
   - File system mount
   - Process scheduler start

4. **Stage 4: User Space**
   - Load initial programs
   - Start shell

## System Call Interface

System calls are implemented via software interrupts:
- Interrupt vector dedicated to syscalls
- Parameters passed in registers
- Kernel/user mode transition

## Synchronization

- **Spinlocks**: Basic mutual exclusion
- Interrupt disabling during critical sections
- No sleep/wait in interrupt context