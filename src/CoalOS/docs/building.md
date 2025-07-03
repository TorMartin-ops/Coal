# Building Coal OS

This document provides detailed instructions for building Coal OS from source.

## Build System Overview

Coal OS uses CMake as its build system, which provides:
- Cross-platform build configuration
- Dependency management
- Multiple target support (kernel, user programs)

## Directory Structure

```
2025-ikt218-osdev/
├── src/CoalOS/           # Source code
│   ├── boot/            # Boot code
│   ├── kernel/          # Kernel source
│   ├── include/         # Header files
│   ├── userspace/       # User programs
│   └── CMakeLists.txt   # Main CMake configuration
├── build/CoalOS/        # Build output directory
│   ├── kernel.bin       # Kernel binary
│   ├── kernel.iso       # Bootable ISO
│   └── disk.img         # FAT16 disk image
└── scripts/             # Build and run scripts
```

## Build Configuration

### CMake Options

The build system supports several configuration options:

```bash
# Debug build (default)
cmake -DCMAKE_BUILD_TYPE=Debug ../../src/CoalOS/

# Release build (optimized)
cmake -DCMAKE_BUILD_TYPE=Release ../../src/CoalOS/

# Enable scheduler optimization
cmake -DUSE_SCHEDULER_OPTIMIZATION=ON ../../src/CoalOS/
```

### Compiler Flags

The build system uses specific flags for kernel compilation:
- `-m32`: 32-bit mode
- `-nostdlib`: No standard library
- `-fno-builtin`: No built-in functions
- `-fno-stack-protector`: No stack protection (kernel manages its own)
- `-march=i386`: Target i386 architecture

## Step-by-Step Build Process

### 1. Clean Build
```bash
cd /workspaces/2025-ikt218-osdev/build/CoalOS
rm -rf *
cmake ../../src/CoalOS/
make -j$(nproc)
```

### 2. Incremental Build
```bash
cd /workspaces/2025-ikt218-osdev/build/CoalOS
make -j$(nproc)
```

### 3. Build Specific Targets
```bash
# Build only the kernel
make coalos-kernel

# Build only user programs
make hello_elf shell_elf shell_modular_elf

# Create ISO image
make coalos-create-image
```

## Build Outputs

### Kernel Files
- `kernel.bin`: The kernel ELF binary
- `kernel.iso`: Bootable ISO with Limine bootloader

### User Programs
- `hello.elf`: Hello World program
- `shell.elf`: Original monolithic shell
- `shell_modular.elf`: Refactored modular shell

### Disk Image
- `disk.img`: FAT16 formatted disk image containing:
  - `/hello.elf`
  - `/bin/shell.elf`
  - `/bin/shell_modular.elf`

## Build System Details

### Linker Scripts
Coal OS uses custom linker scripts:
- `src/arch/i386/linker.ld`: Kernel linker script
- `src/arch/i386/user.ld`: User program linker script

### Assembly Files
NASM is used for assembly files with:
- Format: `elf32`
- Debug symbols: `-g`

### Module Organization

The kernel is built from multiple modules:
- **Core**: Kernel main, initialization, error handling
- **CPU**: GDT, IDT, interrupts, system calls
- **Memory**: Paging, frame allocation, kmalloc
- **Process**: Scheduler, process creation, ELF loader
- **File System**: VFS, FAT driver, page cache
- **Drivers**: Keyboard, timer, serial, display
- **Synchronization**: Spinlocks, mutexes

## Troubleshooting Build Issues

### Common Problems

1. **Missing Cross Compiler**
   ```
   Error: i686-elf-gcc: command not found
   ```
   Solution: Install or build the cross compiler

2. **NASM Version Issues**
   ```
   Error: unrecognized option `-f elf32`
   ```
   Solution: Update NASM to a recent version

3. **Linker Script Errors**
   ```
   Error: cannot find linker script
   ```
   Solution: Check paths in CMakeLists.txt

4. **Out of Memory**
   ```
   Error: cc1: out of memory allocating...
   ```
   Solution: Reduce parallel jobs: `make -j2`

### Debug Build Issues

To debug build problems:
```bash
# Verbose build
make VERBOSE=1

# Clean CMake cache
rm -rf CMakeCache.txt CMakeFiles/

# Check compiler
which i686-elf-gcc
i686-elf-gcc --version
```

## Advanced Build Options

### Custom Compiler
```bash
cmake -DCMAKE_C_COMPILER=/path/to/i686-elf-gcc \
      -DCMAKE_ASM_NASM_COMPILER=/path/to/nasm \
      ../../src/CoalOS/
```

### Build with Specific Features
```bash
# Enable all optimizations
cmake -DUSE_SCHEDULER_OPTIMIZATION=ON \
      -DCMAKE_BUILD_TYPE=Release \
      ../../src/CoalOS/
```

### Cross-Compilation for Different Hosts
The build system is designed for x86 Linux hosts. For other platforms:
- Use a Linux VM or container
- WSL2 on Windows is supported
- macOS requires additional setup

## Continuous Integration

For automated builds:
```bash
#!/bin/bash
# ci-build.sh
set -e
mkdir -p build
cd build
cmake ../src/CoalOS/
make -j$(nproc)
make test  # If tests are implemented
```

## Performance Profiling Builds

For performance analysis:
```bash
# Build with profiling symbols
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ../../src/CoalOS/
make

# Run with QEMU and performance monitoring
qemu-system-i386 -kernel kernel.bin -monitor stdio
```