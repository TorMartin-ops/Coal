# System Call API

This document describes the system call implementation interface for Coal OS kernel developers.

## Overview

System calls provide the interface between user space and kernel space. Coal OS implements Linux-compatible system calls for ease of porting applications.

## System Call Implementation

### System Call Handler Structure

```c
/**
 * @brief System call handler function type
 * @param arg1 First argument (EBX)
 * @param arg2 Second argument (ECX)
 * @param arg3 Third argument (EDX)
 * @param regs Full register context
 * @return System call result (negative for errors)
 */
typedef int32_t (*syscall_func_t)(uint32_t arg1, uint32_t arg2, 
                                  uint32_t arg3, isr_frame_t *regs);

/**
 * @brief Register a system call
 * @param num System call number
 * @param handler Handler function
 * @return 0 on success, negative error code on failure
 */
int syscall_register(uint32_t num, syscall_func_t handler);
```

### Register Context

```c
typedef struct isr_frame {
    // Pushed by interrupt handler
    uint32_t ds;
    
    // Pushed by pusha
    uint32_t edi, esi, ebp, esp;
    uint32_t ebx, edx, ecx, eax;
    
    // Interrupt info
    uint32_t int_no, err_code;
    
    // Pushed by CPU
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;  // Only if privilege change
} isr_frame_t;
```

## User Memory Access

### Safe Copy Functions

```c
/**
 * @brief Copy from user space with validation
 * @param kernel_dst Kernel destination buffer
 * @param user_src User source address
 * @param size Number of bytes to copy
 * @return 0 on success, -EFAULT on failure
 */
int copy_from_user_safe(void *kernel_dst, const_userptr_t user_src, 
                       size_t size);

/**
 * @brief Copy to user space with validation
 * @param user_dst User destination address
 * @param kernel_src Kernel source buffer
 * @param size Number of bytes to copy
 * @return 0 on success, -EFAULT on failure
 */
int copy_to_user_safe(const_userptr_t user_dst, const void *kernel_src,
                     size_t size);

/**
 * @brief Copy string from user space
 * @param kernel_dst Kernel destination buffer
 * @param user_src User source string
 * @param max_len Maximum length including null
 * @return String length or negative error
 */
ssize_t strncpy_from_user_safe(char *kernel_dst, const_userptr_t user_src,
                               size_t max_len);

/**
 * @brief Get string length in user space
 * @param user_str User string address
 * @param max_len Maximum length to check
 * @return String length or negative error
 */
ssize_t strnlen_user_safe(const_userptr_t user_str, size_t max_len);
```

### Validation Functions

```c
/**
 * @brief Validate user pointer
 * @param ptr User pointer
 * @return true if valid, false otherwise
 */
static inline bool syscall_validate_user_pointer(const_userptr_t ptr);

/**
 * @brief Validate user buffer
 * @param ptr Buffer start
 * @param size Buffer size
 * @param write true if checking write access
 * @return true if valid, false otherwise
 */
static inline bool syscall_validate_buffer(const_userptr_t ptr, 
                                          size_t size, bool write);

/**
 * @brief Validate user string
 * @param str String pointer
 * @param max_len Maximum allowed length
 * @return true if valid, false otherwise
 */
static inline bool syscall_validate_string_len(const_userptr_t str,
                                              size_t max_len);

/**
 * @brief Validate and copy path from user
 * @param user_path User path pointer
 * @param kernel_buf Kernel buffer
 * @param buf_size Buffer size
 * @return 0 on success, negative error code on failure
 */
static inline int syscall_copy_path_from_user(const_userptr_t user_path,
                                             char *kernel_buf,
                                             size_t buf_size);
```

## System Call Security

### Security Macros

```c
// Maximum path length for syscalls
#define SYSCALL_MAX_PATH_LEN PATH_MAX

// Check if address is in user space
#define IS_USER_ADDRESS(addr) ((uintptr_t)(addr) < KERNEL_BASE)

// Validate user pointer for read
#define SYSCALL_CHECK_USER_READ(ptr, size) \
    do { \
        if (!syscall_validate_buffer((ptr), (size), false)) { \
            return -EFAULT; \
        } \
    } while (0)

// Validate user pointer for write
#define SYSCALL_CHECK_USER_WRITE(ptr, size) \
    do { \
        if (!syscall_validate_buffer((ptr), (size), true)) { \
            return -EFAULT; \
        } \
    } while (0)
```

### Common Validation Patterns

```c
// Path validation pattern
int32_t sys_example_path(uint32_t user_path_ptr, ...) {
    char path[SYSCALL_MAX_PATH_LEN];
    
    int result = syscall_copy_path_from_user(
        (const_userptr_t)user_path_ptr, path, sizeof(path)
    );
    if (result < 0) {
        return result;
    }
    
    // Use validated path...
}

// Buffer validation pattern
int32_t sys_example_buffer(uint32_t user_buf_ptr, uint32_t size, ...) {
    // Validate size
    if (size > MAX_ALLOWED_SIZE) {
        return -EINVAL;
    }
    
    // Validate buffer access
    SYSCALL_CHECK_USER_WRITE(user_buf_ptr, size);
    
    // Allocate kernel buffer
    void *kernel_buf = kmalloc(size);
    if (!kernel_buf) {
        return -ENOMEM;
    }
    
    // Perform operation...
    
    // Copy result to user
    if (copy_to_user_safe((const_userptr_t)user_buf_ptr, 
                         kernel_buf, size) < 0) {
        kfree(kernel_buf);
        return -EFAULT;
    }
    
    kfree(kernel_buf);
    return 0;
}
```

## Adding New System Calls

### Step 1: Define System Call Number

```c
// In include/kernel/cpu/syscall.h
#define SYS_mynewcall 400  // Pick unused number
```

### Step 2: Implement Handler

```c
// In kernel/cpu/syscall_mynewcall.c
int32_t sys_mynewcall_impl(uint32_t arg1, uint32_t arg2, 
                          uint32_t arg3, isr_frame_t *regs) {
    // Validate arguments
    if (!IS_USER_ADDRESS(arg1)) {
        return -EFAULT;
    }
    
    // Perform operation
    // ...
    
    return 0;  // Success
}
```

### Step 3: Register Handler

```c
// In kernel/cpu/syscall.c syscall table
[SYS_mynewcall] = sys_mynewcall_impl,
```

### Step 4: Create Libc Wrapper

```c
// In userspace library
int mynewcall(void *arg1, int arg2) {
    return syscall(SYS_mynewcall, arg1, arg2);
}
```

## Error Code Translation

```c
/**
 * @brief Convert internal error to Linux error code
 * @param internal_error Internal error code
 * @return Linux-compatible error code
 */
static inline int errno_to_linux(int internal_error) {
    switch (internal_error) {
    case FS_ERR_NOT_FOUND:     return -ENOENT;
    case FS_ERR_NO_PERMISSION: return -EACCES;
    case FS_ERR_EXISTS:        return -EEXIST;
    case FS_ERR_NOT_DIR:       return -ENOTDIR;
    case FS_ERR_IS_DIR:        return -EISDIR;
    case FS_ERR_NO_SPACE:      return -ENOSPC;
    case FS_ERR_IO:           return -EIO;
    default:                   return -EINVAL;
    }
}
```

## System Call Examples

### File System Call

```c
int32_t sys_open_impl(uint32_t pathname_ptr, uint32_t flags, 
                     uint32_t mode, isr_frame_t *regs) {
    // Validate and copy path
    char pathname[SYSCALL_MAX_PATH_LEN];
    int result = syscall_copy_path_from_user(
        (const_userptr_t)pathname_ptr, pathname, sizeof(pathname)
    );
    if (result < 0) {
        return result;
    }
    
    // Validate flags
    if (flags & ~VALID_OPEN_FLAGS) {
        return -EINVAL;
    }
    
    // Call VFS
    int fd = vfs_open(pathname, flags, mode);
    if (fd < 0) {
        return errno_to_linux(fd);
    }
    
    return fd;
}
```

### Process System Call

```c
int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, 
                       uint32_t arg3, isr_frame_t *regs) {
    // Simple syscall - no validation needed
    process_t *current = current_process();
    return current->pid;
}
```

### Memory System Call

```c
int32_t sys_brk_impl(uint32_t new_brk, uint32_t arg2, 
                    uint32_t arg3, isr_frame_t *regs) {
    process_t *proc = current_process();
    
    // Get current break
    if (new_brk == 0) {
        return (int32_t)proc->heap_end;
    }
    
    // Validate new break
    if (new_brk < proc->heap_start || new_brk > proc->heap_max) {
        return -ENOMEM;
    }
    
    // Adjust heap
    proc->heap_end = new_brk;
    
    // Update page tables if needed
    // ...
    
    return (int32_t)new_brk;
}
```

## Debugging System Calls

### Debug Macros

```c
#ifdef DEBUG_SYSCALLS
#define SYSCALL_DEBUG(fmt, ...) \
    serial_printf("[SYSCALL] " fmt "\n", ##__VA_ARGS__)
#else
#define SYSCALL_DEBUG(fmt, ...)
#endif

// Usage in handler
SYSCALL_DEBUG("open: path=%s flags=%x", pathname, flags);
```

### Syscall Tracing

```c
/**
 * @brief Enable syscall tracing
 * @param pid Process to trace (0 for all)
 */
void syscall_trace_enable(pid_t pid);

/**
 * @brief Disable syscall tracing
 * @param pid Process to stop tracing
 */
void syscall_trace_disable(pid_t pid);

/**
 * @brief Print syscall statistics
 */
void syscall_print_stats(void);
```

## Performance Optimization

### Fast Path System Calls

```c
// Mark frequently used syscalls for optimization
#define SYSCALL_HOT __attribute__((hot))

SYSCALL_HOT
int32_t sys_read_impl(uint32_t fd, uint32_t buf_ptr, 
                     uint32_t count, isr_frame_t *regs) {
    // Fast path checks
    if (fd >= MAX_FDS) return -EBADF;
    if (count == 0) return 0;
    
    // ... rest of implementation
}
```

### Batched Operations

```c
// Support for vectored I/O
struct iovec {
    void *iov_base;   // Buffer address
    size_t iov_len;   // Buffer length
};

int32_t sys_readv_impl(uint32_t fd, uint32_t iov_ptr,
                      uint32_t iovcnt, isr_frame_t *regs) {
    // Validate vector count
    if (iovcnt > IOV_MAX) return -EINVAL;
    
    // Copy iovec array
    struct iovec iov[IOV_MAX];
    if (copy_from_user_safe(iov, (const_userptr_t)iov_ptr,
                           iovcnt * sizeof(struct iovec)) < 0) {
        return -EFAULT;
    }
    
    // Process all buffers
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        // Read into each buffer...
    }
    
    return total;
}
```

## Best Practices

1. **Always validate user pointers** before dereferencing
2. **Check bounds** on all size parameters
3. **Use safe copy functions** for user/kernel transfers
4. **Handle partial operations** correctly
5. **Return proper error codes** for compatibility
6. **Minimize time in syscall** handler
7. **Document syscall behavior** thoroughly