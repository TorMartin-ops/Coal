// src/syscall.c

#include "syscall.h"
#include "terminal.h"
#include "process.h"    // For get_current_process()
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For file-related syscall implementations (if any)
#include "mm.h"         // For mm_struct
#include "frame.h"
#include "paging.h"     // For KERNEL_SPACE_VIRT_START
#include "kmalloc.h"    // For kmalloc/kfree
#include "string.h"     // For memcpy
#include "uaccess.h"    // Use the new user access functions
#include "assert.h"     // <<< Added include for KERNEL_PANIC_HALT

/**
 * syscall_handler
 *
 * The main C entry point for system calls invoked via INT 0x80.
 * Dispatches the call based on the syscall number in ctx->eax.
 * Validates arguments and uses copy_from/to_user where appropriate.
 *
 * @param ctx Pointer to the saved register context from the interrupt.
 * @return Value to place in EAX for the user process (syscall return value).
 */
int syscall_handler(syscall_context_t *ctx) {
    int syscall_num = ctx->eax;
    pcb_t* current_proc = get_current_process(); // Get current process for context

    // Check if process context is valid for syscalls needing it
    if (!current_proc && syscall_num != SYS_EXIT) {
         terminal_printf("[Syscall] Error: Syscall %d invoked without valid process context!\n", syscall_num);
         return -EFAULT; // Use a standard error code if defined
    }

    switch (syscall_num) {
        case SYS_WRITE: {
            // Args: fd (EBX), user_buf (ECX), count (EDX)
            int fd = ctx->ebx;
            const void *user_buf = (const void*)ctx->ecx;
            size_t count = ctx->edx;
            char* kernel_buf = NULL;
            size_t bytes_copied = 0;
            int ret_val = -1; // Default to error

            // Use %d for int, %p for pointer, %lu for size_t, %u for uint32_t PID
            // terminal_printf("[Syscall] PID %u: SYS_WRITE(fd=%d, buf=%p, count=%lu)\n",
            //                current_proc ? current_proc->pid : 0,
            //                fd, user_buf, (unsigned long)count);

            // --- Argument Validation ---
            if (fd != 1) {
                 // terminal_printf("[Syscall] SYS_WRITE: Unsupported fd %d\n", fd);
                 ret_val = -EBADF;
                 goto sys_write_cleanup;
            }
            if (count == 0) {
                ret_val = 0;
                goto sys_write_cleanup;
            }
            const size_t MAX_WRITE_SIZE = 4096;
            if (count > MAX_WRITE_SIZE) {
                 // terminal_printf("[Syscall] SYS_WRITE: Count %lu exceeds limit %lu.\n", (unsigned long)count, (unsigned long)MAX_WRITE_SIZE);
                 ret_val = -EINVAL;
                 goto sys_write_cleanup;
            }

            // --- Validate User Pointer with access_ok ---
            if (!access_ok(VERIFY_READ, user_buf, count)) {
                // terminal_printf("[Syscall] SYS_WRITE: Invalid user buffer read access [addr=%p, size=%lu).\n",
                //                user_buf, (unsigned long)count);
                ret_val = -EFAULT;
                goto sys_write_cleanup;
            }

            // --- Allocate Kernel Buffer ---
            kernel_buf = kmalloc(count);
            if (!kernel_buf) {
                // terminal_printf("[Syscall] SYS_WRITE: Failed to allocate kernel buffer (size %lu).\n", (unsigned long)count);
                ret_val = -ENOMEM;
                goto sys_write_cleanup;
            }

            // --- Copy Data From User using (stubbed) copy_from_user ---
            size_t failed_bytes = copy_from_user(kernel_buf, user_buf, count);

            if (failed_bytes > 0) {
                bytes_copied = count - failed_bytes;
                // terminal_printf("[Syscall] SYS_WRITE: copy_from_user failed after copying %lu bytes.\n", (unsigned long)bytes_copied);
                ret_val = -EFAULT;
                goto sys_write_cleanup;
            }
            bytes_copied = count;

            // --- Perform Actual Write (to terminal in this case) ---
            for (size_t i = 0; i < bytes_copied; i++) {
                terminal_write_char(kernel_buf[i]);
            }

            ret_val = (int)bytes_copied; // Return bytes successfully written

        sys_write_cleanup:
            if (kernel_buf) {
                kfree(kernel_buf);
            }
            ctx->eax = ret_val;
            break;

        } // end case SYS_WRITE

        case SYS_EXIT: {
            int exit_code = ctx->ebx;
            // <<< Corrected format specifier for PID to %u >>>
            terminal_printf("[Syscall] Process PID %u requested SYS_EXIT with code %d.\n",
                           current_proc ? current_proc->pid : 0, exit_code);
            remove_current_task_with_code(exit_code);
            // Should not return here
            KERNEL_PANIC_HALT("Returned from remove_current_task_with_code!"); // <<< Needs assert.h
            // ctx->eax = -EFAULT; // Unreachable
            break;
        }

        // Add other syscall cases here...

        default:
            // <<< Corrected format specifier for PID to %u >>>
            terminal_printf("[Syscall] Error: Unknown syscall number %d requested by PID %u.\n",
                           syscall_num, current_proc ? current_proc->pid : 0);
            ctx->eax = -ENOSYS;
            break;
    }

    return ctx->eax;
}