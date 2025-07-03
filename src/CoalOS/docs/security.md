# Security in Coal OS

Coal OS implements multiple layers of security to protect against common vulnerabilities and ensure system integrity.

## Security Architecture

```
┌─────────────────────────────────────────────────────┐
│              User Applications                      │
├─────────────────────────────────────────────────────┤
│         Security Validation Layer                   │
│  - User pointer validation                          │
│  - Buffer overflow protection                       │
│  - Input sanitization                               │
├─────────────────────────────────────────────────────┤
│          Privilege Separation                       │
│  - User mode (Ring 3)                              │
│  - Kernel mode (Ring 0)                            │
├─────────────────────────────────────────────────────┤
│          Memory Protection                          │
│  - Virtual memory isolation                         │
│  - NX bit support                                  │
│  - Guard pages                                      │
├─────────────────────────────────────────────────────┤
│         Access Control                              │
│  - File permissions                                 │
│  - Process isolation                                │
│  - Resource limits                                  │
└─────────────────────────────────────────────────────┘
```

## Memory Protection

### 1. Virtual Memory Isolation

Each process runs in its own address space:

```c
// Process address space layout
0xFFFFFFFF ┌─────────────────┐
           │  Kernel Space   │ <- Not accessible from user mode
0xC0000000 ├─────────────────┤
           │   User Stack    │ <- Grows downward
           │        ↓        │
           │   Free Space    │
           │        ↑        │
           │    User Heap    │ <- Grows upward
           ├─────────────────┤
           │  .bss Section   │ <- Uninitialized data
           ├─────────────────┤
           │  .data Section  │ <- Initialized data
           ├─────────────────┤
           │  .text Section  │ <- Code (read-only)
0x00100000 └─────────────────┘
```

### 2. Page-Level Protection

```c
// Page permission flags
#define PAGE_PRESENT    0x001  // Page is present
#define PAGE_WRITABLE   0x002  // Page is writable
#define PAGE_USER       0x004  // Accessible from user mode
#define PAGE_NX         0x8000000000000000ULL  // No execute

// Example: Read-only code page
paging_map_page(vaddr, paddr, PAGE_PRESENT | PAGE_USER);

// Example: Writable data page
paging_map_page(vaddr, paddr, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX);
```

### 3. Stack Protection

#### Guard Pages
```c
// Allocate stack with guard page
void* allocate_user_stack(size_t size) {
    // Allocate stack + guard page
    void* stack_bottom = mmap(NULL, size + PAGE_SIZE, 
                             PROT_NONE, MAP_PRIVATE);
    
    // Make stack pages accessible (except guard)
    mprotect(stack_bottom + PAGE_SIZE, size, 
             PROT_READ | PROT_WRITE);
    
    return stack_bottom + PAGE_SIZE + size;  // Top of stack
}
```

#### Stack Canaries (Future)
```c
void function_with_canary() {
    uint32_t canary = get_stack_canary();
    
    // Function body
    char buffer[256];
    // ... use buffer ...
    
    // Check canary before return
    if (canary != get_stack_canary()) {
        kernel_panic("Stack corruption detected!");
    }
}
```

## Input Validation

### 1. User Pointer Validation

All user pointers are validated before access:

```c
// Comprehensive pointer validation
bool validate_user_pointer(const void* ptr, size_t size, bool write) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    
    // Check for NULL
    if (!ptr) return false;
    
    // Check for kernel space
    if (start >= KERNEL_BASE || end >= KERNEL_BASE) {
        return false;
    }
    
    // Check for overflow
    if (end < start) return false;
    
    // Check page permissions
    for (uintptr_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (!is_user_accessible(addr, write)) {
            return false;
        }
    }
    
    return true;
}
```

### 2. Safe Copy Functions

```c
// Safe copy from user space
int copy_from_user_safe(void* kernel_dst, const void* user_src, 
                       size_t size) {
    // Validate source
    if (!validate_user_pointer(user_src, size, false)) {
        return -EFAULT;
    }
    
    // Set up page fault handler
    set_safe_copy_handler();
    
    // Perform copy with fault protection
    int result = 0;
    __try {
        memcpy(kernel_dst, user_src, size);
    } __except {
        result = -EFAULT;
    }
    
    // Restore normal fault handler
    clear_safe_copy_handler();
    
    return result;
}
```

### 3. String Validation

```c
// Safe string length from user
ssize_t strnlen_user_safe(const char* user_str, size_t max_len) {
    if (!validate_user_pointer(user_str, 1, false)) {
        return -EFAULT;
    }
    
    size_t len = 0;
    set_safe_copy_handler();
    
    __try {
        while (len < max_len) {
            if (!validate_user_pointer(user_str + len, 1, false)) {
                break;
            }
            if (user_str[len] == '\0') {
                clear_safe_copy_handler();
                return len;
            }
            len++;
        }
    } __except {
        clear_safe_copy_handler();
        return -EFAULT;
    }
    
    clear_safe_copy_handler();
    return -ENAMETOOLONG;
}
```

## System Call Security

### 1. Argument Validation

Every system call validates its arguments:

```c
// Example: open() validation
int sys_open(const char* pathname, int flags, mode_t mode) {
    // Validate pathname
    char kernel_path[PATH_MAX];
    int result = copy_path_from_user(pathname, kernel_path, 
                                    sizeof(kernel_path));
    if (result < 0) return result;
    
    // Validate flags
    if (flags & ~VALID_OPEN_FLAGS) {
        return -EINVAL;
    }
    
    // Check access permissions
    if (!check_file_access(kernel_path, flags)) {
        return -EACCES;
    }
    
    // Proceed with operation
    return do_open(kernel_path, flags, mode);
}
```

### 2. Resource Limits

```c
// Per-process limits
typedef struct {
    size_t max_open_files;    // RLIMIT_NOFILE
    size_t max_memory;        // RLIMIT_AS
    size_t max_stack_size;    // RLIMIT_STACK
    size_t max_cpu_time;      // RLIMIT_CPU
} process_limits_t;

// Check before resource allocation
bool check_resource_limit(resource_t type, size_t requested) {
    process_t* proc = current_process();
    
    switch (type) {
    case RESOURCE_FD:
        return proc->open_files + 1 <= proc->limits.max_open_files;
    case RESOURCE_MEMORY:
        return proc->memory_usage + requested <= proc->limits.max_memory;
    // ... other resources
    }
}
```

## File System Security

### 1. Permission Checks

```c
// Unix-style permissions
bool check_file_permission(inode_t* inode, int access_mode) {
    uid_t uid = current_uid();
    gid_t gid = current_gid();
    mode_t mode = inode->i_mode;
    
    // Root has all permissions
    if (uid == 0) return true;
    
    // Owner permissions
    if (inode->i_uid == uid) {
        return (mode & (access_mode << 6)) != 0;
    }
    
    // Group permissions
    if (inode->i_gid == gid) {
        return (mode & (access_mode << 3)) != 0;
    }
    
    // Other permissions
    return (mode & access_mode) != 0;
}
```

### 2. Path Traversal Prevention

```c
// Validate and normalize path
int validate_path_security(const char* path, char* normalized) {
    // Reject paths with suspicious patterns
    if (strstr(path, "/../") || strstr(path, "/./")) {
        return -EINVAL;
    }
    
    // Normalize the path
    if (!normalize_path(path, normalized)) {
        return -EINVAL;
    }
    
    // Ensure path doesn't escape root
    if (!is_subpath_of(normalized, current_root())) {
        return -EACCES;
    }
    
    return 0;
}
```

## Process Security

### 1. Privilege Separation

```c
// Process privileges
typedef struct {
    uid_t uid;      // Real user ID
    uid_t euid;     // Effective user ID
    gid_t gid;      // Real group ID
    gid_t egid;     // Effective group ID
    
    // Capabilities (future)
    uint64_t capabilities;
} process_creds_t;

// Drop privileges
int drop_privileges(uid_t new_uid) {
    process_t* proc = current_process();
    
    // Only root can change UID
    if (proc->creds.euid != 0) {
        return -EPERM;
    }
    
    proc->creds.uid = new_uid;
    proc->creds.euid = new_uid;
    
    return 0;
}
```

### 2. Signal Security

```c
// Signal permission check
bool can_send_signal(process_t* sender, process_t* target, int sig) {
    // Can always signal yourself
    if (sender == target) return true;
    
    // Root can signal anyone
    if (sender->creds.euid == 0) return true;
    
    // Same UID can signal
    if (sender->creds.uid == target->creds.uid) return true;
    
    // SIGCONT has special rules for job control
    if (sig == SIGCONT && same_session(sender, target)) {
        return true;
    }
    
    return false;
}
```

## Exploit Mitigation

### 1. ASLR (Address Space Layout Randomization) [Future]

```c
// Randomize memory layout
void randomize_memory_layout(process_t* proc) {
    // Randomize stack location
    proc->stack_base = STACK_TOP - (rand() & STACK_RANDOM_MASK);
    
    // Randomize heap start
    proc->heap_start = HEAP_BASE + (rand() & HEAP_RANDOM_MASK);
    
    // Randomize mmap base
    proc->mmap_base = MMAP_BASE + (rand() & MMAP_RANDOM_MASK);
}
```

### 2. DEP/NX (Data Execution Prevention)

```c
// Mark data pages as non-executable
void enable_nx_protection(void) {
    // Enable NX bit in EFER MSR
    uint64_t efer = read_msr(MSR_EFER);
    efer |= EFER_NX;
    write_msr(MSR_EFER, efer);
    
    // Mark all data pages as NX
    update_page_tables_nx();
}
```

## Security Best Practices

### 1. Secure Coding Guidelines

- Always validate user input
- Use safe string functions
- Check return values
- Minimize privileged code
- Clear sensitive data after use

### 2. Resource Management

- Set appropriate limits
- Monitor resource usage
- Implement quotas
- Clean up on failure

### 3. Error Handling

- Don't leak information in errors
- Log security events
- Fail securely (deny by default)

## Security Auditing

### 1. System Call Auditing

```c
// Audit security-relevant system calls
void audit_syscall(int syscall_num, int result) {
    if (should_audit(syscall_num)) {
        audit_log("SYSCALL: pid=%d uid=%d syscall=%d result=%d",
                  current_pid(), current_uid(), 
                  syscall_num, result);
    }
}
```

### 2. File Access Auditing

```c
// Log file access attempts
void audit_file_access(const char* path, int access_type, int result) {
    if (is_sensitive_file(path)) {
        audit_log("FILE_ACCESS: pid=%d uid=%d path=%s type=%d result=%d",
                  current_pid(), current_uid(),
                  path, access_type, result);
    }
}
```

## Future Security Enhancements

1. **SELinux/AppArmor**: Mandatory Access Control
2. **seccomp**: System call filtering
3. **Capabilities**: Fine-grained privileges
4. **TPM Support**: Trusted boot
5. **Encrypted filesystems**: Data at rest protection
6. **Network security**: Firewall and packet filtering