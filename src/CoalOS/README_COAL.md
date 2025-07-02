# Coal OS

A modern, Linux-compatible operating system built from scratch with a focus on efficiency, stability, and clean architecture.

## Overview

Coal OS is a 32-bit x86 operating system that implements:
- Linux system call compatibility for basic POSIX compliance
- Priority-based preemptive scheduling
- Virtual memory with paging
- FAT16 file system support
- Modular driver architecture
- Comprehensive memory management (buddy allocator, slab allocator)

## Features

### Linux Compatibility
- Dual-mode syscall dispatcher supporting both native and Linux system calls
- Linux x86 32-bit system call numbers and error codes
- Basic POSIX system calls: fork, exec, read, write, open, close, etc.
- User space pointer validation and error translation

### Memory Management
- Higher-half kernel design (kernel at 0xC0100000)
- Buddy allocator for physical memory
- Slab allocator for kernel objects
- Demand paging support (in development)
- Per-CPU memory allocation strategy

### Process Management
- Priority-based scheduler with multiple run queues
- Process groups and sessions
- Signal handling framework
- ELF binary loader

### File System
- Virtual File System (VFS) layer
- FAT16 implementation
- Buffer cache for disk I/O
- Long filename support

### Drivers
- PS/2 keyboard driver
- VGA text mode display
- Serial port communication
- PIT timer (1000 Hz)
- IDE/ATA disk driver

## Building

### Prerequisites
- GCC cross-compiler for i686-elf
- NASM assembler
- CMake 3.22.1 or higher
- QEMU for testing

### Build Instructions

```bash
# Create build directory
mkdir -p build/CoalOS
cd build/CoalOS

# Configure with CMake
cmake ../../src/CoalOS/

# Build the kernel
make

# Run in QEMU
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

## Architecture

Coal OS follows a monolithic kernel design with these key components:

```
CoalOS/
├── boot/          # Bootloader interface (Multiboot2)
├── kernel/        # Core kernel code
│   ├── core/      # Kernel initialization and main
│   ├── cpu/       # CPU-specific code (GDT, IDT, interrupts)
│   ├── memory/    # Memory management subsystems
│   ├── process/   # Process and scheduler implementation
│   ├── fs/        # File system implementations
│   ├── drivers/   # Device drivers
│   └── lib/       # Kernel library functions
├── include/       # Header files
├── userspace/     # User programs
└── tests/         # Test suite

```

## Current Status

### Implemented
- ✅ Boot via Limine bootloader
- ✅ Protected mode with paging
- ✅ Interrupt handling (IDT, ISRs)
- ✅ Memory management (physical & virtual)
- ✅ Basic process management
- ✅ System calls (partial Linux compatibility)
- ✅ Keyboard and display drivers
- ✅ FAT16 file system
- ✅ User mode execution

### In Progress
- 🚧 Complete Linux system call compatibility
- 🚧 Demand paging implementation
- 🚧 Signal handling
- 🚧 Inter-process communication
- 🚧 Network stack

### Planned
- 📋 SMP support
- 📋 ext2 file system
- 📋 POSIX compliance
- 📋 Graphics mode support
- 📋 Audio subsystem

## Testing

The kernel includes a comprehensive test suite:

```c
// Run all tests
test_run_all();

// Run specific category
test_run_category(TEST_CATEGORY_MEMORY);
```

Test categories include:
- Memory management
- Process management
- System calls
- File system operations
- Synchronization primitives

## Contributing

Contributions are welcome! Please follow these guidelines:

1. Fork the repository
2. Create a feature branch
3. Write clean, documented code
4. Add tests for new functionality
5. Submit a pull request

## Documentation

- [Architecture Overview](docs/ARCHITECTURE.md)
- [Development Guide](docs/DEVELOPMENT.md)
- [Linux Compatibility](docs/linux_compatibility_analysis.md)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

Tor Martin Kohle

## Acknowledgments

- Built as part of the IKT218 Operating Systems course
- Inspired by Linux and other open-source operating systems
- Uses Limine bootloader for modern boot protocol support