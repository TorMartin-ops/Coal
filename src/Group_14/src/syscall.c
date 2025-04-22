#include "syscall.h"
#include "terminal.h"   // For terminal output (STDOUT/STDERR & debugging)
#include "process.h"    // For get_current_process(), pcb_t
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For sys_open, sys_read, sys_write, sys_close, sys_lseek impls
#include "kmalloc.h"    // For kmalloc/kfree
#include "string.h"     // For memcpy/memset/strncpy (careful usage)
#include "uaccess.h"    // For access_ok, copy_from_user, copy_to_user
#include "fs_errno.h"   // For error codes (EFAULT, ENOSYS, EBADF, EINVAL, ENOMEM, etc.)
#include "fs_limits.h"  // For MAX_PATH_LEN, MAX_FD
#include "vfs.h"        // For SEEK_SET, SEEK_CUR, SEEK_END definitions (via sys_file.h)
#include "assert.h"     // For KERNEL_PANIC_HALT, KERNEL_ASSERT
#include "debug.h"      // For DEBUG_PRINTK

// Define standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Define limits
#define MAX_SYSCALL_STR_LEN MAX_PATH_LEN // Limit for pathnames from user
#define MAX_RW_CHUNK_SIZE 4096           // Max bytes to copy in one kernel buffer (page size?)

// Define MIN macro (ensure it's defined, e.g., in types.h or here)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Forward declarations for static syscall implementation functions
static int sys_exit_impl(uint32_t exit_code, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_read_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_write_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags, uint32_t mode, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_close_impl(uint32_t fd, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_lseek_impl(uint32_t fd, uint32_t offset, uint32_t whence, uint32_t arg3, uint32_t arg4, uint32_t arg5);
static int sys_not_implemented(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);

// The system call dispatch table
static syscall_fn_t syscall_table[MAX_SYSCALLS];

//-----------------------------------------------------------------------------
// Syscall Initialization
//-----------------------------------------------------------------------------

void syscall_init(void) {
    DEBUG_PRINTK("Initializing system call table (max %d syscalls)...\n", MAX_SYSCALLS);
    // Initialize all entries to point to the 'not implemented' function
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented;
    }

    // Register implemented system calls
    syscall_table[SYS_EXIT]  = sys_exit_impl;
    syscall_table[SYS_READ]  = sys_read_impl;
    syscall_table[SYS_WRITE] = sys_write_impl;
    syscall_table[SYS_OPEN]  = sys_open_impl;
    syscall_table[SYS_CLOSE] = sys_close_impl;
    syscall_table[SYS_LSEEK] = sys_lseek_impl;
    // Add other syscalls here...
    // syscall_table[SYS_GETPID] = sys_getpid_impl;
    // syscall_table[SYS_FORK]   = sys_fork_impl;
    // syscall_table[SYS_BRK]    = sys_brk_impl;

    DEBUG_PRINTK("System call table initialized.\n");
}

//-----------------------------------------------------------------------------
// Static Helper Functions
//-----------------------------------------------------------------------------

/**
 * @brief Copies a null-terminated string from user space safely.
 * Uses copy_from_user internally to handle page faults.
 *
 * @param u_src User space address of the string.
 * @param k_dst Kernel space buffer to copy into.
 * @param maxlen Maximum number of bytes to copy into k_dst (including null terminator).
 * @return 0 on success, -ENAMETOOLONG if string exceeds maxlen (k_dst is null-terminated),
 * -EFAULT if a page fault occurs during copy.
 */
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
    if (maxlen == 0) return -EINVAL;

    size_t len = 0;
    size_t remaining = maxlen;

    while (remaining > 0) {
        char current_char;
        size_t not_copied = copy_from_user(&current_char, u_src + len, 1);

        if (not_copied > 0) {
            // Fault occurred. Ensure buffer is null terminated if anything was copied.
            if (len < maxlen) {
                k_dst[len] = '\0'; // Or maybe the character just before fault?
            } else if (maxlen > 0) {
                 k_dst[maxlen - 1] = '\0';
            }
            DEBUG_PRINTK("[strncpy_from_user] Fault copying byte %u from %p\n", len, u_src + len);
            return -EFAULT;
        }

        k_dst[len] = current_char;
        len++;
        remaining--;

        if (current_char == '\0') {
            // Success: Found null terminator within maxlen.
            return 0;
        }
    }

    // Reached maxlen without finding null terminator.
    k_dst[maxlen - 1] = '\0'; // Ensure null termination.
    DEBUG_PRINTK("[strncpy_from_user] String exceeded maxlen %u\n", maxlen);
    return -ENAMETOOLONG;
}


//-----------------------------------------------------------------------------
// Syscall Implementation Functions (Static)
//-----------------------------------------------------------------------------

static int sys_not_implemented(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    // Avoid unused parameter warnings (optional)
    (void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

    pcb_t* current_proc = get_current_process();
    uint32_t pid = current_proc ? current_proc->pid : 0;
    // Note: Accessing the original syscall number requires peeking at the stack frame
    // or modifying the dispatcher to pass it. For simplicity, we omit it here.
    DEBUG_PRINTK("[Syscall] PID %u: Called unimplemented syscall.\n", pid);
    return -ENOSYS; // Function not implemented
}

static int sys_exit_impl(uint32_t exit_code, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; // Unused args

    pcb_t* current_proc = get_current_process();
    uint32_t pid = current_proc ? current_proc->pid : 0; // Get PID safely
    DEBUG_PRINTK("[Syscall] PID %u: SYS_EXIT with code %d.\n", pid, (int)exit_code);

    // This function terminates the process and schedules the next. It should not return.
    remove_current_task_with_code((int)exit_code);

    // If it returns, something is fundamentally wrong.
    KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
    return 0; // Unreachable
}

static int sys_read_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg3; (void)arg4; (void)arg5; // Unused args

    void *user_buf = (void*)user_buf_ptr;
    char* kbuf = NULL; // Kernel buffer for chunking
    ssize_t total_read = 0;
    int ret_val = 0;

    // --- Argument Validation ---
    if ((int32_t)count < 0) return -EINVAL; // Negative count is invalid (though size_t shouldn't be negative)
    if (count == 0) return 0;           // Read 0 bytes is success

    // Check user buffer writability *before* allocation/reading
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        DEBUG_PRINTK("[Syscall] SYS_READ: EFAULT checking user buffer %p (size %u)\n", user_buf, count);
        return -EFAULT;
    }

    // Determine chunk size - use stack buffer if count is small? Or always kmalloc?
    // Using kmalloc is safer if MAX_RW_CHUNK_SIZE is large.
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        return -ENOMEM;
    }

    // Loop to read data in chunks
    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - total_read);

        // Call the underlying file operation function (handles FD validation)
        ssize_t bytes_read_this_chunk = sys_read((int)fd, kbuf, current_chunk_size);

        if (bytes_read_this_chunk < 0) {
            // Error from sys_read (e.g., -EBADF, -EIO)
            ret_val = (total_read > 0) ? total_read : bytes_read_this_chunk; // Return bytes read so far or the error
            goto sys_read_cleanup_and_exit;
        }

        if (bytes_read_this_chunk == 0) {
            // End Of File reached
            break; // Exit loop, return total_read so far
        }

        // Copy the chunk read into kernel buffer back to user space
        size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, bytes_read_this_chunk);

        if (not_copied > 0) {
            // Fault writing back to user space
            size_t copied_back_this_chunk = bytes_read_this_chunk - not_copied;
            total_read += copied_back_this_chunk;
            DEBUG_PRINTK("[Syscall] SYS_READ: EFAULT copying back %u bytes to user %p\n", bytes_read_this_chunk, (char*)user_buf + total_read - copied_back_this_chunk);
            // Return bytes successfully read & copied, or EFAULT if none were copied this time
            ret_val = (total_read > 0) ? total_read : -EFAULT;
            goto sys_read_cleanup_and_exit;
        }

        // Successfully read and copied back this chunk
        total_read += bytes_read_this_chunk;

         // If sys_read returned fewer bytes than requested, it implies EOF or similar, stop reading.
         if ((size_t)bytes_read_this_chunk < current_chunk_size) {
             break;
         }
    } // end while

    ret_val = total_read; // Return total bytes successfully read and copied

sys_read_cleanup_and_exit:
    if (kbuf) kfree(kbuf);
    return ret_val;
}


static int sys_write_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg3; (void)arg4; (void)arg5; // Unused args

    const void *user_buf = (const void*)user_buf_ptr;
    char* kbuf = NULL;
    ssize_t total_written = 0;
    int ret_val = 0;

    // --- Argument Validation ---
    if ((int32_t)count < 0) return -EINVAL;
    if (count == 0) return 0;

    // Check user buffer readability *before* proceeding
    if (!access_ok(VERIFY_READ, user_buf, count)) {
         DEBUG_PRINTK("[Syscall] SYS_WRITE: EFAULT checking user buffer %p (size %u)\n", user_buf, count);
        return -EFAULT;
    }

     // Allocate kernel buffer for chunking
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        return -ENOMEM;
    }

    // Handle console output directly for efficiency (if applicable)
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        while(total_written < (ssize_t)count) {
            size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
            // Copy chunk from user space
            size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);

            size_t copied_this_chunk = current_chunk_size - not_copied;

            // Write the successfully copied part to the terminal character by character
            for (size_t i = 0; i < copied_this_chunk; i++) {
                 // Assuming terminal_write_char exists in terminal.h/c
                 terminal_write_char(kbuf[i]);
            }
            total_written += copied_this_chunk;

            // If copy_from_user failed, stop and return bytes written or EFAULT
            if (not_copied > 0) {
                DEBUG_PRINTK("[Syscall] SYS_WRITE (Console): EFAULT copying from user %p\n", (char*)user_buf + total_written);
                ret_val = (total_written > 0) ? total_written : -EFAULT;
                goto sys_write_cleanup_and_exit;
            }
        }
        ret_val = total_written; // Success for console write
    }
    // Handle writing to actual files via sys_file
    else {
        // FD validation is handled implicitly by sys_write below.
        while(total_written < (ssize_t)count) {
            size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
            // Copy chunk from user space
            size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);

            size_t copied_this_chunk = current_chunk_size - not_copied;

            // If we managed to copy *something* from user space, try writing it
            if (copied_this_chunk > 0) {
                ssize_t bytes_written_this_chunk = sys_write((int)fd, kbuf, copied_this_chunk);

                if (bytes_written_this_chunk < 0) {
                    // Error during sys_write (e.g., -EBADF, -ENOSPC)
                    ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk;
                    goto sys_write_cleanup_and_exit;
                }
                total_written += bytes_written_this_chunk;

                // If sys_write wrote fewer bytes than we gave it, stop (e.g., disk full)
                if ((size_t)bytes_written_this_chunk < copied_this_chunk) {
                    break; // Return total written so far
                }
            }

            // If copy_from_user failed entirely or partially, stop processing.
            if (not_copied > 0) {
                DEBUG_PRINTK("[Syscall] SYS_WRITE (File): EFAULT copying from user %p\n", (char*)user_buf + total_written);
                ret_val = (total_written > 0) ? total_written : -EFAULT;
                goto sys_write_cleanup_and_exit;
            }
        }
        ret_val = total_written; // Success for file write
    }

sys_write_cleanup_and_exit:
    if (kbuf) kfree(kbuf);
    return ret_val;
}


static int sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags, uint32_t mode, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg3; (void)arg4; (void)arg5; // Unused args

    const char *user_pathname = (const char*)user_pathname_ptr;
    // Allocate kernel buffer for path on the stack if reasonably small, else kmalloc
    // Using stack is generally faster if MAX_SYSCALL_STR_LEN isn't huge.
    char k_pathname[MAX_SYSCALL_STR_LEN]; // Kernel buffer for path

    // Safely copy pathname from user space using the improved helper
    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, MAX_SYSCALL_STR_LEN);
    if (copy_err != 0) {
         DEBUG_PRINTK("[Syscall] SYS_OPEN: Error %d copying path from user %p\n", copy_err, user_pathname);
        return copy_err; // Will be -EFAULT or -ENAMETOOLONG
    }

    // DEBUG_PRINTK("[Syscall] SYS_OPEN: Path='%s', Flags=0x%x, Mode=0x%x\n", k_pathname, flags, mode);

    // Call the underlying sys_open function (which handles VFS interaction)
    // sys_open handles process context checking and FD allocation internally
    int fd = sys_open(k_pathname, (int)flags, (int)mode);

    // sys_open returns fd (>= 0) or negative errno on failure
    return fd;
}

static int sys_close_impl(uint32_t fd, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; // Unused args

    // DEBUG_PRINTK("[Syscall] SYS_CLOSE: fd=%d\n", fd);
    // Call the underlying sys_close function (handles VFS and FD table)
    // sys_close handles process context checking and FD validation internally
    int ret = sys_close((int)fd); // Returns 0 or negative errno
    return ret;
}

static int sys_lseek_impl(uint32_t fd, uint32_t offset, uint32_t whence, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)arg3; (void)arg4; (void)arg5; // Unused args

    // Basic validation for whence (should match SEEK_SET, SEEK_CUR, SEEK_END)
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        return -EINVAL;
    }

    // DEBUG_PRINTK("[Syscall] SYS_LSEEK: fd=%d, offset=%d, whence=%d\n", fd, offset, whence);

    // Call the underlying sys_lseek function
    // sys_lseek handles process context and FD validation
    // Need casting for offset potentially
    off_t result_offset = sys_lseek((int)fd, (off_t)offset, (int)whence);

    // Returns new offset (>=0) or negative errno. Cast result back to int for return.
    // Potential issue if off_t > 32 bits, but likely fine for i386
    return (int)result_offset;
}


//-----------------------------------------------------------------------------
// Main Syscall Dispatcher (Called by Assembly)
//-----------------------------------------------------------------------------

/**
 * @brief The C handler called by the assembly stub.
 * Dispatches the syscall based on the number in regs->eax.
 * Arguments are retrieved from the regs structure based on convention.
 * The return value is placed back into regs->eax.
 */
void syscall_dispatcher(syscall_regs_t *regs) {
    uint32_t syscall_num = regs->eax;
    int ret_val = -ENOSYS; // Default return: not implemented

    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        // Critical check: Syscalls require a process context.
        // Allow SYS_EXIT maybe? Otherwise, panic.
        if (syscall_num == SYS_EXIT) {
             DEBUG_PRINTK("[Syscall] Warning: PID ??? : SYS_EXIT without process context! Halting.\n");
             KERNEL_PANIC_HALT("SYS_EXIT without process context!");
             // Unreachable
        } else {
            DEBUG_PRINTK("[Syscall] FATAL: Syscall %u without process context! Halting.\n", syscall_num);
            KERNEL_PANIC_HALT("Syscall without process context!");
            // Unreachable
        }
        // Set a return value just in case panic doesn't halt immediately
        regs->eax = (uint32_t)-EFAULT;
        return;
    }
    // uint32_t current_pid = current_proc->pid; // For logging if needed


    // --- Syscall Dispatch via Table ---
    if (syscall_num < MAX_SYSCALLS) {
        syscall_fn_t handler = syscall_table[syscall_num];
        if (handler) {
            // Call the specific implementation function
            // Arguments passed according to convention: ebx, ecx, edx, esi, edi, ebp
            ret_val = handler(regs->ebx, regs->ecx, regs->edx, regs->esi, regs->edi, regs->ebp);
        } else {
            // Should not happen if table initialized correctly, but handle defensively
            DEBUG_PRINTK("[Syscall] PID %u: Error: NULL handler for syscall %u.\n", current_proc->pid, syscall_num);
            ret_val = -ENOSYS;
        }
    } else {
        DEBUG_PRINTK("[Syscall] PID %u: Error: Invalid syscall number %u (>= MAX_SYSCALLS).\n", current_proc->pid, syscall_num);
        ret_val = -ENOSYS;
    }

    // Set the return value in the context's EAX register for the user process
    regs->eax = (uint32_t)ret_val;

    // DEBUG_PRINTK("[Syscall] Exit: PID=%u, Num=%u, Return=%d (0x%x)\n",
    //              current_pid, syscall_num, ret_val, regs->eax);
}