# Coal OS Documentation

Welcome to the Coal OS documentation. This documentation covers the architecture, design decisions, and implementation details of Coal OS - a hobby operating system designed following SOLID principles.

## Table of Contents

1. [Architecture Overview](architecture/README.md)
2. [Getting Started](getting-started.md)
3. [Building Coal OS](building.md)
4. [Kernel Subsystems](kernel/README.md)
   - [Memory Management](kernel/memory.md)
   - [Process Management](kernel/process.md)
   - [File System](kernel/filesystem.md)
   - [Device Drivers](kernel/drivers.md)
   - [System Calls](kernel/syscalls.md)
5. [API Reference](api/README.md)
6. [Development Guide](development/README.md)
7. [Security](security.md)

## Quick Start

To build and run Coal OS:

```bash
# Build the OS
cd /workspaces/2025-ikt218-osdev/build/CoalOS
cmake ../../src/CoalOS/
make

# Run with QEMU
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

## Design Philosophy

Coal OS is designed with the following principles in mind:

- **SOLID Principles**: Single Responsibility, Open/Closed, Liskov Substitution, Interface Segregation, and Dependency Inversion
- **Modular Architecture**: Clear separation between subsystems
- **Security First**: Comprehensive input validation and buffer overflow protection
- **Linux Compatibility**: System call interface compatible with Linux for easier porting
- **Performance**: Optimized algorithms including O(1) scheduler and efficient caching

## Key Features

- **Memory Management**: Two-tier allocation system with buddy allocator and slab allocator
- **Process Scheduling**: O(1) priority-based scheduler with bitmap optimization
- **File System**: FAT16/32 support with VFS abstraction layer and page cache
- **System Calls**: Linux-compatible system call interface
- **Security**: Comprehensive user pointer validation and buffer overflow protection

## Contributing

See the [Development Guide](development/README.md) for information on contributing to Coal OS.

## License

Coal OS is a hobby project for educational purposes.