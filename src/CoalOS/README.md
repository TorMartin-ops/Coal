# Coal OS

A hobby operating system written in C and x86 assembly.

## Overview

Coal OS is a 32-bit x86 operating system built from scratch as a personal project. It features a monolithic kernel design with support for:

- **Memory Management**: Physical and virtual memory management with paging
- **Process Management**: Multitasking with round-robin scheduling
- **File System**: FAT16 file system support with VFS layer
- **Device Drivers**: Keyboard, display, timer, and disk drivers
- **User Space**: Basic ELF loader and simple shell

## Building

### Prerequisites

- Cross-compiler toolchain (i686-elf-gcc)
- NASM assembler
- CMake (3.22.1 or later)
- QEMU for testing
- xorriso for ISO creation
- mtools for FAT disk image manipulation

### Build Instructions

```bash
# Create build directory
mkdir -p build/CoalOS
cd build/CoalOS

# Configure with CMake
cmake ../../src/CoalOS/

# Build the OS
make

# The build will produce:
# - kernel.bin: The kernel binary
# - kernel.iso: Bootable ISO image
# - disk.img: FAT16 disk image with user programs
```

## Running

```bash
# From the build directory
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

This will start QEMU with debugging enabled (GDB on port 1234).

## Architecture

Coal OS uses a higher-half kernel design with the kernel mapped to upper virtual addresses. Key components include:

- **Boot**: Multiboot2-compliant bootloader support via Limine
- **CPU**: GDT, IDT, TSS, and interrupt handling
- **Memory**: Buddy allocator, slab allocator, and kmalloc
- **Scheduler**: Simple round-robin scheduler with context switching
- **Filesystem**: VFS abstraction layer with FAT16 implementation
- **Drivers**: PIT timer, PS/2 keyboard, serial console, ATA disk

## Project Structure

```
CoalOS/
   boot/           # Boot code and multiboot header
   kernel/         # Kernel source code
      core/       # Core kernel functionality
      cpu/        # CPU-specific code (GDT, IDT, etc.)
      drivers/    # Device drivers
      fs/         # File system implementations
      lib/        # Kernel library functions
      memory/     # Memory management
      process/    # Process management
      sync/       # Synchronization primitives
   include/        # Header files
   userspace/      # User space programs
   scripts/        # Build and run scripts
```

## License

This is a personal hobby project. Feel free to explore and learn from the code.

## Contributing

This is a personal project, but suggestions and discussions are welcome. Feel free to open issues for bugs or feature discussions.