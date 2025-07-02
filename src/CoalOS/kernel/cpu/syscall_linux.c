/**
 * @file syscall_linux.c
 * @brief Linux-compatible system call implementation
 * @author Architectural improvements for CoalOS
 */

#include <kernel/cpu/syscall_linux.h>
#include <kernel/cpu/syscall.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/memory/mm.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/uaccess.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/timer/time.h>
#include <kernel/lib/string.h>
#include <libc/limits.h>

// Define POSIX types for compatibility
typedef long time_t;
typedef int pid_t;

// POSIX limits
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef ARG_MAX
#define ARG_MAX 131072
#endif

// Linux-compatible syscall table
syscall_handler_t linux_syscall_table[__NR_syscalls];

// External function declarations
extern void schedule(void);
extern uint32_t frame_alloc(void);
extern void frame_free(uint32_t frame);
// TODO: These should be imported from the proper headers
void paging_map_page(uint32_t *pgdir, uint32_t vaddr, uint32_t paddr, uint32_t flags);
uint32_t *get_kernel_page_directory(void);
extern int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs);
extern int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs);
extern volatile uint32_t g_pit_ticks;

// Forward declarations for stub functions
static pcb_t *process_fork(pcb_t *parent);
static uint32_t find_free_vma_region(mm_struct_t *mm, size_t size);
static int vfs_mkdir(const char *path, uint32_t mode);
static int vfs_rmdir(const char *path);
static int vfs_unlink(const char *path);
static time_t get_unix_timestamp(void);
static uint32_t get_system_ticks(void);
static void scheduler_sleep(uint32_t ms);

// Helper functions for user space validation
static bool validate_user_buffer(const void *ptr, size_t size, bool write) {
    if (!ptr) return false;
    int type = write ? VERIFY_WRITE : VERIFY_READ;
    return access_ok(type, (const_userptr_t)ptr, size);
}

static bool validate_user_string(const char *str, size_t max_len) {
    if (!str) return false;
    if (!access_ok(VERIFY_READ, (const_userptr_t)str, 1)) return false;
    
    // Check string is null-terminated within max_len
    size_t len = 0;
    while (len < max_len) {
        char c;
        if (copy_from_user(&c, (const_userptr_t)(str + len), 1) != 0) {
            return false;
        }
        if (c == '\0') return true;
        len++;
    }
    return false; // String too long
}

// Forward declarations for syscall handlers
static int sys_linux_exit(uint32_t status, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_fork(uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5, uint32_t unused6);
static int sys_linux_read(uint32_t fd, uint32_t buf, uint32_t count, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_write(uint32_t fd, uint32_t buf, uint32_t count, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_open(uint32_t filename, uint32_t flags, uint32_t mode, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_close(uint32_t fd, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_waitpid(uint32_t pid, uint32_t stat_addr, uint32_t options, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_execve(uint32_t filename, uint32_t argv, uint32_t envp, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_getpid(uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5, uint32_t unused6);
static int sys_linux_brk(uint32_t brk, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_getuid(uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5, uint32_t unused6);
static int sys_linux_getgid(uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5, uint32_t unused6);
static int sys_linux_mmap(uint32_t addr, uint32_t length, uint32_t prot, uint32_t flags, uint32_t fd, uint32_t offset);
static int sys_linux_munmap(uint32_t addr, uint32_t length, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4);
static int sys_linux_mprotect(uint32_t addr, uint32_t len, uint32_t prot, uint32_t unused1, uint32_t unused2, uint32_t unused3);
static int sys_linux_mkdir(uint32_t pathname, uint32_t mode, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4);
static int sys_linux_rmdir(uint32_t pathname, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_unlink(uint32_t pathname, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_time(uint32_t tloc, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4, uint32_t unused5);
static int sys_linux_gettimeofday(uint32_t tv, uint32_t tz, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4);
static int sys_linux_nanosleep(uint32_t req, uint32_t rem, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t unused4);

// Unimplemented syscall handler
static int sys_unimplemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return -LINUX_ENOSYS;
}

// Initialize Linux-compatible syscall table
void init_linux_syscall_table(void) {
    // Initialize all entries to unimplemented
    for (int i = 0; i < __NR_syscalls; i++) {
        linux_syscall_table[i] = sys_unimplemented;
    }
    
    // Process Control
    linux_syscall_table[__NR_exit] = sys_linux_exit;
    linux_syscall_table[__NR_fork] = sys_linux_fork;
    linux_syscall_table[__NR_execve] = sys_linux_execve;
    linux_syscall_table[__NR_waitpid] = sys_linux_waitpid;
    linux_syscall_table[__NR_getpid] = sys_linux_getpid;
    
    // File I/O
    linux_syscall_table[__NR_read] = sys_linux_read;
    linux_syscall_table[__NR_write] = sys_linux_write;
    linux_syscall_table[__NR_open] = sys_linux_open;
    linux_syscall_table[__NR_close] = sys_linux_close;
    
    // Memory Management
    linux_syscall_table[__NR_brk] = sys_linux_brk;
    linux_syscall_table[__NR_mmap] = sys_linux_mmap;
    linux_syscall_table[__NR_munmap] = sys_linux_munmap;
    linux_syscall_table[__NR_mprotect] = sys_linux_mprotect;
    
    // File System
    linux_syscall_table[__NR_mkdir] = sys_linux_mkdir;
    linux_syscall_table[__NR_rmdir] = sys_linux_rmdir;
    linux_syscall_table[__NR_unlink] = sys_linux_unlink;
    
    // Time
    linux_syscall_table[__NR_time] = sys_linux_time;
    linux_syscall_table[__NR_gettimeofday] = sys_linux_gettimeofday;
    linux_syscall_table[__NR_nanosleep] = sys_linux_nanosleep;
    
    // User/Group IDs
    linux_syscall_table[__NR_getuid] = sys_linux_getuid;
    linux_syscall_table[__NR_getgid] = sys_linux_getgid;
    
    serial_printf("[SYSCALL] Linux-compatible syscall table initialized\n");
}

// Linux syscall dispatcher
int linux_syscall_dispatcher(uint32_t syscall_num, uint32_t ebx, uint32_t ecx, 
                            uint32_t edx, uint32_t esi, uint32_t edi) {
    if (syscall_num >= __NR_syscalls) {
        return -LINUX_ENOSYS;
    }
    
    syscall_handler_t handler = linux_syscall_table[syscall_num];
    if (!handler) {
        return -LINUX_ENOSYS;
    }
    
    // Call the handler with Linux ABI parameters
    return handler(ebx, ecx, edx, esi, edi, 0);
}

// System call implementations

static int sys_linux_exit(uint32_t status, uint32_t unused1, uint32_t unused2, 
                         uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // Store exit status
    current->process->exit_status = status;
    
    // Mark process as zombie
    current->process->state = PROC_ZOMBIE;
    current->process->has_exited = true;
    
    // Schedule another task
    schedule();
    
    // Should not return
    return 0;
}

static int sys_linux_fork(uint32_t unused1, uint32_t unused2, uint32_t unused3,
                         uint32_t unused4, uint32_t unused5, uint32_t unused6) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5; (void)unused6;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // Create child process
    pcb_t *child = process_fork(current->process);
    if (!child) {
        return -LINUX_ENOMEM;
    }
    
    // Return child PID to parent, 0 to child
    return child->pid;
}

static int sys_linux_read(uint32_t fd, uint32_t buf, uint32_t count,
                         uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    // Validate user buffer
    if (!validate_user_buffer((void *)buf, count, true)) {
        return -LINUX_EFAULT;
    }
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // Check file descriptor
    if (fd >= MAX_FD || !current->process->fd_table[fd]) {
        return -LINUX_EBADF;
    }
    
    // Perform read
    ssize_t result = sys_read(fd, (char *)buf, count);
    return result < 0 ? coalos_to_linux_error(result) : result;
}

static int sys_linux_write(uint32_t fd, uint32_t buf, uint32_t count,
                          uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    // Validate user buffer
    if (!validate_user_buffer((void *)buf, count, false)) {
        return -LINUX_EFAULT;
    }
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // Check file descriptor
    if (fd >= MAX_FD || !current->process->fd_table[fd]) {
        return -LINUX_EBADF;
    }
    
    // Perform write
    ssize_t result = sys_write(fd, (const char *)buf, count);
    return result < 0 ? coalos_to_linux_error(result) : result;
}

static int sys_linux_open(uint32_t filename, uint32_t flags, uint32_t mode,
                         uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    // Validate filename
    if (!validate_user_string((const char *)filename, PATH_MAX)) {
        return -LINUX_EFAULT;
    }
    
    // Convert Linux flags to CoalOS flags
    int coalos_flags = 0;
    if (flags & 0x0001) coalos_flags |= O_WRONLY;
    if (flags & 0x0002) coalos_flags |= O_RDWR;
    if (flags & 0x0100) coalos_flags |= O_CREAT;
    if (flags & 0x0200) coalos_flags |= O_EXCL;
    if (flags & 0x0400) coalos_flags |= O_NOCTTY;
    if (flags & 0x0800) coalos_flags |= O_TRUNC;
    if (flags & 0x1000) coalos_flags |= O_APPEND;
    
    // Perform open
    int result = sys_open((const char *)filename, coalos_flags, mode);
    return result < 0 ? coalos_to_linux_error(result) : result;
}

static int sys_linux_close(uint32_t fd, uint32_t unused1, uint32_t unused2,
                          uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // Check file descriptor
    if (fd >= MAX_FD || !current->process->fd_table[fd]) {
        return -LINUX_EBADF;
    }
    
    // Perform close
    int result = sys_close(fd);
    return result < 0 ? coalos_to_linux_error(result) : 0;
}

static int sys_linux_waitpid(uint32_t pid, uint32_t stat_addr, uint32_t options,
                            uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    // Validate status pointer if provided
    if (stat_addr && !validate_user_buffer((void *)stat_addr, sizeof(int), true)) {
        return -LINUX_EFAULT;
    }
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    // TODO: Use CoalOS sys_waitpid_impl directly
    // It handles the user space copy internally
    // int32_t result = sys_waitpid_impl(pid, stat_addr, options, NULL);
    // return result < 0 ? coalos_to_linux_error(result) : result;
    return -LINUX_ENOSYS;
}

static int sys_linux_execve(uint32_t filename, uint32_t argv, uint32_t envp,
                           uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    // Validate filename
    if (!validate_user_string((const char *)filename, PATH_MAX)) {
        return -LINUX_EFAULT;
    }
    
    // Count and validate argv
    int argc = 0;
    if (argv) {
        char **user_argv = (char **)argv;
        while (argc < 1024) { // Reasonable limit
            char *arg;
            if (copy_from_user(&arg, &user_argv[argc], sizeof(char *)) != 0) {
                return -LINUX_EFAULT;
            }
            if (!arg) break;
            if (!validate_user_string(arg, ARG_MAX)) {
                return -LINUX_EFAULT;
            }
            argc++;
        }
    }
    
    // TODO: Use CoalOS sys_execve_impl directly
    // int32_t result = sys_execve_impl(filename, argv, envp, NULL);
    // return result < 0 ? coalos_to_linux_error(result) : result;
    return -LINUX_ENOSYS;
}

static int sys_linux_getpid(uint32_t unused1, uint32_t unused2, uint32_t unused3,
                           uint32_t unused4, uint32_t unused5, uint32_t unused6) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5; (void)unused6;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process) {
        return -LINUX_ESRCH;
    }
    
    return current->process->pid;
}

static int sys_linux_brk(uint32_t brk, uint32_t unused1, uint32_t unused2,
                        uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process || !current->process->mm) {
        return -LINUX_ESRCH;
    }
    
    mm_struct_t *mm = current->process->mm;
    
    // If brk is 0, return current brk
    if (brk == 0) {
        return mm->end_brk;
    }
    
    // Validate new brk value
    if (brk < mm->start_brk || brk > mm->start_brk + 0x10000000) { // 256MB heap limit
        return mm->end_brk; // Return current brk on failure
    }
    
    // Update brk
    mm->end_brk = brk;
    
    // TODO: Actually allocate/deallocate pages as needed
    
    return brk;
}

static int sys_linux_getuid(uint32_t unused1, uint32_t unused2, uint32_t unused3,
                           uint32_t unused4, uint32_t unused5, uint32_t unused6) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5; (void)unused6;
    
    // For now, always return root (0)
    return 0;
}

static int sys_linux_getgid(uint32_t unused1, uint32_t unused2, uint32_t unused3,
                           uint32_t unused4, uint32_t unused5, uint32_t unused6) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5; (void)unused6;
    
    // For now, always return root (0)
    return 0;
}

static int sys_linux_mmap(uint32_t addr, uint32_t length, uint32_t prot,
                         uint32_t flags, uint32_t fd, uint32_t offset) {
    tcb_t *current = get_current_task();
    if (!current || !current->process || !current->process->mm) {
        return -LINUX_ESRCH;
    }
    
    // Round length up to page size
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    // Validate parameters
    if (length == 0 || length > 0x10000000) { // 256MB limit
        return -LINUX_EINVAL;
    }
    
    // Convert protection flags
    uint32_t vm_flags = 0;
    if (prot & 0x1) vm_flags |= VM_READ;
    if (prot & 0x2) vm_flags |= VM_WRITE;
    if (prot & 0x4) vm_flags |= VM_EXEC;
    
    // For now, only support anonymous mappings
    if (!(flags & 0x20)) { // MAP_ANONYMOUS
        return -LINUX_ENOSYS;
    }
    
    // Find free address space if addr is 0
    if (addr == 0) {
        addr = find_free_vma_region(current->process->mm, length);
        if (addr == 0) {
            return -LINUX_ENOMEM;
        }
    }
    
    // Create VMA
    vma_struct_t *vma = kmalloc(sizeof(vma_struct_t));
    if (!vma) {
        return -LINUX_ENOMEM;
    }
    
    vma->vm_start = addr;
    vma->vm_end = addr + length;
    vma->vm_flags = vm_flags;
    vma->vm_file = NULL;
    vma->vm_offset = 0;
    
    // Insert VMA using the proper function signature
    vma_struct_t *inserted = insert_vma(current->process->mm, addr, addr + length, 
                                       vm_flags, 0, NULL, 0);
    if (!inserted) {
        kfree(vma);
        return -LINUX_ENOMEM;
    }
    
    // Allocate and map pages if MAP_POPULATE is set
    if (flags & 0x8000) { // MAP_POPULATE
        for (uint32_t va = addr; va < addr + length; va += PAGE_SIZE) {
            uint32_t frame = frame_alloc();
            if (frame == 0) {
                // TODO: Clean up on failure
                return -LINUX_ENOMEM;
            }
            
            uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (vm_flags & VM_WRITE) page_flags |= PAGE_RW;
            
            // TODO: Implement page mapping
            // paging_map_page(get_kernel_page_directory(), va, frame << 12, page_flags);
        }
    }
    
    return addr;
}

static int sys_linux_munmap(uint32_t addr, uint32_t length, uint32_t unused1,
                           uint32_t unused2, uint32_t unused3, uint32_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process || !current->process->mm) {
        return -LINUX_ESRCH;
    }
    
    // Validate parameters
    if ((addr & (PAGE_SIZE - 1)) || length == 0) {
        return -LINUX_EINVAL;
    }
    
    // Round length up to page size
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    // Find and remove VMAs in range
    // TODO: Implement VMA removal and page unmapping
    
    return 0;
}

static int sys_linux_mprotect(uint32_t addr, uint32_t len, uint32_t prot,
                             uint32_t unused1, uint32_t unused2, uint32_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    tcb_t *current = get_current_task();
    if (!current || !current->process || !current->process->mm) {
        return -LINUX_ESRCH;
    }
    
    // Validate parameters
    if ((addr & (PAGE_SIZE - 1)) || len == 0) {
        return -LINUX_EINVAL;
    }
    
    // Round length up to page size
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    // Convert protection flags
    uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & 0x2) page_flags |= PAGE_RW; // PROT_WRITE
    
    // Update page protections
    for (uint32_t va = addr; va < addr + len; va += PAGE_SIZE) {
        // TODO: Check if page exists in VMA
        // TODO: Update page table entry
    }
    
    return 0;
}

static int sys_linux_mkdir(uint32_t pathname, uint32_t mode, uint32_t unused1,
                          uint32_t unused2, uint32_t unused3, uint32_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    
    // Validate pathname
    if (!validate_user_string((const char *)pathname, PATH_MAX)) {
        return -LINUX_EFAULT;
    }
    
    // Create directory
    int result = vfs_mkdir((const char *)pathname, mode);
    return result < 0 ? coalos_to_linux_error(result) : 0;
}

static int sys_linux_rmdir(uint32_t pathname, uint32_t unused1, uint32_t unused2,
                          uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    // Validate pathname
    if (!validate_user_string((const char *)pathname, PATH_MAX)) {
        return -LINUX_EFAULT;
    }
    
    // Remove directory
    int result = vfs_rmdir((const char *)pathname);
    return result < 0 ? coalos_to_linux_error(result) : 0;
}

static int sys_linux_unlink(uint32_t pathname, uint32_t unused1, uint32_t unused2,
                           uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    // Validate pathname
    if (!validate_user_string((const char *)pathname, PATH_MAX)) {
        return -LINUX_EFAULT;
    }
    
    // Remove file
    int result = vfs_unlink((const char *)pathname);
    return result < 0 ? coalos_to_linux_error(result) : 0;
}

static int sys_linux_time(uint32_t tloc, uint32_t unused1, uint32_t unused2,
                         uint32_t unused3, uint32_t unused4, uint32_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    
    // Get current time (seconds since epoch)
    time_t current_time = get_unix_timestamp();
    
    // Store to user space if requested
    if (tloc) {
        if (!validate_user_buffer((void *)tloc, sizeof(time_t), true)) {
            return -LINUX_EFAULT;
        }
        if (copy_to_user((void *)tloc, &current_time, sizeof(time_t)) != 0) {
            return -LINUX_EFAULT;
        }
    }
    
    return current_time;
}

static int sys_linux_gettimeofday(uint32_t tv, uint32_t tz, uint32_t unused1,
                                 uint32_t unused2, uint32_t unused3, uint32_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    
    if (!tv) {
        return -LINUX_EINVAL;
    }
    
    // Validate timeval structure
    if (!validate_user_buffer((void *)tv, 8, true)) { // sizeof(struct timeval)
        return -LINUX_EFAULT;
    }
    
    // Get current time
    struct {
        long tv_sec;
        long tv_usec;
    } timeval;
    
    timeval.tv_sec = get_unix_timestamp();
    timeval.tv_usec = (get_system_ticks() % 1000) * 1000; // Convert ms to us
    
    // Copy to user space
    if (copy_to_user((void *)tv, &timeval, sizeof(timeval)) != 0) {
        return -LINUX_EFAULT;
    }
    
    // Ignore timezone for now
    if (tz) {
        // Just zero it out
        struct { int tz_minuteswest; int tz_dsttime; } tzero = {0, 0};
        if (validate_user_buffer((void *)tz, sizeof(tzero), true)) {
            copy_to_user((void *)tz, &tzero, sizeof(tzero));
        }
    }
    
    return 0;
}

static int sys_linux_nanosleep(uint32_t req, uint32_t rem, uint32_t unused1,
                              uint32_t unused2, uint32_t unused3, uint32_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    
    if (!req) {
        return -LINUX_EINVAL;
    }
    
    // Validate timespec structure
    if (!validate_user_buffer((void *)req, 8, false)) { // sizeof(struct timespec)
        return -LINUX_EFAULT;
    }
    
    // Read requested sleep time
    struct {
        long tv_sec;
        long tv_nsec;
    } timespec;
    
    if (copy_from_user(&timespec, (void *)req, sizeof(timespec)) != 0) {
        return -LINUX_EFAULT;
    }
    
    // Validate nanoseconds
    if (timespec.tv_nsec < 0 || timespec.tv_nsec >= 1000000000) {
        return -LINUX_EINVAL;
    }
    
    // Convert to milliseconds
    uint32_t ms = timespec.tv_sec * 1000 + timespec.tv_nsec / 1000000;
    
    // Sleep
    scheduler_sleep(ms);
    
    // If rem is provided, zero it out (we don't support interruption)
    if (rem && validate_user_buffer((void *)rem, sizeof(timespec), true)) {
        timespec.tv_sec = 0;
        timespec.tv_nsec = 0;
        copy_to_user((void *)rem, &timespec, sizeof(timespec));
    }
    
    return 0;
}

// Additional error code for unimplemented syscalls
#define LINUX_ENOSYS 38  /* Function not implemented */

// Stub implementations for missing functions
// TODO: Move these to their proper locations

static pcb_t *process_fork(pcb_t *parent) {
    // TODO: Implement proper fork functionality
    (void)parent;
    return NULL;
}

static uint32_t find_free_vma_region(mm_struct_t *mm, size_t size) {
    // TODO: Implement VMA region finding
    (void)mm;
    (void)size;
    return 0x40000000; // Return a placeholder address
}

static int vfs_mkdir(const char *path, uint32_t mode) {
    // TODO: Implement directory creation
    (void)path;
    (void)mode;
    return -LINUX_ENOSYS;
}

static int vfs_rmdir(const char *path) {
    // TODO: Implement directory removal
    (void)path;
    return -LINUX_ENOSYS;
}

static int vfs_unlink(const char *path) {
    // TODO: Implement file deletion
    (void)path;
    return -LINUX_ENOSYS;
}

static time_t get_unix_timestamp(void) {
    // TODO: Implement proper time tracking
    return 0;
}

static uint32_t get_system_ticks(void) {
    // TODO: Get actual system ticks
    // extern volatile uint32_t g_pit_ticks;
    // return g_pit_ticks;
    return 0;
}

static void scheduler_sleep(uint32_t ms) {
    // TODO: Implement proper sleep
    (void)ms;
}