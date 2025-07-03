# Coal OS Development Guide

This guide provides information for developers who want to contribute to Coal OS or build upon it.

## Development Environment Setup

### Required Tools

1. **Cross Compiler Toolchain**
   ```bash
   # Install dependencies
   sudo apt-get install build-essential bison flex libgmp3-dev \
                        libmpc-dev libmpfr-dev texinfo

   # Build cross compiler (example for i686-elf)
   export PREFIX="$HOME/opt/cross"
   export TARGET=i686-elf
   export PATH="$PREFIX/bin:$PATH"
   
   # Build binutils
   wget https://ftp.gnu.org/gnu/binutils/binutils-2.37.tar.gz
   tar xf binutils-2.37.tar.gz
   cd binutils-2.37
   mkdir build && cd build
   ../configure --target=$TARGET --prefix="$PREFIX" --with-sysroot \
                --disable-nls --disable-werror
   make && make install
   
   # Build GCC
   wget https://ftp.gnu.org/gnu/gcc/gcc-11.2.0/gcc-11.2.0.tar.gz
   tar xf gcc-11.2.0.tar.gz
   cd gcc-11.2.0
   ./contrib/download_prerequisites
   mkdir build && cd build
   ../configure --target=$TARGET --prefix="$PREFIX" --disable-nls \
                --enable-languages=c,c++ --without-headers
   make all-gcc all-target-libgcc
   make install-gcc install-target-libgcc
   ```

2. **Development Tools**
   ```bash
   # Essential tools
   sudo apt-get install cmake nasm qemu-system-x86 \
                        xorriso mtools gdb

   # Optional tools
   sudo apt-get install valgrind cppcheck clang-format \
                        doxygen graphviz
   ```

3. **IDE Setup**
   - **VSCode**: Install C/C++ extension and CMake Tools
   - **CLion**: Native CMake support
   - **Vim/Emacs**: Configure with ctags/cscope

### Workspace Organization

```
workspace/
├── src/CoalOS/           # Source code
├── build/CoalOS/         # Build directory
├── tools/                # Development tools
├── tests/                # Test suites
└── docs/                 # Documentation
```

## Code Organization

### Directory Structure

```
src/CoalOS/
├── boot/                 # Bootloader interface
├── kernel/              # Kernel source
│   ├── core/           # Core kernel code
│   ├── cpu/            # CPU-specific code
│   ├── memory/         # Memory management
│   ├── process/        # Process management
│   ├── fs/             # File systems
│   ├── drivers/        # Device drivers
│   ├── sync/           # Synchronization
│   └── lib/            # Kernel library
├── include/             # Header files
│   ├── kernel/         # Kernel headers
│   ├── libc/           # C library headers
│   └── sys/            # System headers
├── userspace/           # User programs
└── scripts/             # Build scripts
```

### Module Guidelines

Each module should:
1. Have a clear, single responsibility
2. Provide a well-defined interface
3. Minimize dependencies on other modules
4. Include comprehensive error handling
5. Be thoroughly documented

Example module structure:
```
kernel/subsystem/
├── module.c            # Implementation
├── module_internal.h   # Internal definitions
├── module_api.c        # Public API implementation
└── README.md          # Module documentation
```

## Coding Standards

### C Coding Style

1. **Indentation**: 4 spaces (no tabs)
2. **Line Length**: 80 characters maximum
3. **Braces**: K&R style
   ```c
   if (condition) {
       // code
   } else {
       // code
   }
   ```

4. **Naming**:
   ```c
   // Functions
   void function_name(void);
   
   // Variables
   int local_variable;
   static int static_variable;
   
   // Constants
   #define CONSTANT_NAME 42
   
   // Types
   typedef struct {
       int field;
   } struct_name_t;
   ```

5. **Comments**:
   ```c
   /**
    * @brief Function description
    * @param param Parameter description
    * @return Return value description
    */
   int documented_function(int param);
   
   // Single line comment
   
   /*
    * Multi-line comment
    * for longer explanations
    */
   ```

### SOLID Principles

1. **Single Responsibility**: Each function/module does one thing
2. **Open/Closed**: Extensible without modification
3. **Liskov Substitution**: Interfaces are consistent
4. **Interface Segregation**: Small, focused interfaces
5. **Dependency Inversion**: Depend on abstractions

Example:
```c
// Interface (abstraction)
typedef struct vfs_driver {
    int (*read)(file_t* file, void* buf, size_t len);
    int (*write)(file_t* file, const void* buf, size_t len);
} vfs_driver_t;

// Implementation
static int fat_read(file_t* file, void* buf, size_t len) {
    // FAT-specific implementation
}

// Registration
vfs_driver_t fat_driver = {
    .read = fat_read,
    .write = fat_write,
};
```

## Development Workflow

### 1. Feature Development

```bash
# Create feature branch
git checkout -b feature/new-feature

# Make changes
vim kernel/module.c

# Build and test
cd build/CoalOS
make
./scripts/start_qemu.sh kernel.iso

# Commit changes
git add -p
git commit -m "module: Add new feature

Detailed description of what was added and why."
```

### 2. Testing

#### Unit Tests
```c
// tests/test_memory.c
void test_kmalloc_basic(void) {
    void* ptr = kmalloc(1024);
    assert(ptr != NULL);
    
    // Verify alignment
    assert((uintptr_t)ptr % 8 == 0);
    
    kfree(ptr);
}

void test_kmalloc_zero(void) {
    void* ptr = kmalloc(0);
    assert(ptr == NULL);
}
```

#### Integration Tests
```bash
# Run kernel with test suite
qemu-system-i386 -kernel kernel.bin -append "test=memory"
```

### 3. Debugging

#### GDB Debugging
```bash
# Terminal 1: Start QEMU with GDB server
qemu-system-i386 -kernel kernel.bin -s -S

# Terminal 2: Connect GDB
gdb kernel.bin
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

#### Debug Prints
```c
// Use debug macros
DEBUG_PRINT("Variable value: %d", var);

// Conditional debug output
#ifdef DEBUG_MEMORY
    kprintf("[MM] Allocated %u bytes at %p\n", size, ptr);
#endif
```

#### Serial Port Debugging
```c
// Direct serial output (always works)
serial_printf("Early boot: %s\n", message);

// Check serial log
tail -f qemu_output.log
```

## Contributing

### Contribution Process

1. **Fork the repository**
2. **Create a feature branch**
3. **Write code following standards**
4. **Add tests for new functionality**
5. **Update documentation**
6. **Submit pull request**

### Commit Messages

Format:
```
subsystem: Short description (50 chars max)

Longer explanation of what and why (72 chars per line).
Can be multiple paragraphs.

Fixes: #issue-number
Signed-off-by: Your Name <email@example.com>
```

Example:
```
memory: Add large page support for kernel heap

Implement 2MB page support for kernel heap allocations larger
than 2MB. This reduces TLB pressure and improves performance
for large allocations.

The implementation uses a bitmap to track large page availability
and falls back to 4KB pages when large pages are exhausted.

Fixes: #123
Signed-off-by: Jane Developer <jane@example.com>
```

### Code Review

Before submitting:
1. **Run formatter**: `clang-format -i kernel/*.c`
2. **Check warnings**: `make CFLAGS="-Wall -Wextra"`
3. **Run static analysis**: `cppcheck kernel/`
4. **Test thoroughly**: All existing tests pass
5. **Document changes**: Update relevant docs

## Performance Optimization

### Profiling

```c
// Simple profiling
uint64_t start = get_ticks();
function_to_profile();
uint64_t elapsed = get_ticks() - start;
kprintf("Function took %llu ticks\n", elapsed);

// CPU cycle counting
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}
```

### Optimization Guidelines

1. **Measure first**: Profile before optimizing
2. **Algorithm first**: Better algorithm > micro-optimization
3. **Cache-friendly**: Keep data compact and aligned
4. **Minimize locks**: Use lock-free when possible
5. **Batch operations**: Reduce system call overhead

## Security Considerations

### Input Validation
```c
// Always validate user input
if (!validate_user_pointer(user_ptr, size)) {
    return -EFAULT;
}

// Check bounds
if (index >= array_size) {
    return -EINVAL;
}

// Sanitize strings
if (strlen(user_string) >= MAX_LEN) {
    return -ENAMETOOLONG;
}
```

### Resource Limits
```c
// Check resource limits
if (current_process()->open_files >= MAX_OPEN_FILES) {
    return -EMFILE;
}

// Prevent integer overflow
if (size > SIZE_MAX - offset) {
    return -EOVERFLOW;
}
```

## Documentation

### Code Documentation
```c
/**
 * @file module.c
 * @brief Module implementation
 * @author Your Name
 * @date 2024-01-01
 */

/**
 * @brief Allocate a new widget
 * 
 * Allocates and initializes a new widget structure. The widget
 * must be freed with widget_free() when no longer needed.
 * 
 * @param size Size of the widget in bytes
 * @param flags Creation flags (WIDGET_*)
 * @return Pointer to widget or NULL on error
 * 
 * @see widget_free()
 * @note This function may sleep
 */
widget_t* widget_alloc(size_t size, uint32_t flags);
```

### API Documentation
- Use Doxygen format
- Document all public functions
- Include examples
- Explain error conditions

## Release Process

### Version Numbering
- Format: `MAJOR.MINOR.PATCH`
- MAJOR: Incompatible API changes
- MINOR: New functionality
- PATCH: Bug fixes

### Release Checklist
1. [ ] All tests pass
2. [ ] Documentation updated
3. [ ] CHANGELOG updated
4. [ ] Version bumped
5. [ ] Release notes written
6. [ ] Tagged in git
7. [ ] Binaries built
8. [ ] Announcement sent