/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations
 * @version 5.5 - Confirmed copy_to_user args; using opaque uaccess pointers.
 * @author Tor Martin Kohle & Gemini
 */

// --- Includes ---
#include <kernel/cpu/syscall.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/memory/uaccess.h> // Now includes userptr_t, kernelptr_t definitions
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/fs_limits.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_process.h>
#include <kernel/memory/mm.h>
#include <kernel/process/signal.h>
#include <libc/limits.h>
#include <libc/stdbool.h>
#include <libc/stddef.h>
#include <kernel/cpu/syscall_linux.h>


// --- Constants ---
#define MAX_PUTS_LEN 256
#define MAX_ARGS 64
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef MAX_SYSCALL_STR_LEN
#define MAX_SYSCALL_STR_LEN 256
#endif
#ifndef MAX_RW_CHUNK_SIZE
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define MAX_RW_CHUNK_SIZE PAGE_SIZE
#endif
#ifndef MAX_INPUT_LENGTH
#define MAX_INPUT_LENGTH 256 // Should match terminal.h
#endif


// --- Utility Macros ---
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// --- Static Data ---
static syscall_fn_t syscall_table[MAX_SYSCALLS];
static bool linux_compat_mode = true; // Enable Linux compatibility by default

// --- Forward Declarations of Syscall Implementations ---
static int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_read_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
static int32_t sys_write_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
static int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags, uint32_t mode, isr_frame_t *regs);
static int32_t sys_close_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_lseek_impl(uint32_t fd, uint32_t offset, uint32_t whence, isr_frame_t *regs);
static int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_puts_impl(uint32_t user_str_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen);
static int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs);
static int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs);
static int32_t sys_pipe_impl(uint32_t user_pipefd_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_dup2_impl(uint32_t oldfd, uint32_t newfd, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_signal_impl(uint32_t signum, uint32_t user_handler_ptr, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_kill_impl(uint32_t pid, uint32_t sig, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_chdir_impl(uint32_t user_path_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_getcwd_impl(uint32_t user_buf_ptr, uint32_t size, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_stat_impl(uint32_t user_path_ptr, uint32_t user_stat_ptr, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_readdir_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
static int32_t sys_setsid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_getsid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_setpgid_impl(uint32_t pid, uint32_t pgid, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_getpgid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_getpgrp_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_tcsetpgrp_impl(uint32_t fd, uint32_t pgid, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_tcgetpgrp_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

// Forward declarations for pipe operations
static ssize_t pipe_read_operation(vnode_t *vnode, void *buffer, size_t count, off_t offset);
static ssize_t pipe_write_operation(vnode_t *vnode, const void *buffer, size_t count, off_t offset);
static int pipe_close_operation(vnode_t *vnode, bool is_write_end);

// Forward declarations for process management
pcb_t *process_get_by_pid(uint32_t pid);

// Stub implementation for process_get_by_pid - to be moved to process.c
pcb_t *process_get_by_pid(uint32_t pid) {
    // Simplified implementation - just return current process for now
    // In a real implementation, this would search through a process table
    if (pid == 0) return NULL;
    
    pcb_t *current = get_current_process();
    if (current && current->pid == pid) {
        return current;
    }
    
    // For now, always return NULL if not current process
    return NULL;
}

// Helper macros
#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type*)0)->member)
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif



//-----------------------------------------------------------------------------
// Syscall Initialization
//-----------------------------------------------------------------------------
void syscall_init(void) {
    serial_write("[Syscall] Initializing table...\n");
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented;
    }
    syscall_table[SYS_EXIT]   = sys_exit_impl;
    syscall_table[SYS_FORK]   = sys_fork_impl;
    syscall_table[SYS_READ]   = sys_read_impl;
    syscall_table[SYS_WRITE]  = sys_write_impl;
    syscall_table[SYS_OPEN]   = sys_open_impl;
    syscall_table[SYS_CLOSE]  = sys_close_impl;
    syscall_table[SYS_PUTS]   = sys_puts_impl;
    syscall_table[SYS_EXECVE] = sys_execve_impl;
    syscall_table[SYS_WAITPID] = sys_waitpid_impl;
    syscall_table[SYS_LSEEK]  = sys_lseek_impl;
    syscall_table[SYS_GETPID] = sys_getpid_impl;
    syscall_table[SYS_READ_TERMINAL_LINE] = sys_read_terminal_line_impl;
    syscall_table[SYS_PIPE]   = sys_pipe_impl;
    syscall_table[SYS_GETPPID] = sys_getppid_impl;
    syscall_table[SYS_DUP2]   = sys_dup2_impl;
    syscall_table[SYS_SIGNAL] = sys_signal_impl;
    syscall_table[SYS_KILL]   = sys_kill_impl;
    syscall_table[SYS_CHDIR]  = sys_chdir_impl;
    syscall_table[SYS_GETCWD] = sys_getcwd_impl;
    syscall_table[SYS_STAT]   = sys_stat_impl;
    syscall_table[SYS_READDIR] = sys_readdir_impl;
    
    // Process Groups and Sessions syscalls
    syscall_table[SYS_SETSID] = sys_setsid_impl;
    syscall_table[SYS_GETSID] = sys_getsid_impl;
    syscall_table[SYS_SETPGID] = sys_setpgid_impl;
    syscall_table[SYS_GETPGID] = sys_getpgid_impl;
    syscall_table[SYS_GETPGRP] = sys_getpgrp_impl;
    syscall_table[SYS_TCSETPGRP] = sys_tcsetpgrp_impl;
    syscall_table[SYS_TCGETPGRP] = sys_tcgetpgrp_impl;

    KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "SYS_EXIT assignment sanity check failed!");
    serial_write("[Syscall] Table initialized.\n");
    
    // Initialize Linux-compatible syscall table
    init_linux_syscall_table();
    serial_write("[Syscall] Linux compatibility mode enabled.\n");
}

//-----------------------------------------------------------------------------
// Static Helper: Safe String Copy from User Space
//-----------------------------------------------------------------------------
static int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen) {
    KERNEL_ASSERT(k_dst != NULL, "k_dst cannot be NULL in strncpy_from_user_safe");
    if (maxlen == 0) return -EINVAL;
    k_dst[0] = '\0';

    // Basic check, uaccess.c's access_ok will do more thorough VMA checks.
    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        return -EFAULT;
    }
    // access_ok is called by copy_from_user, no need to duplicate here if copy_from_user is robust.
    // However, checking for at least 1 byte readability upfront can be a quick filter.
    if (!access_ok(VERIFY_READ, u_src, 1)) { // Check at least one byte can be touched by access_ok
        return -EFAULT;
    }

    size_t len = 0;
    while (len < maxlen -1) { // Leave space for null terminator
        char current_char;
        // Cast u_src for pointer arithmetic if needed by copy_from_user's second param type if it's just `const void*`
        if (copy_from_user((kernelptr_t)&current_char, (const_userptr_t)((const char*)u_src + len), 1) != 0) {
            k_dst[len] = '\0'; // Null-terminate on partial copy due to fault
            return -EFAULT;
        }
        k_dst[len] = current_char;
        if (current_char == '\0') {
            return 0; // Success, null terminator copied
        }
        len++;
    }

    k_dst[len] = '\0'; // Maxlen-1 chars copied, ensure null termination
    
    // Check if the original string was actually longer (meaning truncation occurred)
    char next_char_check;
    if (copy_from_user((kernelptr_t)&next_char_check, (const_userptr_t)((const char*)u_src + len), 1) == 0 && next_char_check != '\0') {
        return -ENAMETOOLONG; // Original string was longer
    }
    // If copy_from_user failed above, it implies the accessible range ended exactly at u_src + len,
    // or there was another fault. If it succeeded and next_char_check is '\0', it fit perfectly.
    return 0; // String fit or was shorter and NUL terminated within maxlen
}


//-----------------------------------------------------------------------------
// Syscall Implementations
//-----------------------------------------------------------------------------
static int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3;
    serial_write("[Syscall] WARNING: Unimplemented syscall #");
    serial_print_hex(regs->eax);
    serial_write(" called by PID ");
    pcb_t* proc = get_current_process();
    serial_print_hex(proc ? proc->pid : 0xFFFFFFFF);
    serial_write("\n");
    return -ENOSYS;
}

static int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    remove_current_task_with_code(code);
    KERNEL_PANIC_HALT("sys_exit returned!");
    return 0;
}

static int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count_arg, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;

    userptr_t user_buf = (userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t bytes_read_for_user = 0; // Actual number of chars (excl NUL) for user
    char* k_line_buffer = NULL;

    if (count == 0) return -EINVAL; 
    if (!access_ok(VERIFY_WRITE, user_buf, count)) return -EFAULT;

    size_t kernel_buffer_size = MIN(count > 0 ? count : MAX_INPUT_LENGTH, MAX_INPUT_LENGTH);
    if (kernel_buffer_size == 0) kernel_buffer_size = 1;

    k_line_buffer = kmalloc(kernel_buffer_size);
    if (!k_line_buffer) return -ENOMEM;

    ssize_t bytes_from_terminal_device = terminal_read_line_blocking(k_line_buffer, kernel_buffer_size);

    if (bytes_from_terminal_device < 0) { 
        kfree(k_line_buffer);
        return bytes_from_terminal_device;
    }

    bytes_read_for_user = MIN((size_t)bytes_from_terminal_device, count - 1);

    if (copy_to_user(user_buf, (const_kernelptr_t)k_line_buffer, bytes_read_for_user) != 0) {
        kfree(k_line_buffer);
        return -EFAULT; 
    }

    // Create a null terminator in kernel space
    char null_terminator = '\0';
    if (copy_to_user((userptr_t)((char*)user_buf + bytes_read_for_user), (const_kernelptr_t)&null_terminator, 1) != 0) {
        kfree(k_line_buffer);
        return -EFAULT; 
    }

    // Corrected: Move kfree after the serial_write calls that use k_line_buffer
    serial_write("[Terminal] terminal_read_line_blocking: Copied bytes: '"); 
    serial_write(k_line_buffer);  
    serial_write("'\n");           
    kfree(k_line_buffer); 

    return (int32_t)bytes_read_for_user; 
}


static int32_t sys_read_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    userptr_t user_buf = (userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_read = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    if (!access_ok(VERIFY_WRITE, user_buf, count)) return -EFAULT;

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Read chunk size zero");

        ssize_t bytes_read_this_chunk;
        
        // Special case for STDIN_FILENO - read from terminal
        if (fd == STDIN_FILENO) {
            bytes_read_this_chunk = terminal_read_line_blocking(kbuf, current_chunk_size);
        }
        // Check if this is a pipe file descriptor
        else {
            pcb_t *current_process = get_current_process();
            if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
                sys_file_t *sf = current_process->fd_table[fd];
                if (sf && sf->vfs_file && sf->vfs_file->vnode && 
                    sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
                    // This is a pipe - call pipe_read_operation directly
                    bytes_read_this_chunk = pipe_read_operation(sf->vfs_file->vnode, kbuf, current_chunk_size, 0);
                } else {
                    // Regular file - use existing VFS path
                    bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
                }
            } else {
                // Invalid fd or process - use existing VFS path which will return appropriate error
                bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
            }
        }

        if (bytes_read_this_chunk < 0) {
            if (total_read > 0) break;
            total_read = bytes_read_this_chunk;
            break;
        }
        if (bytes_read_this_chunk == 0) break;

        if (copy_to_user((userptr_t)((char*)user_buf + total_read), (const_kernelptr_t)kbuf, (size_t)bytes_read_this_chunk) != 0) {
            if (total_read > 0) break;
            total_read = -EFAULT;
            break;
        }
        total_read += bytes_read_this_chunk;
        if ((size_t)bytes_read_this_chunk < current_chunk_size) break;
    }
    if (kbuf) kfree(kbuf);
    return total_read;
}

static int32_t sys_write_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    const_userptr_t user_buf = (const_userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_written = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    if (!access_ok(VERIFY_READ, user_buf, count)) return -EFAULT;

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Write chunk size zero");

        size_t not_copied_from_user = copy_from_user((kernelptr_t)kbuf, (const_userptr_t)((const char*)user_buf + total_written), current_chunk_size);
        size_t copied_this_chunk_from_user = current_chunk_size - not_copied_from_user;

        if (copied_this_chunk_from_user > 0) {
            ssize_t bytes_written_this_chunk;
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_this_chunk_from_user);
                bytes_written_this_chunk = copied_this_chunk_from_user;
            } else {
                // Check if this is a pipe file descriptor
                pcb_t *current_process = get_current_process();
                if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
                    sys_file_t *sf = current_process->fd_table[fd];
                    if (sf && sf->vfs_file && sf->vfs_file->vnode && 
                        sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
                        // This is a pipe - call pipe_write_operation directly
                        bytes_written_this_chunk = pipe_write_operation(sf->vfs_file->vnode, kbuf, copied_this_chunk_from_user, 0);
                    } else {
                        // Regular file - use existing VFS path
                        bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk_from_user);
                    }
                } else {
                    // Invalid fd or process - use existing VFS path which will return appropriate error
                    bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk_from_user);
                }
            }

            if (bytes_written_this_chunk < 0) {
                if (total_written > 0) break;
                total_written = bytes_written_this_chunk;
                break;
            }
            total_written += bytes_written_this_chunk;
            if ((size_t)bytes_written_this_chunk < copied_this_chunk_from_user) break;
        }

        if (not_copied_from_user > 0) {
             if (total_written > 0) break;
             total_written = -EFAULT;
             break;
        }
        if (copied_this_chunk_from_user == 0 && not_copied_from_user == 0 && current_chunk_size > 0) {
            if (total_written > 0) break;
            total_written = -EFAULT;
            break;
        }
    }
    if (kbuf) kfree(kbuf);
    return total_written;
}

static int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags_arg, uint32_t mode_arg, isr_frame_t *regs) {
    (void)regs;
    const_userptr_t user_pathname = (const_userptr_t)user_pathname_ptr;
    int flags = (int)flags_arg;
    int mode = (int)mode_arg;
    char k_pathname[MAX_SYSCALL_STR_LEN];

    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) return copy_err;

    return sys_open(k_pathname, flags, mode);
}

static int32_t sys_close_impl(uint32_t fd_arg, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    int fd = (int)fd_arg;
    
    // Check if this is a pipe file descriptor
    pcb_t *current_process = get_current_process();
    if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
        sys_file_t *sf = current_process->fd_table[fd];
        if (sf && sf->vfs_file && sf->vfs_file->vnode && 
            sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
            // This is a pipe - handle pipe closure
            vnode_t *vnode = sf->vfs_file->vnode;
            bool is_write_end = (sf->flags & O_WRONLY) != 0;
            
            // Clear the file descriptor from the table first
            uintptr_t irq_flags = spinlock_acquire_irqsave(&current_process->fd_table_lock);
            current_process->fd_table[fd] = NULL;
            spinlock_release_irqrestore(&current_process->fd_table_lock, irq_flags);
            
            // Call pipe close operation
            int result = pipe_close_operation(vnode, is_write_end);
            
            // Free the sys_file structure
            if (sf->vfs_file) {
                kfree(sf->vfs_file);
            }
            kfree(sf);
            
            return result;
        }
    }
    
    // Not a pipe - use regular file close
    return sys_close(fd);
}

static int32_t sys_lseek_impl(uint32_t fd_arg, uint32_t offset_arg, uint32_t whence_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    off_t offset = (off_t)(int32_t)offset_arg; // Cast to signed 32-bit then to off_t
    int whence = (int)whence_arg;
    return sys_lseek(fd, offset, whence);
}

static int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "get_current_process returned NULL in sys_getpid");
    return (int32_t)current_proc->pid;
}

// Forward declaration for buddy guard checking
extern void buddy_check_guards(const char *context);

static int32_t sys_puts_impl(uint32_t user_str_ptr_arg, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    const_userptr_t user_str_ptr = (const_userptr_t)user_str_ptr_arg;
    char kbuffer[MAX_PUTS_LEN];

    // Check buddy guards at start of SYS_PUTS
    buddy_check_guards("sys_puts_start");

    int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer));
    if (copy_err != 0) {
        buddy_check_guards("sys_puts_copy_error");
        return copy_err;
    }

    // Check guards after memory copy but before terminal write
    buddy_check_guards("sys_puts_before_terminal_write");

    terminal_write(kbuffer); // sys_puts usually writes to stdout.
    
    // Check guards after terminal write
    buddy_check_guards("sys_puts_after_terminal_write");
    
    // Standard puts adds a newline. If this sys_puts should too:
    // terminal_putchar('\n');
    return 0; // Return non-negative on success.
}

//-----------------------------------------------------------------------------
// POSIX System Call Implementations for Linux Compatibility
//-----------------------------------------------------------------------------

// Helper function to copy VMA tree for fork() - simplified implementation
static bool copy_vma_tree_simple(mm_struct_t *child_mm, mm_struct_t *parent_mm) {
    if (!child_mm || !parent_mm || !parent_mm->vma_tree.root) {
        return true; // Nothing to copy
    }
    
    // Use a simple approach: find all VMAs and copy them one by one
    // This is not the most efficient but avoids complex tree traversal
    
    // Start from 0 and scan through address space
    uintptr_t scan_addr = 0;
    int vmas_copied = 0;
    
    while (scan_addr < KERNEL_SPACE_VIRT_START) {
        vma_struct_t *parent_vma = find_vma(parent_mm, scan_addr);
        if (!parent_vma) {
            // No VMA at this address, skip to next potential VMA location
            scan_addr += PAGE_SIZE;
            continue;
        }
        
        // Copy this VMA to child
        vma_struct_t *child_vma = insert_vma(child_mm,
                                             parent_vma->vm_start,
                                             parent_vma->vm_end,
                                             parent_vma->vm_flags,
                                             parent_vma->page_prot,
                                             parent_vma->vm_file,
                                             parent_vma->vm_offset);
        
        if (!child_vma) {
            serial_printf("[Fork] Failed to copy VMA [0x%x-0x%x]\n", 
                          parent_vma->vm_start, parent_vma->vm_end);
            return false;
        }
        
        vmas_copied++;
        serial_printf("[Fork] Copied VMA [0x%x-0x%x] flags=0x%x\n",
                      parent_vma->vm_start, parent_vma->vm_end, parent_vma->vm_flags);
        
        // Move scan pointer to end of this VMA
        scan_addr = parent_vma->vm_end;
    }
    
    serial_printf("[Fork] Successfully copied %d VMAs\n", vmas_copied);
    return true;
}

// Helper function to copy memory management structures for fork()
// Full implementation with VMA copying and COW support
static mm_struct_t* copy_mm(mm_struct_t *parent_mm) {
    if (!parent_mm) {
        return NULL;
    }
    
    serial_printf("[Fork] Starting memory space duplication with COW support\n");
    
    // Clone the page directory for the child process
    uintptr_t child_pgd_phys = paging_clone_directory(parent_mm->pgd_phys);
    if (!child_pgd_phys) {
        serial_printf("[Fork] Failed to clone page directory\n");
        return NULL;
    }
    
    // Create new mm_struct for child
    mm_struct_t *child_mm = create_mm((uint32_t*)child_pgd_phys);
    if (!child_mm) {
        serial_printf("[Fork] Failed to create child mm_struct\n");
        // Clean up cloned page directory
        paging_free_user_space((uint32_t*)child_pgd_phys);
        return NULL;
    }
    
    // Copy memory region boundaries from parent
    child_mm->start_code = parent_mm->start_code;
    child_mm->end_code = parent_mm->end_code;
    child_mm->start_data = parent_mm->start_data;
    child_mm->end_data = parent_mm->end_data;
    child_mm->start_brk = parent_mm->start_brk;
    child_mm->end_brk = parent_mm->end_brk;
    child_mm->start_stack = parent_mm->start_stack;
    
    // Copy all VMAs from parent to child
    uintptr_t parent_flags = spinlock_acquire_irqsave(&parent_mm->lock);
    
    // Simplified VMA copying - iterate through tree and copy each VMA
    if (parent_mm->vma_tree.root) {
        if (!copy_vma_tree_simple(child_mm, parent_mm)) {
            spinlock_release_irqrestore(&parent_mm->lock, parent_flags);
            destroy_mm(child_mm);
            serial_printf("[Fork] Failed to copy VMAs\n");
            return NULL;
        }
    }
    
    spinlock_release_irqrestore(&parent_mm->lock, parent_flags);
    
    serial_printf("[Fork] Successfully duplicated memory space: %d VMAs\n", child_mm->map_count);
    return child_mm;
}

// Helper function to copy file descriptor table for fork()
static int copy_fd_table(pcb_t *parent, pcb_t *child) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&parent->fd_table_lock);
    
    for (int fd = 0; fd < MAX_FD; fd++) {
        sys_file_t *parent_sf = parent->fd_table[fd];
        if (parent_sf) {
            // Create new sys_file_t for child
            sys_file_t *child_sf = (sys_file_t*)kmalloc(sizeof(sys_file_t));
            if (!child_sf) {
                spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
                return -ENOMEM;
            }
            
            // Copy sys_file structure
            *child_sf = *parent_sf;
            
            // Child gets its own copy of the file structure
            file_t *child_file = (file_t*)kmalloc(sizeof(file_t));
            if (!child_file) {
                kfree(child_sf);
                spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
                return -ENOMEM;
            }
            
            // Copy file structure
            *child_file = *parent_sf->vfs_file;
            spinlock_init(&child_file->lock);
            child_sf->vfs_file = child_file;
            
            // Reference same vnode (files are shared between parent and child)
            // Note: In a full implementation, we'd need proper vnode reference counting
            
            child->fd_table[fd] = child_sf;
        }
    }
    
    spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
    return 0;
}

static int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3;
    
    pcb_t *parent = get_current_process();
    if (!parent) {
        return -ESRCH; // No parent process
    }
    
    // Create new process structure
    pcb_t *child = process_create("child_process");
    if (!child) {
        return -ENOMEM;
    }
    
    // Set up parent-child relationship
    child->ppid = parent->pid;
    process_add_child(parent, child);
    
    // Initialize signal handling for child and copy handlers from parent
    signal_init_process(child);
    signal_copy_handlers(parent, child);
    
    // Copy memory management structure
    if (parent->mm) {
        child->mm = copy_mm(parent->mm);
        if (!child->mm) {
            destroy_process(child);
            return -ENOMEM;
        }
        
        // Update child's page directory physical address
        child->page_directory_phys = child->mm->pgd_phys;
    }
    
    // Copy file descriptor table
    int fd_result = copy_fd_table(parent, child);
    if (fd_result < 0) {
        destroy_process(child);
        return fd_result;
    }
    
    // Copy parent's register state to child (including stack pointer)
    child->context = parent->context;
    
    // Child returns 0, parent returns child PID
    child->context.eax = 0; // Child's return value
    
    // Add child to scheduler
    int result = scheduler_add_task(child);
    if (result < 0) {
        destroy_process(child);
        return result;
    }
    
    serial_printf("[Fork] Created child process PID %u from parent PID %u\n", 
                  child->pid, parent->pid);
    
    // Parent returns child PID
    return child->pid;
}

// Helper function to parse argv array from user space
static int parse_argv(uint32_t user_argv_ptr, char ***argv_out, int *argc_out) {
    if (user_argv_ptr == 0) {
        *argv_out = NULL;
        *argc_out = 0;
        return 0;
    }
    
    userptr_t user_argv = (userptr_t)user_argv_ptr;
    if (!access_ok(VERIFY_READ, user_argv, sizeof(char*))) {
        return -EFAULT;
    }
    
    // Count argc and validate pointers
    int argc = 0;
    for (int i = 0; i < MAX_ARGS; i++) {
        char *user_arg_ptr;
        if (copy_from_user(&user_arg_ptr, (const_userptr_t)((char**)user_argv + i), sizeof(char*)) != 0) {
            return -EFAULT;
        }
        
        if (user_arg_ptr == NULL) {
            break; // End of argv array
        }
        argc++;
    }
    
    if (argc == 0) {
        *argv_out = NULL;
        *argc_out = 0;
        return 0;
    }
    
    // Allocate kernel argv array
    char **argv = (char**)kmalloc((argc + 1) * sizeof(char*));
    if (!argv) {
        return -ENOMEM;
    }
    
    // Copy each argument string
    for (int i = 0; i < argc; i++) {
        char *user_arg_ptr;
        copy_from_user(&user_arg_ptr, (const_userptr_t)((char**)user_argv + i), sizeof(char*));
        
        // Allocate buffer for this argument
        argv[i] = (char*)kmalloc(MAX_SYSCALL_STR_LEN);
        if (!argv[i]) {
            // Clean up on failure
            for (int j = 0; j < i; j++) {
                kfree(argv[j]);
            }
            kfree(argv);
            return -ENOMEM;
        }
        
        // Copy string from user space
        if (strncpy_from_user_safe((const_userptr_t)user_arg_ptr, argv[i], MAX_SYSCALL_STR_LEN) < 0) {
            // Clean up on failure
            for (int j = 0; j <= i; j++) {
                kfree(argv[j]);
            }
            kfree(argv);
            return -EFAULT;
        }
    }
    
    argv[argc] = NULL; // Null-terminate array
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

// Helper function to free parsed argv
static void free_argv(char **argv, int argc) {
    if (!argv) return;
    
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            kfree(argv[i]);
        }
    }
    kfree(argv);
}

static int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs) {
    (void)user_envp_ptr; // TODO: Implement environment variables
    
    const_userptr_t user_pathname = (const_userptr_t)user_pathname_ptr;
    char k_pathname[MAX_SYSCALL_STR_LEN];
    
    // Copy pathname from user space
    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) return copy_err;
    
    // Parse argv array
    char **argv;
    int argc;
    int argv_result = parse_argv(user_argv_ptr, &argv, &argc);
    if (argv_result < 0) {
        return argv_result;
    }
    
    // Get current process
    pcb_t *current = get_current_process();
    if (!current) {
        free_argv(argv, argc);
        return -ESRCH;
    }
    
    serial_printf("[Execve] Loading '%s' for PID %u with %d arguments\n", 
                  k_pathname, current->pid, argc);
    
    // Load new executable - simplified implementation
    // In a full implementation, this would parse ELF and set up memory properly
    pcb_t *new_proc = create_user_process(k_pathname);
    if (!new_proc) {
        free_argv(argv, argc);
        return -ENOENT; // File not found or load failed
    }
    
    // Save old memory management structure to clean up
    mm_struct_t *old_mm = current->mm;
    
    // Replace current process image with new one
    current->entry_point = new_proc->entry_point;
    current->user_stack_top = new_proc->user_stack_top;
    current->page_directory_phys = new_proc->page_directory_phys;
    current->mm = new_proc->mm;
    
    // Prevent cleanup of new MM when destroying temp process
    new_proc->mm = NULL;
    new_proc->page_directory_phys = NULL;
    
    // Clean up temporary new_proc structure
    destroy_process(new_proc);
    
    // Clean up old memory space
    if (old_mm) {
        destroy_mm(old_mm);
    }
    
    // Switch to new page directory
    paging_switch_directory((uint32_t)(uintptr_t)current->page_directory_phys);
    
    // Set up initial register state for new program
    // execve() replaces the process image, so we need to set up entry point
    regs->eip = current->entry_point;
    regs->useresp = (uintptr_t)current->user_stack_top;
    regs->ebp = 0;
    regs->eax = argc;  // Pass argc in EAX
    regs->ebx = 0;     // argv pointer (simplified)
    regs->ecx = 0;     // envp pointer (not implemented)
    regs->edx = 0;
    regs->esi = 0;
    regs->edi = 0;
    regs->eflags = 0x202; // IF=1, reserved bit=1
    regs->cs = 0x1B;   // User code segment
    regs->ds = 0x23;   // User data segment
    regs->es = 0x23;
    regs->fs = 0x23;
    regs->gs = 0x23;
    regs->ss = 0x23;   // User stack segment
    
    // TODO: Set up argv/envp on user stack properly
    // For now, we just pass argc and assume program doesn't need detailed argv
    
    free_argv(argv, argc);
    
    serial_printf("[Execve] Successfully loaded '%s', entry=0x%x, stack=0x%x\n", 
                  k_pathname, current->entry_point, (uintptr_t)current->user_stack_top);
    
    // execve() doesn't return on success - the process image is replaced
    // The return happens when the new program starts executing at the entry point
    return 0;
}

static int32_t sys_waitpid_impl(uint32_t pid_arg, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs) {
    (void)options; (void)regs;
    
    pid_t target_pid = (pid_t)pid_arg;
    userptr_t user_status = (userptr_t)user_status_ptr;
    
    pcb_t *parent = get_current_process();
    if (!parent) {
        return -ESRCH;
    }
    
    // Basic validation
    if (user_status && !access_ok(VERIFY_WRITE, user_status, sizeof(int))) {
        return -EFAULT;
    }
    
    // TODO: Implement proper child process tracking and waiting
    // For now, return immediately with no child found
    serial_printf("[Wait] Process PID %u waiting for child PID %d\n", 
                  parent->pid, target_pid);
    
    // Write exit status if requested
    if (user_status) {
        int exit_status = 0; // Default exit status
        if (copy_to_user(user_status, (const_kernelptr_t)&exit_status, sizeof(int)) != 0) {
            return -EFAULT;
        }
    }
    
    return -ECHILD; // No child processes (until proper process hierarchy is implemented)
}

// Pipe data structure for simple pipe implementation
typedef struct pipe_data {
    char buffer[4096];        // 4KB pipe buffer
    size_t read_pos;          // Current read position
    size_t write_pos;         // Current write position
    size_t count;             // Number of bytes in buffer
    spinlock_t lock;          // Synchronization lock
    bool closed_write;        // Write end closed
    bool closed_read;         // Read end closed
    uint32_t ref_count;       // Reference count for cleanup
} pipe_data_t;

// Simple pipe operations
static ssize_t pipe_read_operation(vnode_t *vnode, void *buffer, size_t count, off_t offset) {
    (void)offset;  // Pipes don't use offsets
    
    pipe_data_t *pipe = (pipe_data_t*)vnode->data;
    if (!pipe || !buffer) return -EINVAL;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&pipe->lock);
    
    // If no data and write end is closed, return EOF
    if (pipe->count == 0 && pipe->closed_write) {
        spinlock_release_irqrestore(&pipe->lock, irq_flags);
        return 0;
    }
    
    // If no data available, return would block (simplified)
    if (pipe->count == 0) {
        spinlock_release_irqrestore(&pipe->lock, irq_flags);
        return -EAGAIN;
    }
    
    // Read available data
    size_t bytes_to_read = MIN(count, pipe->count);
    size_t bytes_read = 0;
    
    while (bytes_read < bytes_to_read) {
        ((char*)buffer)[bytes_read] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % sizeof(pipe->buffer);
        bytes_read++;
        pipe->count--;
    }
    
    spinlock_release_irqrestore(&pipe->lock, irq_flags);
    return bytes_read;
}

static ssize_t pipe_write_operation(vnode_t *vnode, const void *buffer, size_t count, off_t offset) {
    (void)offset;  // Pipes don't use offsets
    
    pipe_data_t *pipe = (pipe_data_t*)vnode->data;
    if (!pipe || !buffer) return -EINVAL;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&pipe->lock);
    
    // If read end is closed, return SIGPIPE (simplified as error)
    if (pipe->closed_read) {
        spinlock_release_irqrestore(&pipe->lock, irq_flags);
        return -EPIPE;
    }
    
    // Calculate available space
    size_t available_space = sizeof(pipe->buffer) - pipe->count;
    if (available_space == 0) {
        spinlock_release_irqrestore(&pipe->lock, irq_flags);
        return -EAGAIN;  // Would block
    }
    
    // Write available space
    size_t bytes_to_write = MIN(count, available_space);
    size_t bytes_written = 0;
    
    while (bytes_written < bytes_to_write) {
        pipe->buffer[pipe->write_pos] = ((const char*)buffer)[bytes_written];
        pipe->write_pos = (pipe->write_pos + 1) % sizeof(pipe->buffer);
        bytes_written++;
        pipe->count++;
    }
    
    spinlock_release_irqrestore(&pipe->lock, irq_flags);
    return bytes_written;
}

static int pipe_close_operation(vnode_t *vnode, bool is_write_end) {
    pipe_data_t *pipe = (pipe_data_t*)vnode->data;
    if (!pipe) return -EINVAL;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&pipe->lock);
    
    if (is_write_end) {
        pipe->closed_write = true;
    } else {
        pipe->closed_read = true;
    }
    
    pipe->ref_count--;
    bool should_free = (pipe->ref_count == 0);
    
    spinlock_release_irqrestore(&pipe->lock, irq_flags);
    
    // Free pipe data when both ends are closed
    if (should_free) {
        kfree(pipe);
        kfree(vnode);
    }
    
    return 0;
}

static int32_t sys_pipe_impl(uint32_t user_pipefd_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    
    userptr_t user_pipefd = (userptr_t)user_pipefd_ptr;
    
    // Validate user buffer
    if (!access_ok(VERIFY_WRITE, user_pipefd, 2 * sizeof(int))) {
        return -EFAULT;
    }
    
    pcb_t *current_process = get_current_process();
    if (!current_process) {
        return -ESRCH;
    }
    
    // Allocate pipe data structure
    pipe_data_t *pipe = (pipe_data_t*)kmalloc(sizeof(pipe_data_t));
    if (!pipe) {
        return -ENOMEM;
    }
    
    // Initialize pipe
    memset(pipe->buffer, 0, sizeof(pipe->buffer));
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    spinlock_init(&pipe->lock);
    pipe->closed_write = false;
    pipe->closed_read = false;
    pipe->ref_count = 2;  // Read and write ends
    
    // Create vnodes for read and write ends
    vnode_t *read_vnode = (vnode_t*)kmalloc(sizeof(vnode_t));
    vnode_t *write_vnode = (vnode_t*)kmalloc(sizeof(vnode_t));
    
    if (!read_vnode || !write_vnode) {
        if (read_vnode) kfree(read_vnode);
        if (write_vnode) kfree(write_vnode);
        kfree(pipe);
        return -ENOMEM;
    }
    
    read_vnode->data = pipe;
    read_vnode->fs_driver = NULL;  // Simplified - no VFS driver for now
    write_vnode->data = pipe;
    write_vnode->fs_driver = NULL;
    
    // Create file structures
    file_t *read_file = (file_t*)kmalloc(sizeof(file_t));
    file_t *write_file = (file_t*)kmalloc(sizeof(file_t));
    
    if (!read_file || !write_file) {
        if (read_file) kfree(read_file);
        if (write_file) kfree(write_file);
        kfree(read_vnode);
        kfree(write_vnode);
        kfree(pipe);
        return -ENOMEM;
    }
    
    read_file->vnode = read_vnode;
    read_file->flags = O_RDONLY;
    read_file->offset = 0;
    spinlock_init(&read_file->lock);
    
    write_file->vnode = write_vnode;
    write_file->flags = O_WRONLY;
    write_file->offset = 0;
    spinlock_init(&write_file->lock);
    
    // Create sys_file wrappers
    sys_file_t *read_sys_file = (sys_file_t*)kmalloc(sizeof(sys_file_t));
    sys_file_t *write_sys_file = (sys_file_t*)kmalloc(sizeof(sys_file_t));
    
    if (!read_sys_file || !write_sys_file) {
        if (read_sys_file) kfree(read_sys_file);
        if (write_sys_file) kfree(write_sys_file);
        kfree(read_file);
        kfree(write_file);
        kfree(read_vnode);
        kfree(write_vnode);
        kfree(pipe);
        return -ENOMEM;
    }
    
    read_sys_file->vfs_file = read_file;
    read_sys_file->flags = O_RDONLY;
    write_sys_file->vfs_file = write_file;
    write_sys_file->flags = O_WRONLY;
    
    // Assign file descriptors
    uintptr_t fd_irq_flags = spinlock_acquire_irqsave(&current_process->fd_table_lock);
    
    // Find free file descriptors
    int read_fd = -1, write_fd = -1;
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (current_process->fd_table[fd] == NULL) {
            if (read_fd == -1) {
                read_fd = fd;
                current_process->fd_table[fd] = read_sys_file;
            } else if (write_fd == -1) {
                write_fd = fd;
                current_process->fd_table[fd] = write_sys_file;
                break;
            }
        }
    }
    
    spinlock_release_irqrestore(&current_process->fd_table_lock, fd_irq_flags);
    
    if (read_fd < 0 || write_fd < 0) {
        // Cleanup on failure
        uintptr_t cleanup_irq_flags = spinlock_acquire_irqsave(&current_process->fd_table_lock);
        if (read_fd >= 0) {
            current_process->fd_table[read_fd] = NULL;
        }
        if (write_fd >= 0) {
            current_process->fd_table[write_fd] = NULL;
        }
        spinlock_release_irqrestore(&current_process->fd_table_lock, cleanup_irq_flags);
        
        kfree(read_sys_file);
        kfree(write_sys_file);
        kfree(read_file);
        kfree(write_file);
        kfree(read_vnode);
        kfree(write_vnode);
        kfree(pipe);
        return -EMFILE;  // Too many open files
    }
    
    // Copy file descriptors to user space
    int pipe_fds[2] = { read_fd, write_fd };
    
    if (copy_to_user(user_pipefd, pipe_fds, sizeof(pipe_fds)) != 0) {
        // Cleanup on copy failure
        uintptr_t copy_cleanup_irq_flags = spinlock_acquire_irqsave(&current_process->fd_table_lock);
        current_process->fd_table[read_fd] = NULL;
        current_process->fd_table[write_fd] = NULL;
        spinlock_release_irqrestore(&current_process->fd_table_lock, copy_cleanup_irq_flags);
        
        kfree(read_sys_file);
        kfree(write_sys_file);
        kfree(read_file);
        kfree(write_file);
        kfree(read_vnode);
        kfree(write_vnode);
        kfree(pipe);
        return -EFAULT;
    }
    
    serial_printf("[Pipe] Created pipe: read_fd=%d, write_fd=%d for PID %u\n", 
                  read_fd, write_fd, current_process->pid);
    
    return 0;  // Success
}

static int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // TODO: Add parent PID tracking to PCB structure
    // For now, return 1 (init process) as parent for all processes
    return 1; // Simplified - all processes have init as parent
}

//-----------------------------------------------------------------------------
// Advanced Shell Support System Calls
//-----------------------------------------------------------------------------

static int32_t sys_dup2_impl(uint32_t oldfd, uint32_t newfd, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) return -ESRCH;
    
    // Basic validation
    if ((int)oldfd < 0 || (int)oldfd >= MAX_FD || (int)newfd < 0 || (int)newfd >= MAX_FD) {
        return -EBADF;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&current->fd_table_lock);
    
    sys_file_t *old_sf = current->fd_table[oldfd];
    if (!old_sf) {
        spinlock_release_irqrestore(&current->fd_table_lock, flags);
        return -EBADF;
    }
    
    // If newfd == oldfd, just return newfd
    if (oldfd == newfd) {
        spinlock_release_irqrestore(&current->fd_table_lock, flags);
        return newfd;
    }
    
    // Close existing newfd if it's open
    if (current->fd_table[newfd]) {
        // TODO: Close the file descriptor properly
        current->fd_table[newfd] = NULL;
    }
    
    // Duplicate the file descriptor
    current->fd_table[newfd] = old_sf; // Simple duplication - same file object
    
    spinlock_release_irqrestore(&current->fd_table_lock, flags);
    
    serial_printf("[Dup2] Duplicated fd %u to fd %u for PID %u\n", oldfd, newfd, current->pid);
    return newfd;
}

static int32_t sys_signal_impl(uint32_t signum, uint32_t user_handler_ptr, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    int signal = (int)signum;
    void *handler = (void*)user_handler_ptr;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // Validate signal number
    if (signal <= 0 || signal >= SIGNAL_MAX) {
        return -EINVAL;
    }
    
    // Validate handler pointer if not SIG_DFL or SIG_IGN
    if (handler != SIG_DFL && handler != SIG_IGN) {
        if (!access_ok(VERIFY_READ, (userptr_t)handler, sizeof(void*))) {
            return -EFAULT;
        }
    }
    
    // Register the signal handler
    void *old_handler = signal_register_handler(current, signal, handler, 0);
    
    if (old_handler == SIG_ERR) {
        return -EINVAL;
    }
    
    serial_printf("[Signal] Registered handler %p for signal %d in PID %u\n", 
                  handler, signal, current->pid);
    
    return (int32_t)(uintptr_t)old_handler;
}

static int32_t sys_kill_impl(uint32_t pid, uint32_t sig, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    uint32_t target_pid = pid;
    int signal = (int)sig;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // Validate signal number
    if (signal < 0 || signal >= SIGNAL_MAX) {
        return -EINVAL;
    }
    
    // Signal 0 is used to test if process exists
    if (signal == 0) {
        pcb_t *target = process_get_by_pid(target_pid);
        return target ? 0 : -ESRCH;
    }
    
    // Special handling for PID values
    if (target_pid == 0) {
        // Send to process group (simplified: send to current process)
        target_pid = current->pid;
    } else if ((int32_t)target_pid == -1) {
        // Send to all processes except init (simplified: not implemented)
        return -EPERM;
    } else if ((int32_t)target_pid < -1) {
        // Send to process group (simplified: not implemented)
        return -ESRCH;
    }
    
    // Send the signal
    int result = signal_send(target_pid, signal, current->pid);
    
    if (result == 0) {
        serial_printf("[Kill] Process PID %u sent signal %d to PID %u\n", 
                      current->pid, signal, target_pid);
        
        // Special logging for SIGKILL
        if (signal == SIGKILL) {
            serial_printf("[Kill] SIGKILL sent to PID %u - process will be terminated\n", target_pid);
        }
    }
    
    return result;
}

static int32_t sys_chdir_impl(uint32_t user_path_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    
    const_userptr_t user_path = (const_userptr_t)user_path_ptr;
    char k_path[MAX_SYSCALL_STR_LEN];
    
    int copy_err = strncpy_from_user_safe(user_path, k_path, sizeof(k_path));
    if (copy_err != 0) return copy_err;
    
    pcb_t *current = get_current_process();
    if (!current) return -ESRCH;
    
    // TODO: Implement proper directory changing with VFS
    // For now, just validate the path exists
    serial_printf("[Chdir] Process PID %u changing directory to '%s'\n", current->pid, k_path);
    
    // Basic validation - directory should exist
    // TODO: Use VFS to check if directory exists
    return 0; // Success (stub implementation)
}

static int32_t sys_getcwd_impl(uint32_t user_buf_ptr, uint32_t size, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    userptr_t user_buf = (userptr_t)user_buf_ptr;
    
    if (!access_ok(VERIFY_WRITE, user_buf, size)) {
        return -EFAULT;
    }
    
    pcb_t *current = get_current_process();
    if (!current) return -ESRCH;
    
    // TODO: Implement proper current working directory tracking
    // For now, return root directory
    const char *cwd = "/";
    size_t cwd_len = strlen(cwd) + 1;
    
    if (size < cwd_len) {
        return -ERANGE;
    }
    
    if (copy_to_user(user_buf, (const_kernelptr_t)cwd, cwd_len) != 0) {
        return -EFAULT;
    }
    
    return cwd_len - 1; // Return length without null terminator
}

static int32_t sys_stat_impl(uint32_t user_path_ptr, uint32_t user_stat_ptr, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    const_userptr_t user_path = (const_userptr_t)user_path_ptr;
    userptr_t user_stat = (userptr_t)user_stat_ptr;
    char k_path[MAX_SYSCALL_STR_LEN];
    
    int copy_err = strncpy_from_user_safe(user_path, k_path, sizeof(k_path));
    if (copy_err != 0) return copy_err;
    
    if (!access_ok(VERIFY_WRITE, user_stat, 64)) { // Assume stat struct is ~64 bytes
        return -EFAULT;
    }
    
    // TODO: Implement proper stat structure and VFS stat operation
    // For now, just return basic file existence
    serial_printf("[Stat] Checking file '%s'\n", k_path);
    
    // TODO: Use VFS to get file information
    return -ENOENT; // File not found (stub implementation)
}

static int32_t sys_readdir_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs) {
    (void)regs;
    
    userptr_t user_buf = (userptr_t)user_buf_ptr;
    
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        return -EFAULT;
    }
    
    pcb_t *current = get_current_process();
    if (!current) return -ESRCH;
    
    // TODO: Implement proper directory reading with VFS
    serial_printf("[Readdir] Reading directory entries from fd %u\n", fd);
    
    return 0; // No entries (stub implementation)
}

//============================================================================
// Process Groups and Sessions System Calls Implementation
//============================================================================

static int32_t sys_setsid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    int result = process_setsid(current);
    serial_printf("[Syscall] setsid() called by PID %u, result=%d\n", current->pid, result);
    
    return result;
}

static int32_t sys_getsid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *target = NULL;
    
    if (pid == 0) {
        // Get SID of calling process
        target = get_current_process();
    } else {
        // Get SID of specified process
        target = process_get_by_pid(pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    uint32_t sid = process_getsid(target);
    serial_printf("[Syscall] getsid(%u) called, result=%u\n", pid, sid);
    
    return sid;
}

static int32_t sys_setpgid_impl(uint32_t pid, uint32_t pgid, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;
    
    pcb_t *target = NULL;
    
    if (pid == 0) {
        // Set PGID of calling process
        target = get_current_process();
    } else {
        // Set PGID of specified process
        target = process_get_by_pid(pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    int result = process_setpgid(target, pgid);
    serial_printf("[Syscall] setpgid(%u, %u) called, result=%d\n", pid, pgid, result);
    
    return result;
}

static int32_t sys_getpgid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *target = NULL;
    
    if (pid == 0) {
        // Get PGID of calling process
        target = get_current_process();
    } else {
        // Get PGID of specified process
        target = process_get_by_pid(pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    uint32_t pgid = process_getpgid(target);
    serial_printf("[Syscall] getpgid(%u) called, result=%u\n", pid, pgid);
    
    return pgid;
}

static int32_t sys_getpgrp_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    uint32_t pgid = process_getpgid(current);
    serial_printf("[Syscall] getpgrp() called by PID %u, result=%u\n", current->pid, pgid);
    
    return pgid;
}

static int32_t sys_tcsetpgrp_impl(uint32_t fd, uint32_t pgid, uint32_t arg3, isr_frame_t *regs) {
    (void)fd; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // TODO: Validate file descriptor refers to a terminal
    // For now, simplified implementation
    
    int result = process_tcsetpgrp(current, pgid);
    serial_printf("[Syscall] tcsetpgrp(%u, %u) called by PID %u, result=%d\n", 
                  fd, pgid, current->pid, result);
    
    return result;
}

static int32_t sys_tcgetpgrp_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)fd; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // TODO: Validate file descriptor refers to a terminal
    // For now, simplified implementation
    
    int result = process_tcgetpgrp(current);
    serial_printf("[Syscall] tcgetpgrp(%u) called by PID %u, result=%d\n", 
                  fd, current->pid, result);
    
    return result;
}

//-----------------------------------------------------------------------------
// Main Syscall Dispatcher
//-----------------------------------------------------------------------------
int32_t syscall_dispatcher(isr_frame_t *regs) {
    KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");

    uint32_t syscall_num = regs->eax;
    uint32_t arg1_ebx    = regs->ebx;
    uint32_t arg2_ecx    = regs->ecx;
    uint32_t arg3_edx    = regs->edx;
    uint32_t arg4_esi    = regs->esi;
    uint32_t arg5_edi    = regs->edi;
    int32_t ret_val;

    pcb_t* current_proc = get_current_process();
    
    // Debug output for first few syscalls
    static int syscall_count = 0;
    if (syscall_count < 10) {
        serial_printf("[SYSCALL DEBUG] Syscall #%d: num=%u, arg1=%x, arg2=%x, arg3=%x\n", 
                      syscall_count++, syscall_num, arg1_ebx, arg2_ecx, arg3_edx);
    }
    
    if (!current_proc && syscall_num != SYS_EXIT && syscall_num != __NR_exit) {
        KERNEL_PANIC_HALT("Syscall (not SYS_EXIT) executed without process context!");
        //This path should ideally not be reached if process management is robust.
        return -EFAULT; 
    }

    // Check if we're in Linux compatibility mode
    if (linux_compat_mode && syscall_num < __NR_syscalls && syscall_num >= 100) {
        // Use Linux syscall dispatcher for syscalls >= 100
        ret_val = linux_syscall_dispatcher(syscall_num, arg1_ebx, arg2_ecx, 
                                         arg3_edx, arg4_esi, arg5_edi);
    } else if (syscall_num < MAX_SYSCALLS && syscall_table[syscall_num] != NULL) {
        // Use CoalOS native syscalls (including low-numbered ones)
        ret_val = syscall_table[syscall_num](arg1_ebx, arg2_ecx, arg3_edx, regs);
    } else {
        ret_val = -ENOSYS;
    }

    regs->eax = (uint32_t)ret_val;
    return ret_val;
}