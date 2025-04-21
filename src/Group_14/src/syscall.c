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

// --- Use syscall numbers consistent with hello.c ---
#define KERNEL_SYS_WRITE 4  // Matches SYS_WRITE in hello.c
#define KERNEL_SYS_EXIT  1  // Matches SYS_EXIT in hello.c

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
    // Syscall number is passed in EAX by convention
    uint32_t syscall_num = ctx->eax;
    // Arguments are typically in EBX, ECX, EDX, ESI, EDI, EBP (check specific ABI)
    // Assuming standard Linux i386 convention: EBX, ECX, EDX, ESI, EDI, EBP

    pcb_t* current_proc = get_current_process();
    // Allow SYS_EXIT even without full process context maybe? Risky. Better to assert.
    if (!current_proc) {
        if (syscall_num == KERNEL_SYS_EXIT) {
             terminal_printf("[Syscall] Warning: PID ??? : SYS_EXIT without process context! Halting.\n");
             KERNEL_PANIC_HALT("SYS_EXIT without process context!");
             return 0; // Should be unreachable
        } else {
            terminal_printf("[Syscall] Error: PID ??? : Syscall %lu without process context!\n", (unsigned long)syscall_num);
            KERNEL_PANIC_HALT("Syscall without process context!");
            return -EFAULT; // Unreachable
        }
    }
    uint32_t current_pid = current_proc->pid;

    // Debug log entry for syscall tracking
    terminal_printf("[Syscall] Entry: PID=%lu, Num=%lu (eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx)\n",
                   (unsigned long)current_pid,
                   (unsigned long)syscall_num,
                   (unsigned long)ctx->eax,
                   (unsigned long)ctx->ebx,
                   (unsigned long)ctx->ecx,
                   (unsigned long)ctx->edx);


    int ret_val = -ENOSYS; // Default return: Function not implemented

    // Use the CORRECT syscall numbers in the switch
    switch (syscall_num) {
        case KERNEL_SYS_WRITE: { // <<< Use 4
            int fd = (int)ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx; // Still read args for logging
            size_t count = (size_t)ctx->edx;

            terminal_printf("[Syscall] SYS_WRITE: fd=%d, buf=%p, count=%lu\n",
                          fd, user_buf, (unsigned long)count);

            // --- Argument Validation ---
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                 terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Unsupported fd %d\n", (unsigned long)current_pid, fd);
                 ret_val = -EBADF;
                 break; // Exit case
            }
            if ((int32_t)count < 0) {
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid count %ld\n", (unsigned long)current_pid, (long)count);
                ret_val = -EINVAL;
                break; // Exit case
            }
            if (count == 0) {
                ret_val = 0;
                break; // Exit case
            }

            // ***************************************************************
            // *** TEMPORARY DEBUGGING: Bypass user memory access ***
            terminal_printf("[Syscall DEBUG] Bypassing copy_from_user for SYS_WRITE.\n");
            // Instead of copying, just print a placeholder message to the kernel console
            const char* temp_msg = "[Kernel simulated write]\n";
            for(const char* p = temp_msg; *p; ++p) {
                terminal_write_char(*p);
            }
            // Pretend we wrote the requested number of bytes
            ret_val = (int)count;
            // ***************************************************************

            /* --- Original Code (Commented Out) ---
            char* kernel_buf = NULL;
            size_t bytes_to_write = 0;
            size_t total_written = 0;

            if (count > MAX_SYSCALL_STR_LEN) {
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Warning: Count %lu exceeds limit %u, truncating.\n",
                    (unsigned long)current_pid, (unsigned long)count, MAX_SYSCALL_STR_LEN);
                count = MAX_SYSCALL_STR_LEN;
             }

            const size_t MAX_WRITE_CHUNK = 256;
            bytes_to_write = MIN(count, MAX_WRITE_CHUNK);

            if (bytes_to_write == 0) {
                ret_val = 0;
                goto sys_write_cleanup;
            }
            kernel_buf = kmalloc(bytes_to_write + 1);
            if (!kernel_buf) {
                terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: kmalloc failed (%lu bytes)\n",
                              (unsigned long)current_pid, (unsigned long)bytes_to_write);
                ret_val = -ENOMEM;
                goto sys_write_cleanup;
            }

            while (total_written < count) {
                size_t current_chunk_size = MIN(bytes_to_write, count - total_written);
                size_t not_copied;
                if (current_chunk_size == 0) break;

                if (!access_ok(VERIFY_READ, (char*)user_buf + total_written, current_chunk_size)) {
                    terminal_printf("[Syscall] PID %lu: SYS_WRITE Error: Invalid user buffer read access [addr=%p, size=%lu).\n",
                                  (unsigned long)current_pid, (char*)user_buf + total_written,
                                  (unsigned long)current_chunk_size);
                    ret_val = -EFAULT;
                    goto sys_write_cleanup;
                }

                not_copied = copy_from_user(kernel_buf, (char*)user_buf + total_written, current_chunk_size);

                if (not_copied > 0) {
                    size_t copied_this_time = current_chunk_size - not_copied;
                    for (size_t i = 0; i < copied_this_time; i++) { terminal_write_char(kernel_buf[i]); }
                    total_written += copied_this_time;
                    terminal_printf("[Syscall] PID %lu: SYS_WRITE Fault after copying %lu bytes.\n",
                                  (unsigned long)current_pid, (unsigned long)total_written);
                    ret_val = (int)total_written > 0 ? (int)total_written : -EFAULT;
                    goto sys_write_cleanup;
                }

                for (size_t i = 0; i < current_chunk_size; i++) { terminal_write_char(kernel_buf[i]); }
                total_written += current_chunk_size;
            }

            ret_val = (int)total_written;
            terminal_printf("[Syscall] SYS_WRITE completed: %d bytes written\n", ret_val);

        sys_write_cleanup:
            if (kernel_buf) { kfree(kernel_buf); }
            */ // --- End Original Code ---

            break; // End case KERNEL_SYS_WRITE
        }

        case KERNEL_SYS_EXIT: { // <<< Use 1
            int exit_code = (int)ctx->ebx; // Exit code in EBX
            terminal_printf("[Syscall] Process PID %lu requested SYS_EXIT with code %d.\n",
                           (unsigned long)current_pid, exit_code);
            remove_current_task_with_code(exit_code);
            KERNEL_PANIC_HALT("FATAL: Returned from remove_current_task_with_code!");
            break; // End case KERNEL_SYS_EXIT
        }

        default:
            terminal_printf("[Syscall] Error: PID %lu requested unknown syscall number %lu.\n",
                         (unsigned long)current_pid, (unsigned long)syscall_num);
            ret_val = -ENOSYS; // Function not implemented
            break;
    }

    // Set the return value in the context's EAX register for the user process
    ctx->eax = (uint32_t)ret_val;

    // Log exit for debugging
    terminal_printf("[Syscall] Exit: PID=%lu, Num=%lu, Return=%d (0x%lx)\n",
                   (unsigned long)current_pid, (unsigned long)syscall_num, ret_val, (unsigned long)ctx->eax);

    return ret_val; // Also return for consistency, though EAX in ctx is what matters
}