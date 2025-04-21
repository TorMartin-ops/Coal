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

// Add mapping for user program syscall numbers
// These match the syscall numbers in hello.c
#define USER_SYS_WRITE 1
#define USER_SYS_EXIT  2

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
    if (!current_proc && syscall_num != USER_SYS_EXIT) {
        terminal_printf("[Syscall] Error: PID ??? : Syscall %lu without process context!\n", syscall_num);
        return -EFAULT; // Cannot proceed safely
    }
    uint32_t current_pid = current_proc ? current_proc->pid : 0; // Handle SYS_EXIT case

    // Debug log entry for syscall tracking
    terminal_printf("[Syscall] Entry: PID=%lu, Num=%lu (eax=0x%lx, ebx=0x%lx)\n",
                   (unsigned long)current_pid, 
                   (unsigned long)syscall_num, 
                   (unsigned long)ctx->eax, 
                   (unsigned long)ctx->ebx);

    int ret_val = -ENOSYS; // Default return: Function not implemented

    // Map user program syscall numbers to kernel internal syscall numbers
    switch (syscall_num) {
        case USER_SYS_WRITE: {
            // Args: fd (EBX), user_buf (ECX), count (EDX)
            int fd = (int)ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx;
            size_t count = (size_t)ctx->edx;
            char* kernel_buf = NULL;
            size_t bytes_to_write = 0;
            size_t total_written = 0;

            terminal_printf("[Syscall] SYS_WRITE: fd=%d, buf=%p, count=%lu\n", 
                          fd, user_buf, (unsigned long)count);

            // --- Argument Validation ---
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) { // Only support stdout/stderr for now
                 terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Unsupported fd %d\n", (unsigned long)current_pid, fd);
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
            kernel_buf = kmalloc(bytes_to_write + 1); // +1 for null terminator in debug prints
            if (!kernel_buf) {
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: kmalloc failed (%lu bytes)\n", 
                              (unsigned long)current_pid, (unsigned long)bytes_to_write);
                ret_val = -ENOMEM; // Out of memory
                goto sys_write_cleanup;
            }

            // --- Loop for chunked copying and writing ---
            while (total_written < count) {
                size_t current_chunk_size = (count - total_written < bytes_to_write) ? 
                                           (count - total_written) : bytes_to_write;
                size_t not_copied;

                // --- Validate User Pointer Access for the *current chunk* ---
                if (!access_ok(VERIFY_READ, (char*)user_buf + total_written, current_chunk_size)) {
                    terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid user buffer read access [addr=%p, size=%lu).\n",
                                  (unsigned long)current_pid, (char*)user_buf + total_written, 
                                  (unsigned long)current_chunk_size);
                    ret_val = -EFAULT; // Bad address
                    goto sys_write_cleanup; // Exit loop on error
                }

                // --- Copy Data From User (Safe) ---
                not_copied = copy_from_user(kernel_buf, (char*)user_buf + total_written, current_chunk_size);

                if (not_copied > 0) {
                    // Fault occurred during copy_from_user
                    size_t copied_this_time = current_chunk_size - not_copied;
                    // Write the bytes that *were* successfully copied before the fault
                    for (size_t i = 0; i < copied_this_time; i++) {
                        terminal_write_char(kernel_buf[i]); // Assuming terminal_write_char exists
                    }
                    total_written += copied_this_time;
                    terminal_printf("[Syscall] PID %lu: SYS_WRITE Fault after copying %lu bytes.\n", 
                                  (unsigned long)current_pid, (unsigned long)total_written);
                    ret_val = -EFAULT; // Signal the overall failure
                    goto sys_write_cleanup; // Exit loop on error
                }

                // Add null terminator for debug print (don't count in total)
                kernel_buf[current_chunk_size] = '\0';
                terminal_printf("[Syscall] Writing: \"%s\"\n", kernel_buf);

                // --- Perform Actual Write Operation (to terminal) ---
                for (size_t i = 0; i < current_chunk_size; i++) {
                    terminal_write_char(kernel_buf[i]);
                }
                total_written += current_chunk_size;
            } // end while loop

            // If we finished the loop without error
            ret_val = (int)total_written; // Return total bytes successfully written
            terminal_printf("[Syscall] SYS_WRITE completed: %d bytes written\n", ret_val);

        sys_write_cleanup:
            if (kernel_buf) {
                kfree(kernel_buf);
            }
            break; // End case SYS_WRITE
        }

        case USER_SYS_EXIT: {
            int exit_code = (int)ctx->ebx; // Exit code in EBX
            terminal_printf("[Syscall] Process PID %lu requested SYS_EXIT with code %d.\n",
                           (unsigned long)current_pid, exit_code);

            // This function should handle process termination, resource cleanup,
            // and eventually call the scheduler. It should NOT return.
            remove_current_task_with_code(exit_code);

            // This point should be unreachable
            KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
            break;
        }

        // --- Add other system calls here ---

        default:
            terminal_printf("[Syscall] Error: PID %lu requested unknown syscall number %lu.\n", 
                         (unsigned long)current_pid, (unsigned long)syscall_num);
            ret_val = -ENOSYS; // Function not implemented
            break;
    }

    // Set the return value in the context's EAX register for the user process
    ctx->eax = (uint32_t)ret_val;

    // Log exit for debugging
    terminal_printf("[Syscall] Exit: PID=%lu, Num=%lu, Return=%d\n",
                   (unsigned long)current_pid, (unsigned long)syscall_num, ret_val);

    return ret_val; // Also return for consistency, though EAX in ctx is what matters
}