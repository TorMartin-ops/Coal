#include "syscall.h"    // Syscall numbers and context struct
#include "terminal.h"   // For terminal output
#include "process.h"    // For get_current_process() etc.
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For file-related syscall implementations (SYS_OPEN, READ, CLOSE...)
// #include "mm.h"      // For VMA checks via uaccess.h -> uaccess.c
// #include "paging.h"  // For KERNEL_SPACE_VIRT_START (via uaccess)
#include "kmalloc.h"    // For kmalloc/kfree (used in SYS_WRITE example)
#include "string.h"     // For memcpy/memset (careful usage)
#include "uaccess.h"    // For access_ok, copy_from_user, copy_to_user
#include "fs_errno.h"   // For error codes like EFAULT, ENOSYS, EBADF, EINVAL, ENOMEM
#include "assert.h"     // For KERNEL_PANIC_HALT, KERNEL_ASSERT

// Define standard file descriptors (adjust if different)
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/**
 * @brief The main C entry point for system calls.
 * Dispatches calls based on ctx->eax, validates args, uses uaccess functions.
 *
 * @param ctx Pointer to the saved register context from syscall.asm.
 * @return Value to place back in the user process's EAX register.
 */
int syscall_handler(syscall_context_t *ctx) {
    // Syscall number is passed in EAX by convention
    uint32_t syscall_num = ctx->eax;
    // Arguments are typically in EBX, ECX, EDX, ESI, EDI, EBP (check specific ABI)
    // Let's assume standard Linux i386 convention: EBX, ECX, EDX, ESI, EDI, EBP

    pcb_t* current_proc = get_current_process();
    if (!current_proc && syscall_num != SYS_EXIT) {
        terminal_printf("[Syscall] Error: PID ??? : Syscall %lu without process context!\n", syscall_num);
        return -EFAULT; // Cannot proceed safely
    }
    uint32_t current_pid = current_proc ? current_proc->pid : 0; // Handle SYS_EXIT case

    // Log entry for debugging (optional)
    // terminal_printf("[Syscall] Entry: PID=%u, Num=%u (eax=0x%x, ebx=0x%x, ecx=0x%x, edx=0x%x)\n",
    //                 current_pid, syscall_num, ctx->eax, ctx->ebx, ctx->ecx, ctx->edx);

    int ret_val = -ENOSYS; // Default return: Function not implemented

    switch (syscall_num) {
        case SYS_WRITE: {
            // Args: fd (EBX), user_buf (ECX), count (EDX)
            int fd = (int)ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;
            char* kernel_buf = NULL;
            size_t bytes_to_write = 0;
            size_t total_written = 0;

            // --- Argument Validation ---
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) { // Only support stdout/stderr for now
                 // terminal_printf("[Syscall] PID %u: SYS_WRITE Error: Unsupported fd %d\n", current_pid, fd);
                 ret_val = -EBADF; // Bad file descriptor
                 goto sys_write_cleanup;
            }
            if ((int32_t)count < 0) { // Check for negative size treated as large unsigned
                ret_val = -EINVAL;
                goto sys_write_cleanup;
            }
            if (count == 0) {
                ret_val = 0; // Nothing to write -> success
                goto sys_write_cleanup;
            }

            // Avoid excessive allocation, process in chunks if necessary (or limit)
            const size_t MAX_WRITE_CHUNK = 1024; // Write in chunks
            bytes_to_write = (count < MAX_WRITE_CHUNK) ? count : MAX_WRITE_CHUNK;

            // --- Allocate Kernel Buffer ---
            kernel_buf = kmalloc(bytes_to_write);
            if (!kernel_buf) {
                // terminal_printf("[Syscall] PID %u: SYS_WRITE Error: kmalloc failed (%u bytes)\n", current_pid, bytes_to_write);
                ret_val = -ENOMEM; // Out of memory
                goto sys_write_cleanup;
            }

            // --- Loop for chunked copying and writing ---
            while (total_written < count) {
                size_t current_chunk_size = (count - total_written < bytes_to_write) ? (count - total_written) : bytes_to_write;
                size_t not_copied;

                // --- Validate User Pointer Access for the *current chunk* ---
                if (!access_ok(VERIFY_READ, user_buf + total_written, current_chunk_size)) {
                    // terminal_printf("[Syscall] PID %u: SYS_WRITE Error: Invalid user buffer read access [addr=%p, size=%u).\n",
                    //            current_pid, user_buf + total_written, current_chunk_size);
                    ret_val = -EFAULT; // Bad address
                    goto sys_write_cleanup; // Exit loop on error
                }

                // --- Copy Data From User (Safe) ---
                not_copied = copy_from_user(kernel_buf, user_buf + total_written, current_chunk_size);

                if (not_copied > 0) {
                    // Fault occurred during copy_from_user
                    size_t copied_this_time = current_chunk_size - not_copied;
                    // Write the bytes that *were* successfully copied before the fault
                    for (size_t i = 0; i < copied_this_time; i++) {
                        terminal_write_char(kernel_buf[i]); // Assuming terminal_write_char exists
                    }
                    total_written += copied_this_time;
                    // terminal_printf("[Syscall] PID %u: SYS_WRITE Fault after copying %u bytes.\n", current_pid, total_written);
                    ret_val = -EFAULT; // Signal the overall failure
                    goto sys_write_cleanup; // Exit loop on error
                }

                // --- Perform Actual Write Operation (to terminal) ---
                for (size_t i = 0; i < current_chunk_size; i++) {
                    terminal_write_char(kernel_buf[i]);
                }
                total_written += current_chunk_size;
            } // end while loop

            // If we finished the loop without error
            ret_val = (int)total_written; // Return total bytes successfully written

        sys_write_cleanup:
            if (kernel_buf) {
                kfree(kernel_buf);
            }
            break; // End case SYS_WRITE
        }

        case SYS_EXIT: {
            int exit_code = (int)ctx->ebx; // Exit code in EBX
            // terminal_printf("[Syscall] Process PID %u requested SYS_EXIT with code %d.\n",
            //                current_pid, exit_code);

            // This function should handle process termination, resource cleanup,
            // and eventually call the scheduler. It should NOT return.
            remove_current_task_with_code(exit_code);

            // This point should be unreachable
            KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
            break;
        }

        // --- Add other system calls here ---
        /*
        case SYS_READ: {
            int fd = (int)ctx->ebx;
            void *user_buf = (void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;
            // Validate fd, count
            // if (!access_ok(VERIFY_WRITE, user_buf, count)) return -EFAULT;
            // Allocate kernel buffer
            // Perform read into kernel buffer (e.g., from keyboard, file)
            // size_t not_copied = copy_to_user(user_buf, kernel_buf, actual_bytes_read);
            // kfree buffer
            // if (not_copied > 0) return -EFAULT; else return actual_bytes_read;
            ret_val = -ENOSYS; // Placeholder
            break;
        }
        case SYS_OPEN: {
            const char *user_pathname = (const char*)ctx->ebx;
            int flags = (int)ctx->ecx; // O_RDONLY etc.
            // Need to copy pathname from user safely
            // size_t path_len = strnlen_user(user_pathname, MAX_PATH_LEN); // Need safe strlen
            // if (path_len error or too long) return -EFAULT / -ENAMETOOLONG;
            // if (!access_ok(VERIFY_READ, user_pathname, path_len + 1)) return -EFAULT;
            // char kernel_path[MAX_PATH_LEN];
            // size_t not_copied = copy_from_user(kernel_path, user_pathname, path_len + 1);
            // if (not_copied > 0) return -EFAULT;
            // Call VFS open function: vfs_open(kernel_path, flags);
            // Return file descriptor or error code.
            ret_val = -ENOSYS; // Placeholder
            break;
        }
        */

        default:
        terminal_printf("[Syscall] Error: PID %lu requested unknown syscall number %lu.\n", (unsigned long)current_pid, (unsigned long)syscall_num);
            ret_val = -ENOSYS; // Function not implemented
            break;
    }

    // Set the return value in the context's EAX register for the user process
    ctx->eax = (uint32_t)ret_val;

    // Log exit for debugging (optional)
    // terminal_printf("[Syscall] Exit: PID=%u, Num=%u, Return=0x%x (%d)\n",
    //                 current_pid, syscall_num, ctx->eax, ret_val);

    return ret_val; // Also return for consistency, though EAX in ctx is what matters
}