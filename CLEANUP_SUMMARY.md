# CoalOS Codebase Cleanup Summary

## Date: January 2025

### Overview
This document summarizes the comprehensive cleanup performed on the CoalOS codebase to remove old files, logs, and update naming conventions.

## Cleanup Actions Performed

### 1. Log Files Removed (31 files)
All old QEMU and test log files were removed from `/build/CoalOS/`:
- `qemu_*.log` files (24 files)
- `test_*.log` files (6 files) 
- `final_test.log`

### 2. Build Artifacts Cleaned
- Removed stray object file: `signal_test.o`
- Removed misplaced build artifacts from scripts directory:
  - `/src/CoalOS/scripts/disk.img`
  - `/src/CoalOS/scripts/kernel.iso`

### 3. Old/Disabled Files Removed
- `/src/CoalOS/kernel/process/scheduler_core.c.disabled` - Old scheduler implementation

### 4. Project Naming Updates
Replaced all references from "UiAOS" to "CoalOS" in 7 files:
- `userspace/entry.asm`
- `kernel/lib/rbtree.c`
- `kernel/drivers/input/keyboard.c`
- `kernel/cpu/idt.c`
- `kernel/cpu/syscall.asm`
- `kernel/process/scheduler.c`
- `include/kernel/fs/vfs/fs_limits.h` (including macros UIAOS_LONG_MAX → COALOS_LONG_MAX)

### 5. Other Cleanup
- Removed `/qemu_panic_output.log` from workspace root

## Files Preserved

### Build System Files (Kept)
- CMake configuration and build files
- Current build outputs: `kernel.bin`, `kernel.iso`, `disk.img`, `hello.elf`, `shell.elf`
- CMake logs: `CMakeOutput.log`, `CMakeError.log`

### Historical Documentation (Kept)
- CHANGELOG.md - Contains historical reference to "Group_14" which documents project evolution

### Development Markers (Kept)
- TODO/FIXME comments in source files - These are active development markers

## Result
The codebase is now cleaner and more consistent:
- No old log files cluttering the build directory
- Consistent naming throughout (CoalOS instead of UiAOS)
- No disabled or temporary files
- Build artifacts are only in appropriate build directories

## Recommendations for Future Maintenance
1. Add `*.log` to `.gitignore` to prevent log files from being committed
2. Regularly clean build directory of old logs
3. Use consistent naming for any new files
4. Remove disabled files instead of keeping them with .disabled extension