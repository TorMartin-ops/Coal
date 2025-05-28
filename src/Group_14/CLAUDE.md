# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build the OS (from build/Group_14 directory)
cmake ../../src/Group_14/
make

# Run with QEMU
cd /workspaces/2025-ikt218-osdev/build/Group_14
../../src/Group_14/scripts/start_qemu.sh kernel.iso

# The script will:
# - Launch QEMU with debugging enabled (GDB on port 1234)
# - Load kernel.iso and disk.img
# - Log serial output to qemu_output.log
```

## Architecture Overview

This is a 32-bit x86 operating system with a monolithic kernel design. Key architectural decisions:

### Memory Layout
- **Higher-half kernel**: Kernel mapped to upper virtual addresses
- **Physical memory**: Managed by frame allocator with buddy system
- **Virtual memory**: x86 paging with 4KB pages
- **Heap**: Two-tier system - slab allocator for fixed sizes, kmalloc for variable sizes

### Boot Sequence
1. Limine bootloader loads kernel via Multiboot2 protocol
2. `boot/multiboot2.asm` sets up initial stack and calls kernel
3. `kernel/core/kernel.c::main()` initializes subsystems in order:
   - CPU (GDT, IDT, TSS)
   - Memory management
   - Device drivers
   - File system
   - Process management

### Critical Subsystem Dependencies
- **Memory** must initialize before any dynamic allocation
- **GDT/IDT** must be set up before interrupts
- **Paging** must be enabled before higher-half access
- **PIT timer** required for scheduler
- **VFS** must mount root before file operations

### User-Kernel Interface
- System calls via interrupt (specific vector in IDT)
- User programs loaded as ELF executables
- Separate address spaces per process
- Context switching preserves full CPU state

## Key Development Patterns

### Adding New Drivers
1. Create header in `include/kernel/drivers/category/`
2. Implement in `kernel/drivers/category/`
3. Initialize in kernel main after prerequisites
4. Update CMakeLists.txt if adding new files

### File System Operations
- All FS operations go through VFS layer
- FAT16 is the concrete implementation
- Mount points managed by mount table
- Block cache sits between FS and disk driver

### Memory Allocation
- Use `kmalloc()` for general kernel allocations
- Use slab allocator for frequently allocated fixed-size objects
- Physical pages via `frame_alloc()`
- Per-CPU data via `percpu_alloc()`

### Synchronization
- Spinlocks for short critical sections
- Interrupts disabled during lock acquisition
- No sleeping while holding spinlocks