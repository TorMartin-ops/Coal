# Getting Started with Coal OS

This guide will help you get Coal OS up and running on your system.

## Prerequisites

Before you begin, ensure you have the following installed:

### Required Tools
- **CMake** (version 3.22.1 or higher)
- **GCC Cross Compiler** for i686-elf target
- **NASM** assembler
- **QEMU** for testing (qemu-system-i386)
- **xorriso** for ISO creation
- **mtools** for FAT filesystem manipulation
- **Limine** bootloader

### Development Environment
Coal OS is designed to be built in a Linux environment. WSL2 on Windows is also supported.

## Setting Up the Development Environment

### 1. Install Dependencies (Ubuntu/Debian)
```bash
# Update package list
sudo apt update

# Install build essentials
sudo apt install build-essential cmake nasm

# Install QEMU
sudo apt install qemu-system-x86

# Install ISO tools
sudo apt install xorriso mtools

# Install GDB for debugging (optional)
sudo apt install gdb
```

### 2. Install Cross Compiler
You need a cross compiler targeting i686-elf. You can either:
- Build it from source (recommended)
- Use a pre-built toolchain

#### Building from Source:
```bash
# This is a simplified version - see OSDev wiki for full instructions
export PREFIX="$HOME/opt/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"

# Download and build binutils
# Download and build GCC
```

### 3. Install Limine Bootloader
```bash
# Clone Limine
git clone https://github.com/limine-bootloader/limine.git --branch=v5.x-branch-binary
cd limine
make
sudo make install-strip DESTDIR=/usr/local
```

## Building Coal OS

### 1. Clone the Repository
```bash
git clone <repository-url>
cd 2025-ikt218-osdev
```

### 2. Create Build Directory
```bash
mkdir -p build/CoalOS
cd build/CoalOS
```

### 3. Configure with CMake
```bash
cmake ../../src/CoalOS/
```

### 4. Build the Kernel
```bash
make -j$(nproc)
```

This will create:
- `kernel.bin`: The kernel binary
- `kernel.iso`: Bootable ISO image
- `disk.img`: FAT16 disk image with user programs

## Running Coal OS

### Basic Run (without debugging)
```bash
../../src/CoalOS/scripts/start_qemu_no_debug.sh kernel.iso
```

### Run with GDB Debugging
```bash
# Terminal 1: Start QEMU with GDB server
../../src/CoalOS/scripts/start_qemu.sh kernel.iso

# Terminal 2: Connect GDB
gdb kernel.bin
(gdb) target remote :1234
(gdb) continue
```

### QEMU Monitor Commands
While QEMU is running, press `Ctrl+Alt+2` to access the monitor:
- `info registers` - Show CPU registers
- `info mem` - Show memory mappings
- `info tlb` - Show TLB entries
- `quit` - Exit QEMU

## First Steps in Coal OS

When Coal OS boots, you'll see:
1. Boot messages showing kernel initialization
2. The shell prompt

### Available Commands
The shell supports several built-in commands:
- `help` - Show available commands
- `ls` - List files
- `cat <file>` - Display file contents
- `echo <text>` - Print text
- `pwd` - Print working directory
- `cd <dir>` - Change directory
- `clear` - Clear screen
- `exit` - Exit shell

### Running Programs
Coal OS includes sample programs:
- `/hello.elf` - Simple "Hello, World!" program
- `/bin/shell.elf` - The shell itself

To run a program:
```
/hello.elf
```

## Troubleshooting

### Build Errors
1. **Missing headers**: Ensure all dependencies are installed
2. **Linker errors**: Check that your cross-compiler is in PATH
3. **CMake errors**: Verify CMake version (3.22.1+)

### Runtime Issues
1. **Boot failures**: Check that Limine is properly installed
2. **Triple fault**: Usually indicates memory management issues
3. **Page faults**: Check the serial output for details

### Debug Output
Coal OS outputs debug information to the serial port. When using QEMU, this is saved to `qemu_output.log`:
```bash
tail -f qemu_output.log
```

## Next Steps

- Read the [Architecture Overview](architecture/README.md)
- Learn about [Kernel Subsystems](kernel/README.md)
- Check the [Development Guide](development/README.md) to start contributing