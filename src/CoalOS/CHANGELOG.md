# Changelog

All notable changes to Coal OS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-01-30

### Added
- Initial release of Coal OS as a personal hobby project
- Basic kernel with memory management (paging, buddy allocator, slab allocator)
- Process management with ELF loading and context switching
- FAT16 file system support with VFS abstraction layer
- Device drivers for keyboard, timer, serial console, and ATA disk
- Basic userspace with shell and hello world program
- Multiboot2 support via Limine bootloader
- CMake-based build system

### Changed
- Refactored from school project structure to standalone hobby OS
- Renamed project from Group_14 to Coal OS
- Updated all branding and documentation

### Technical Details
- Architecture: x86 32-bit
- Memory Model: Higher-half kernel
- File System: FAT16
- Boot Protocol: Multiboot2
- Scheduler: Round-robin