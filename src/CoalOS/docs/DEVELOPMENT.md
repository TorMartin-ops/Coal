# Coal OS Development Guide

## Getting Started

### Prerequisites

For development, you'll need:

1. **Cross-Compiler Toolchain**
   - i686-elf-gcc (GCC cross-compiler for i686)
   - i686-elf-binutils

2. **Build Tools**
   - CMake 3.22.1 or later
   - GNU Make
   - NASM assembler

3. **Testing Tools**
   - QEMU (qemu-system-i386)
   - GDB (with i386 support)

4. **ISO Creation**
   - xorriso
   - mtools (for FAT disk image manipulation)

### Building from Source

```bash
# Clone the repository
git clone <repository-url>
cd coal-os

# Create build directory
mkdir -p build/CoalOS
cd build/CoalOS

# Configure with CMake
cmake ../../src/CoalOS/

# Build
make -j$(nproc)
```

### Running Coal OS

```bash
# From build directory
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

## Development Workflow

### Adding a New Driver

1. Create header file in `include/kernel/drivers/<category>/`
2. Implement driver in `kernel/drivers/<category>/`
3. Add initialization call in `kernel/core/kernel.c`
4. Update CMakeLists.txt to include new files

Example structure:
```
include/kernel/drivers/network/
    rtl8139.h
kernel/drivers/network/
    rtl8139.c
```

### Adding a System Call

1. Define syscall number in `include/kernel/cpu/syscall.h`
2. Implement handler in `kernel/cpu/syscall.c`
3. Add user-space wrapper in `include/libc/unistd.h`

### Memory Allocation Guidelines

- **kmalloc()**: General purpose allocation
- **frame_alloc()**: Physical page allocation
- **slab allocator**: For frequently allocated fixed-size objects
- **percpu_alloc()**: For per-CPU data structures

## Debugging

### Using GDB

```bash
# Terminal 1: Start QEMU with GDB server
cd build/CoalOS
../../src/CoalOS/scripts/start_qemu.sh kernel.iso

# Terminal 2: Connect GDB
gdb-multiarch kernel.bin
(gdb) target remote :1234
(gdb) break main
(gdb) continue
```

### Serial Debugging

Coal OS outputs debug information to serial port. View with:
```bash
tail -f build/CoalOS/qemu_output.log
```

### Common Debug Macros

```c
#include <kernel/core/debug.h>

// Debug print
KLOG_DEBUG("Value: %d", value);

// Assertions
KERNEL_ASSERT(condition, "Error message");

// Panic
KERNEL_PANIC_HALT("Fatal error: %s", reason);
```

## Code Style

### C Code
- 4 spaces indentation
- K&R style braces
- Descriptive variable names
- Comments for complex logic

### Assembly Code
- 4 spaces indentation
- AT&T syntax for inline assembly
- Intel syntax for NASM files
- Comment every non-trivial operation

### Header Files
- Include guards using `#ifndef COAL_MODULE_NAME_H`
- Minimal includes in headers
- Function documentation in headers

## Testing

### Unit Tests
Place in `tests/` directory (to be implemented).

### Integration Tests
Test full system functionality in QEMU.

### Performance Testing
Use PIT timer for benchmarking kernel operations.

## Contributing Guidelines

1. **Branching**
   - `main`: Stable releases
   - `develop`: Active development
   - `feature/*`: New features
   - `fix/*`: Bug fixes

2. **Commit Messages**
   - Use imperative mood
   - Keep under 72 characters
   - Reference issues if applicable

3. **Pull Requests**
   - Test thoroughly
   - Update documentation
   - Follow code style

## Resources

- [OSDev Wiki](https://wiki.osdev.org/)
- [Intel x86 Manuals](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html)
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/)