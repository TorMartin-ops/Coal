# System Calls

Coal OS implements a Linux-compatible system call interface, providing a stable API for user applications while maintaining security through comprehensive validation.

## System Call Architecture

```
┌─────────────────────────────────────────────────────┐
│              User Application                       │
│                                                     │
│  int result = syscall(SYS_open, path, flags, mode);│
└──────────────────┬──────────────────────────────────┘
                   │ INT 0x80
┌──────────────────▼──────────────────────────────────┐
│           System Call Entry (ASM)                   │
│  - Save user context                                │
│  - Switch to kernel stack                           │
│  - Validate syscall number                          │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│         System Call Dispatcher                      │
│  - Security validation                              │
│  - Parameter extraction                             │
│  - Route to handler                                 │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│      System Call Implementation                     │
│  - Perform operation                                │
│  - Return result/error                              │
└─────────────────────────────────────────────────────┘
```

## System Call Interface

### Calling Convention (x86)
- **Syscall Number**: EAX
- **Arguments**: EBX, ECX, EDX, ESI, EDI, EBP
- **Return Value**: EAX
- **Error**: Negative return value

### Assembly Interface
```nasm
; Example: open system call
mov eax, 5          ; SYS_open
mov ebx, filename   ; const char *pathname
mov ecx, O_RDONLY   ; int flags
mov edx, 0          ; mode_t mode
int 0x80            ; Trigger system call
; Result in EAX
```

### C Library Wrapper
```c
int open(const char *pathname, int flags, mode_t mode) {
    int result;
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (SYS_open), "b" (pathname), 
          "c" (flags), "d" (mode)
        : "memory"
    );
    return result;
}
```

## Implemented System Calls

### Process Management

#### exit (1)
```c
void sys_exit(int status);
```
Terminates the calling process.

#### fork (2)
```c
pid_t sys_fork(void);
```
Creates a child process. Returns PID to parent, 0 to child.

#### read (3)
```c
ssize_t sys_read(int fd, void *buf, size_t count);
```
Reads from a file descriptor.

#### write (4)
```c
ssize_t sys_write(int fd, const void *buf, size_t count);
```
Writes to a file descriptor.

#### waitpid (7)
```c
pid_t sys_waitpid(pid_t pid, int *status, int options);
```
Waits for process state change.

#### execve (11)
```c
int sys_execve(const char *filename, char *const argv[], 
               char *const envp[]);
```
Executes a program.

#### getpid (20)
```c
pid_t sys_getpid(void);
```
Returns process ID.

### File System Calls

#### open (5)
```c
int sys_open(const char *pathname, int flags, mode_t mode);
```
Opens a file. Flags include:
- `O_RDONLY`: Read only
- `O_WRONLY`: Write only
- `O_RDWR`: Read/write
- `O_CREAT`: Create if doesn't exist
- `O_APPEND`: Append mode

#### close (6)
```c
int sys_close(int fd);
```
Closes a file descriptor.

#### lseek (19)
```c
off_t sys_lseek(int fd, off_t offset, int whence);
```
Repositions file offset. Whence values:
- `SEEK_SET`: From beginning
- `SEEK_CUR`: From current position
- `SEEK_END`: From end

#### stat (106)
```c
int sys_stat(const char *pathname, struct stat *statbuf);
```
Gets file status.

#### mkdir (39)
```c
int sys_mkdir(const char *pathname, mode_t mode);
```
Creates a directory.

#### rmdir (40)
```c
int sys_rmdir(const char *pathname);
```
Removes an empty directory.

#### unlink (10)
```c
int sys_unlink(const char *pathname);
```
Deletes a file.

#### chdir (12)
```c
int sys_chdir(const char *path);
```
Changes current directory.

#### getcwd (183)
```c
char *sys_getcwd(char *buf, size_t size);
```
Gets current working directory.

#### getdents (141)
```c
int sys_getdents(unsigned int fd, struct dirent *dirp, 
                 unsigned int count);
```
Reads directory entries.

### Memory Management

#### brk (45)
```c
int sys_brk(void *addr);
```
Changes program break (heap end).

#### mmap (90) [Future]
```c
void *sys_mmap(void *addr, size_t length, int prot, 
               int flags, int fd, off_t offset);
```
Maps files or devices into memory.

### Time Management

#### time (13)
```c
time_t sys_time(time_t *tloc);
```
Gets current time.

#### nanosleep (162)
```c
int sys_nanosleep(const struct timespec *req, 
                  struct timespec *rem);
```
High-resolution sleep.

## Security Features

### 1. User Pointer Validation

All system calls validate user pointers:

```c
// Validation functions
static inline bool syscall_validate_user_pointer(const_userptr_t ptr) {
    if (!ptr) return false;
    
    uintptr_t addr = (uintptr_t)ptr;
    
    // Must be in user space
    if (addr >= KERNEL_BASE) return false;
    
    // Must be mapped and accessible
    return is_user_accessible(addr);
}

// String validation with length limit
static inline bool syscall_validate_string_len(const_userptr_t str, 
                                               size_t max_len) {
    if (!syscall_validate_user_pointer(str)) return false;
    
    // Use safe string length check
    ssize_t len = strnlen_user_safe(str, max_len);
    return (len > 0 && len <= max_len);
}
```

### 2. Buffer Overflow Protection

Safe copy functions with bounds checking:

```c
// Copy from user with validation
static inline int syscall_copy_from_user(void *kernel_buf, 
                                         const_userptr_t user_buf,
                                         size_t size) {
    // Validate entire buffer range
    if (!syscall_validate_buffer(user_buf, size, false)) {
        return -EFAULT;
    }
    
    // Perform safe copy
    return copy_from_user_safe(kernel_buf, user_buf, size);
}
```

### 3. Path Validation

Comprehensive path security:

```c
static inline int syscall_validate_path(const_userptr_t user_path,
                                       char *kernel_buf,
                                       size_t buf_size) {
    // Basic validation
    if (!syscall_validate_string_len(user_path, PATH_MAX)) {
        return -ENAMETOOLONG;
    }
    
    // Copy to kernel
    int result = strncpy_from_user_safe(user_path, kernel_buf, 
                                        buf_size);
    if (result < 0) return result;
    
    // Additional checks
    if (strchr(kernel_buf, '\0') == NULL) {
        return -EINVAL;
    }
    
    // Check for directory traversal attempts
    if (strstr(kernel_buf, "../")) {
        // Additional validation needed
    }
    
    return 0;
}
```

## Error Codes

Coal OS uses standard Linux error codes:

| Error | Value | Description |
|-------|-------|-------------|
| EPERM | 1 | Operation not permitted |
| ENOENT | 2 | No such file or directory |
| EINTR | 4 | Interrupted system call |
| EIO | 5 | I/O error |
| ENXIO | 6 | No such device or address |
| E2BIG | 7 | Argument list too long |
| EBADF | 9 | Bad file descriptor |
| ENOMEM | 12 | Out of memory |
| EACCES | 13 | Permission denied |
| EFAULT | 14 | Bad address |
| EEXIST | 17 | File exists |
| ENOTDIR | 20 | Not a directory |
| EISDIR | 21 | Is a directory |
| EINVAL | 22 | Invalid argument |
| EMFILE | 24 | Too many open files |
| ENOSPC | 28 | No space left on device |
| EROFS | 30 | Read-only file system |
| ENAMETOOLONG | 36 | File name too long |

## System Call Implementation

### Dispatcher
```c
void syscall_handler(isr_frame_t *regs) {
    // Get syscall number and arguments
    uint32_t syscall_num = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;
    
    // Validate syscall number
    if (syscall_num >= NUM_SYSCALLS) {
        regs->eax = -ENOSYS;
        return;
    }
    
    // Enable interrupts for long operations
    asm volatile("sti");
    
    // Call handler
    syscall_func_t handler = syscall_table[syscall_num];
    if (handler) {
        regs->eax = handler(arg1, arg2, arg3, regs);
    } else {
        regs->eax = -ENOSYS;
    }
    
    // Check for pending signals
    deliver_pending_signals();
}
```

### Handler Example
```c
int32_t sys_open_impl(uint32_t pathname_ptr, uint32_t flags, 
                      uint32_t mode, isr_frame_t *regs) {
    // Validate and copy pathname
    char pathname[PATH_MAX];
    int result = syscall_copy_path_from_user(
        (const_userptr_t)pathname_ptr, pathname, sizeof(pathname)
    );
    if (result < 0) return result;
    
    // Validate flags
    if (flags & ~VALID_OPEN_FLAGS) {
        return -EINVAL;
    }
    
    // Perform operation
    file_t *file = vfs_open(pathname, flags);
    if (!file) {
        return -errno_to_linux(get_last_error());
    }
    
    // Allocate file descriptor
    int fd = process_allocate_fd(current_process(), file);
    if (fd < 0) {
        vfs_close(file);
        return -EMFILE;
    }
    
    return fd;
}
```

## Adding New System Calls

1. **Define the system call number** in `syscall.h`
2. **Implement the handler** in appropriate module
3. **Add security validation**
4. **Register in syscall table**
5. **Add libc wrapper**
6. **Document the interface**

## Performance Considerations

- Fast path for common syscalls
- Minimal copying between user/kernel
- Efficient parameter validation
- Batched operations where possible

## Future System Calls

- **Networking**: socket, bind, listen, accept
- **IPC**: pipe, msgget, shmget, semget
- **Signals**: sigaction, sigprocmask, kill
- **Threading**: clone, futex
- **Advanced I/O**: poll, select, epoll