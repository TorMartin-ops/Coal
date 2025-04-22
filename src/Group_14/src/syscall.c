// syscall.c
#include "syscall.h"    // Syscall numbers and context struct
#include "terminal.h"   // For terminal output
#include "process.h"    // For get_current_process() etc.
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For file-related syscall implementations (SYS_OPEN, READ, CLOSE...)
#include "kmalloc.h"    // For kmalloc/kfree (used in SYS_WRITE example)
#include "string.h"     // For memcpy/memset (careful usage)
#include "uaccess.h"    // For access_ok, copy_from_user, copy_to_user
#include "fs_errno.h"   // For error codes like EFAULT, ENOSYS, EBADF, EINVAL, ENOMEM
#include "assert.h"     // For KERNEL_PANIC_HALT, KERNEL_ASSERT

// Define standard file descriptors (adjust if different)
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// --- Use syscall numbers consistent with syscall.h ---
#define KERNEL_SYS_WRITE SYS_WRITE // Use definition from syscall.h
#define KERNEL_SYS_EXIT  SYS_EXIT  // Use definition from syscall.h

// Define syscall limits
#define MAX_SYSCALL_STR_LEN 1024

// Define MIN macro
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif


/**
 * @brief The main C entry point for system calls.
 * Dispatches calls based on ctx->eax, validates args, uses uaccess functions.
 *
 * @param ctx Pointer to the saved register context from syscall.asm.
 * @return Value to place back in the user process's EAX register.
 */
int syscall_handler(syscall_context_t *ctx) {
    uint32_t syscall_num = ctx->eax;
    pcb_t* current_proc = get_current_process();

    if (!current_proc) {
        // Handle error: syscall without process context
        // Cannot reliably call remove_current_task_with_code here.
        terminal_printf("[Syscall] FATAL: Syscall %lu without process context!\n", (unsigned long)syscall_num);
        KERNEL_PANIC_HALT("Syscall without process context!");
        return -EFAULT; // Unreachable
    }
    uint32_t current_pid = current_proc->pid;

    terminal_printf("[Syscall] Entry: PID=%lu, Num=%lu (eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx)\n",
                   (unsigned long)current_pid, (unsigned long)syscall_num,
                   (unsigned long)ctx->eax, (unsigned long)ctx->ebx,
                   (unsigned long)ctx->ecx, (unsigned long)ctx->edx);

    int ret_val = -ENOSYS; // Default: Function not implemented

    switch (syscall_num) {
        case KERNEL_SYS_WRITE: {
            int fd = (int)ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;

            terminal_printf("[Syscall] SYS_WRITE: fd=%d, buf=%p, count=%lu\n",
                          fd, user_buf, (unsigned long)count);

            // --- Argument Validation ---
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                 terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Unsupported fd %d\n", (unsigned long)current_pid, fd);
                 ret_val = -EBADF;
                 break;
            }
            if ((int32_t)count < 0) { // Check for negative size interpreted as large unsigned
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid count %ld\n", (unsigned long)current_pid, (long)count);
                ret_val = -EINVAL;
                break;
            }
            if (count == 0) { // Writing zero bytes is a no-op success
                ret_val = 0;
                break;
            }
             // Check if user pointer is valid *before* allocation
             if (!access_ok(VERIFY_READ, user_buf, count)) {
                 terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid user buffer read access [addr=%p, size=%lu).\n",
                               (unsigned long)current_pid, user_buf, (unsigned long)count);
                 ret_val = -EFAULT;
                 break;
             }

            // --- REMOVED DEBUG BYPASS ---

            // --- Original Code Re-enabled ---
            char* kernel_buf = NULL;
            // Limit the amount copied in one go to avoid large kernel allocations
            const size_t MAX_WRITE_CHUNK = 256;
            size_t total_written = 0;

            // Allocate a kernel buffer for the chunk
            // Note: A smaller fixed-size buffer on the stack might be okay if MAX_WRITE_CHUNK is small
            kernel_buf = kmalloc(MAX_WRITE_CHUNK);
            if (!kernel_buf) {
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: kmalloc failed (%lu bytes)\n",
                              (unsigned long)current_pid, (unsigned long)MAX_WRITE_CHUNK);
                ret_val = -ENOMEM;
                break; // Exit case KERNEL_SYS_WRITE
            }

            // Loop in case the requested count is larger than our chunk size
            while(total_written < count) {
                size_t current_chunk_size = MIN(MAX_WRITE_CHUNK, count - total_written);
                size_t not_copied;

                // Ensure access_ok check covers the *current* chunk being copied
                 if (!access_ok(VERIFY_READ, (char*)user_buf + total_written, current_chunk_size)) {
                     terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid user buffer read access [addr=%p, size=%lu).\n",
                                   (unsigned long)current_pid, (char*)user_buf + total_written, (unsigned long)current_chunk_size);
                     ret_val = -EFAULT;
                     goto sys_write_cleanup; // Use goto for cleanup in loop
                 }

                not_copied = copy_from_user(kernel_buf, (char*)user_buf + total_written, current_chunk_size);

                if (not_copied > 0) {
                    // Partial copy failure
                    size_t copied_this_chunk = current_chunk_size - not_copied;
                    // Write the part that *was* successfully copied
                    for (size_t i = 0; i < copied_this_chunk; i++) { terminal_write_char(kernel_buf[i]); }
                    total_written += copied_this_chunk;
                    terminal_printf("[Syscall] PID %lu: SYS_WRITE Fault after copying %lu bytes.\n",
                                  (unsigned long)current_pid, (unsigned long)total_written);
                    ret_val = (int)total_written > 0 ? (int)total_written : -EFAULT; // Return bytes written or EFAULT
                    goto sys_write_cleanup; // Use goto for cleanup in loop
                }

                // Write the successfully copied chunk to the terminal
                for (size_t i = 0; i < current_chunk_size; i++) {
                    terminal_write_char(kernel_buf[i]);
                }
                total_written += current_chunk_size;
            } // end while loop

            ret_val = (int)total_written;
            terminal_printf("[Syscall] SYS_WRITE completed: %d bytes written\n", ret_val);

        sys_write_cleanup: // Label for unified cleanup
            if (kernel_buf) { kfree(kernel_buf); }
            // --- End Original Code Re-enabled ---

            break; // End case KERNEL_SYS_WRITE
        }

        case KERNEL_SYS_EXIT: {
            int exit_code = (int)ctx->ebx;
            terminal_printf("[Syscall] Process PID %lu requested SYS_EXIT with code %d.\n",
                           (unsigned long)current_pid, exit_code);
            // This function should not return
            remove_current_task_with_code(exit_code);
            // If it does, something is very wrong
            KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
            break;
        }

        default:
            terminal_printf("[Syscall] Error: PID %lu requested unknown syscall number %lu.\n",
                         (unsigned long)current_pid, (unsigned long)syscall_num);
            ret_val = -ENOSYS;
            break;
    }

    ctx->eax = (uint32_t)ret_val; // Set return value in user process's context

    terminal_printf("[Syscall] Exit: PID=%lu, Num=%lu, Return=%d (0x%lx)\n",
                   (unsigned long)current_pid, (unsigned long)syscall_num, ret_val, (unsigned long)ctx->eax);

    return ret_val; // Return value (also in ctx->eax)
}