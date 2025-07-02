# Coal OS Development Repository

Coal OS is a hobby operating system project written in C and x86 assembly.

## Project Overview

This repository contains the Coal OS project, a 32-bit x86 operating system built from scratch. The project started as part of an OS development course but has been refactored into a personal hobby OS project.

## Building Coal OS

### Prerequisites

- Docker (recommended) or a local development environment with:
  - Cross-compiler toolchain (i686-elf-gcc)
  - NASM assembler
  - CMake (3.22.1 or later)
  - QEMU for testing
  - xorriso for ISO creation
  - mtools for FAT disk image manipulation

### Using Docker (Recommended)

The project includes a devcontainer configuration for easy development:

1. Install Docker and Visual Studio Code
2. Open the project in VS Code
3. Use the "Reopen in Container" option when prompted

### Build Instructions

```bash
# Create build directory
cd build/CoalOS

# Configure with CMake
cmake ../../src/CoalOS/

# Build the OS
make

# Run with QEMU
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

## Project Structure

```
.vscode/           # VS Code configuration
.devcontainer/     # Docker development container configuration
build/CoalOS/      # Build output directory
src/CoalOS/        # Source code for Coal OS
```

The main Coal OS source is located in `src/CoalOS/`. See the [Coal OS README](src/CoalOS/README.md) for detailed information about the OS architecture and components.

## Features

- Multiboot2-compliant boot process via Limine bootloader
- Memory management with paging, buddy allocator, and slab allocator
- Process management with ELF loading and context switching
- FAT16 file system with VFS abstraction layer
- Device drivers for keyboard, display, timer, and disk
- Basic userspace with shell and test programs

## License

This is a personal hobby project. Feel free to explore and learn from the code.