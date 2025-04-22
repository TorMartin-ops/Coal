#include "syscall.h"    // Syscall numbers and context struct
#include "terminal.h"   // For terminal output (used for STDOUT/STDERR)
#include "process.h"    // For get_current_process(), pcb_t
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For sys_open, sys_read, sys_write, sys_close, sys_lseek
#include "kmalloc.h"    // For kmalloc/kfree
#include "string.h"     // For memcpy/memset/strncpy (careful usage)
#include "uaccess.h"    // For access_ok, copy_from_user, copy_to_user
#include "fs_errno.h"   // For error codes (EFAULT, ENOSYS, EBADF, EINVAL, ENOMEM, ENAMETOOLONG, etc.)
#include "fs_limits.h"  // For MAX_PATH_LEN, MAX_FD
#include "vfs.h"        // For SEEK_SET, SEEK_CUR, SEEK_END definitions
#include "assert.h"     // For KERNEL_PANIC_HALT, KERNEL_ASSERT

// Define standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Define limits
#define MAX_SYSCALL_STR_LEN MAX_PATH_LEN // Limit for pathnames from user
#define MAX_RW_CHUNK 256                 // Max bytes to copy in one kernel buffer for read/write

// Define MIN macro (ensure it's defined, e.g., in types.h or here)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Assume SEEK_* constants are defined (e.g., in vfs.h)
#ifndef SEEK_SET
#define SEEK_SET    0   /* Seek from beginning of file.  */
#define SEEK_CUR    1   /* Seek from current position.  */
#define SEEK_END    2   /* Seek from end of file.  */
#endif

//-----------------------------------------------------------------------------
// Static Helper Functions
//-----------------------------------------------------------------------------

/**
 * @brief Copies a null-terminated string from user space, safely.
 *
 * @param u_src User space address of the string.
 * @param k_dst Kernel space buffer to copy into.
 * @param maxlen Maximum number of bytes to copy (including potential null terminator).
 * @return 0 on success, negative error code on failure (-ENAMETOOLONG, -EFAULT).
 */
static int strncpy_from_user(const char *u_src, char *k_dst, size_t maxlen) {
    if (maxlen == 0) return -EINVAL; // Cannot copy into zero-length buffer

    // Check accessibility of the first byte to catch obvious bad pointers early.
    // We still need byte-by-byte checks or copy_from_user's fault handling below.
    if (!access_ok(VERIFY_READ, u_src, 1)) {
        // terminal_printf("[strncpy_from_user] Initial access_ok failed for src %p\n", u_src);
        return -EFAULT;
    }

    size_t len = 0;
    while (len < maxlen) {
        char current_char;
        // Attempt to copy one byte
        size_t not_copied = copy_from_user(&current_char, u_src + len, 1);

        if (not_copied > 0) {
            // Page fault occurred during copy
            // terminal_printf("[strncpy_from_user] Fault copying byte %lu from %p\n", len, u_src + len);
            return -EFAULT;
        }

        k_dst[len] = current_char;

        if (current_char == '\0') {
            // Success: Found null terminator within maxlen
            return 0;
        }

        len++;
    }

    // Reached maxlen without finding null terminator
    // Ensure the kernel buffer is null-terminated, even if truncated.
    k_dst[maxlen - 1] = '\0';
    // terminal_printf("[strncpy_from_user] String exceeded maxlen %lu\n", maxlen);
    return -ENAMETOOLONG;
}

//-----------------------------------------------------------------------------
// Main Syscall Dispatcher
//-----------------------------------------------------------------------------

/**
 * @brief The main C entry point for system calls.
 * Dispatches calls based on ctx->eax, validates args, uses uaccess functions.
 *
 * @param ctx Pointer to the saved register context from syscall.asm.
 * @return Value to place back in the user process's EAX register (typically 0 on success, negative errno on error).
 */
int syscall_handler(syscall_context_t *ctx) {
    serial_write("[Syscall] Enter C syscall_handler\n");
    uint32_t syscall_num = ctx->eax;
    pcb_t* current_proc = get_current_process();

    // Critical check: Syscalls require a process context.
    if (!current_proc) {
        // Allow SYS_EXIT maybe? Otherwise, panic.
        if (syscall_num == SYS_EXIT) {
             terminal_printf("[Syscall] Warning: PID ??? : SYS_EXIT without process context! Halting.\n");
             KERNEL_PANIC_HALT("SYS_EXIT without process context!");
             return 0; // Unreachable
        } else {
            terminal_printf("[Syscall] FATAL: Syscall %lu without process context!\n", (unsigned long)syscall_num);
            KERNEL_PANIC_HALT("Syscall without process context!");
            return -EFAULT; // Unreachable
        }
    }
    // uint32_t current_pid = current_proc->pid; // For logging if needed

    // Default return value: System call number not implemented.
    int ret_val = -ENOSYS;

    // --- Syscall Dispatch ---
    switch (syscall_num) {

        case SYS_EXIT: { // Syscall 1
            int exit_code = (int)ctx->ebx; // Exit code in EBX
            // terminal_printf("[Syscall] Process PID %lu requested SYS_EXIT with code %d.\n", (unsigned long)current_pid, exit_code);
            // This function should terminate the process and schedule the next one. It should not return.
            remove_current_task_with_code(exit_code);
            // If remove_current_task_with_code returns, something is fundamentally wrong.
            KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
            ret_val = 0; // Unreachable, but satisfies compiler
            break;
        }

        case SYS_READ: { // Syscall 3
            int fd = (int)ctx->ebx;
            void *user_buf = (void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;
            char* kbuf = NULL; // Kernel buffer for chunking
            size_t total_read = 0;

            // --- Argument Validation ---
            // File descriptor validation is handled implicitly by sys_read below.
            if ((int32_t)count < 0) { ret_val = -EINVAL; break; } // Negative count is invalid
            if (count == 0) { ret_val = 0; break; } // Read 0 bytes is success

            // Check user buffer writability *before* allocation/reading
            if (!access_ok(VERIFY_WRITE, user_buf, count)) {
                ret_val = -EFAULT;
                break;
            }

            // Allocate kernel buffer for chunking
            kbuf = kmalloc(MAX_RW_CHUNK);
            if (!kbuf) {
                ret_val = -ENOMEM;
                break;
            }

            // Loop to read data in chunks
            while (total_read < count) {
                size_t current_chunk_size = MIN(MAX_RW_CHUNK, count - total_read);
                // Call the underlying file operation function
                ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);

                if (bytes_read_this_chunk < 0) {
                    // Error from sys_read (e.g., -EBADF, -EIO)
                    ret_val = (total_read > 0) ? (int)total_read : bytes_read_this_chunk; // Return bytes read so far or the error
                    goto sys_read_cleanup;
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
                    // Return bytes successfully read & copied, or EFAULT if none were copied this time
                    ret_val = (total_read > 0) ? (int)total_read : -EFAULT;
                    goto sys_read_cleanup;
                }

                // Successfully read and copied back this chunk
                total_read += bytes_read_this_chunk;

                 // If sys_read returned fewer bytes than requested, it implies EOF or similar condition, stop reading.
                 if ((size_t)bytes_read_this_chunk < current_chunk_size) {
                      break;
                 }
            } // end while

            ret_val = (int)total_read; // Return total bytes successfully read and copied

        sys_read_cleanup:
            if (kbuf) kfree(kbuf);
            break; // End case SYS_READ
        }

        case SYS_WRITE: { // Syscall 4
            int fd = (int)ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;

            // --- Argument Validation ---
             if ((int32_t)count < 0) { ret_val = -EINVAL; break; } // Negative count
             if (count == 0) { ret_val = 0; break; } // Write 0 bytes is success

             // Check user buffer readability *before* proceeding
             if (!access_ok(VERIFY_READ, user_buf, count)) {
                 ret_val = -EFAULT;
                 break;
             }

            // Handle console output directly for efficiency and simplicity
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                char* kbuf = NULL;
                size_t total_written = 0;

                kbuf = kmalloc(MAX_RW_CHUNK); // Allocate kernel buffer for chunking
                if (!kbuf) { ret_val = -ENOMEM; break; }

                while(total_written < count) {
                    size_t current_chunk_size = MIN(MAX_RW_CHUNK, count - total_written);
                    // Copy chunk from user space
                    size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);

                    size_t copied_this_chunk = current_chunk_size - not_copied;

                    // Write the successfully copied part to the terminal
                    for (size_t i = 0; i < copied_this_chunk; i++) {
                        terminal_write_char(kbuf[i]); // Assuming this handles console output
                    }
                    total_written += copied_this_chunk;

                    // If copy_from_user failed, stop and return bytes written or EFAULT
                    if (not_copied > 0) {
                        ret_val = (total_written > 0) ? (int)total_written : -EFAULT;
                        goto sys_write_console_cleanup;
                    }
                }
                ret_val = (int)total_written; // Success, return total bytes written

            sys_write_console_cleanup:
                if (kbuf) kfree(kbuf);

            } else {
                // Handle writing to actual files via sys_file
                char* kbuf = NULL;
                size_t total_written = 0;

                // FD validation is handled implicitly by sys_write below.
                kbuf = kmalloc(MAX_RW_CHUNK); // Allocate kernel buffer for chunking
                if (!kbuf) { ret_val = -ENOMEM; break; }

                while(total_written < count) {
                    size_t current_chunk_size = MIN(MAX_RW_CHUNK, count - total_written);
                    // Copy chunk from user space
                    size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);

                    size_t copied_this_chunk = current_chunk_size - not_copied;

                    // If we managed to copy *something* from user space, try writing it
                    if (copied_this_chunk > 0) {
                        ssize_t bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk);

                        if (bytes_written_this_chunk < 0) {
                            // Error during sys_write (e.g., -EBADF, -ENOSPC)
                            ret_val = (total_written > 0) ? (int)total_written : bytes_written_this_chunk;
                            goto sys_write_file_cleanup;
                        }
                        total_written += bytes_written_this_chunk;

                        // If sys_write wrote fewer bytes than we gave it, stop (e.g., disk full)
                        if ((size_t)bytes_written_this_chunk < copied_this_chunk) {
                            break; // Return total written so far
                        }
                    }

                    // If copy_from_user failed entirely or partially, stop processing.
                    if (not_copied > 0) {
                        ret_val = (total_written > 0) ? (int)total_written : -EFAULT;
                        goto sys_write_file_cleanup;
                    }
                }
                ret_val = (int)total_written; // Success, return total bytes written

            sys_write_file_cleanup:
                 if (kbuf) kfree(kbuf);
            }
            break; // End case SYS_WRITE
        }

        case SYS_OPEN: { // Syscall 5
            const char *user_pathname = (const char*)ctx->ebx;
            int flags = (int)ctx->ecx;
            int mode = (int)ctx->edx; // Mode often ignored if not creating (e.g., O_CREAT)
            char k_pathname[MAX_SYSCALL_STR_LEN]; // Kernel buffer for path

            // Safely copy pathname from user space
            int copy_err = strncpy_from_user(user_pathname, k_pathname, MAX_SYSCALL_STR_LEN);
            if (copy_err != 0) {
                 ret_val = copy_err; // Will be -EFAULT or -ENAMETOOLONG
                 break;
            }

            // Call the underlying sys_open function (which handles VFS interaction)
            ret_val = sys_open(k_pathname, flags, mode);
            // sys_open returns fd (>= 0) or negative errno on failure

            break; // End case SYS_OPEN
        }

        case SYS_CLOSE: { // Syscall 6
            int fd = (int)ctx->ebx;
            // Call the underlying sys_close function (which handles VFS and FD table)
            ret_val = sys_close(fd); // Returns 0 or negative errno
            break; // End case SYS_CLOSE
        }

         case SYS_LSEEK: { // Syscall 19
             int fd = (int)ctx->ebx;
             off_t offset = (off_t)ctx->ecx; // Assuming off_t fits in 32 bits for i386
             int whence = (int)ctx->edx;

             // Basic validation for whence
             if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
                 ret_val = -EINVAL;
                 break;
             }

             // Call the underlying sys_lseek function
             ret_val = sys_lseek(fd, offset, whence); // Returns new offset (>=0) or negative errno
             break; // End case SYS_LSEEK
         }

        // --- Add other syscall cases here ---
        // case SYS_FORK: ret_val = sys_fork(); break; // Example

        default:
            // terminal_printf("[Syscall] Error: PID %lu requested unknown syscall number %lu.\n",
            //              (unsigned long)current_pid, (unsigned long)syscall_num);
            ret_val = -ENOSYS; // Function not implemented
            break;
    }

    // Set the return value in the context's EAX register for the user process
    ctx->eax = (uint32_t)ret_val;

    // Log exit for debugging - can be commented out
    // terminal_printf("[Syscall] Exit: PID=%lu, Num=%lu, Return=%d (0x%lx)\n",
    //                (unsigned long)current_pid, (unsigned long)syscall_num, ret_val, (unsigned long)ctx->eax);

    return ret_val; // Return value (also in ctx->eax)
}
