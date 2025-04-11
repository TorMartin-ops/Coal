// src/syscall.c

#include "syscall.h"
#include "terminal.h"
#include "process.h"    // For get_current_process()
#include "scheduler.h"  // For remove_current_task_with_code()
#include "sys_file.h"   // For sys_write() and potentially others
#include "mm.h"         // For mm_struct and potentially memory syscalls
// Include frame.h if syscalls need to interact with frames (e.g., mmap/munmap)
#include "frame.h"      // To call frame functions if needed by syscalls

/**
 * syscall_handler
 *
 * The main C entry point for system calls invoked via INT 0x80.
 * Dispatches the call based on the syscall number in ctx->eax.
 *
 * @param ctx Pointer to the saved register context from the interrupt.
 * @return Value to place in EAX for the user process (syscall return value).
 */
int syscall_handler(syscall_context_t *ctx) {
    int syscall_num = ctx->eax; // Syscall number passed in EAX

    switch (syscall_num) {
        case SYS_WRITE: {
            // SYS_WRITE ( fd, buf, count )
            // Args expected in EBX, ECX, EDX (typical convention)
            // Although ctx has registers saved, accessing directly might be easier
            // if the calling convention is well-defined. Let's assume args are
            // pushed or in standard registers before INT 0x80.
            // For simplicity, let's assume EBX=fd, ECX=buf, EDX=count was setup.
            // NOTE: This is a simplistic view, real OS passes args on user stack.

            int fd = ctx->ebx;
            const char *buf = (const char *)ctx->ecx;
            size_t count = ctx->edx;

            // Basic validation (should be more robust)
            if (fd == 1) { // Assume fd 1 is stdout
                // Need to verify user pointer 'buf' is valid!
                // For now, just write to terminal directly.
                // A real implementation would use sys_write from sys_file.c
                // which interacts with VFS.
                 for (size_t i = 0; i < count; i++) {
                     // TODO: Add pointer validation check here!
                     terminal_write_char(buf[i]);
                 }
                ctx->eax = count; // Return number of bytes written
            } else {
                 terminal_printf("[Syscall] SYS_WRITE: Unsupported fd %d\n", fd);
                ctx->eax = -1; // Return error
            }
            break;
        }

        case SYS_EXIT: {
            // SYS_EXIT ( code )
            // Arg expected in EBX
            int exit_code = ctx->ebx;
             pcb_t* current_proc = get_current_process();
             terminal_printf("[Syscall] Process PID %d requested SYS_EXIT with code %d.\n",
                            current_proc ? current_proc->pid : 0, exit_code);

            // This function should not return to the caller process.
            // It removes the task and schedules the next one.
            remove_current_task_with_code(exit_code);

            // Should not be reached:
            terminal_write("[Syscall] ERROR: remove_current_task_with_code returned!\n");
             ctx->eax = -1; // Indicate error if somehow reached
            break;
        }

        case SYS_MMAP:
            terminal_write("[Syscall] SYS_MMAP: Not implemented.\n");
            ctx->eax = -1; // Return error (e.g., ENOSYS)
            break;

        case SYS_MUNMAP:
            terminal_write("[Syscall] SYS_MUNMAP: Not implemented.\n");
            ctx->eax = -1;
            break;

        case SYS_BRK:
            terminal_write("[Syscall] SYS_BRK: Not implemented.\n");
            ctx->eax = -1;
            break;

        // Add cases for other system calls (SYS_OPEN, SYS_READ, SYS_CLOSE, SYS_LSEEK etc.)
        // They would likely call functions from sys_file.c after validating arguments.

        default:
             pcb_t* current_proc_def = get_current_process();
            terminal_printf("[Syscall] Error: Unknown syscall number %d requested by PID %d.\n",
                           syscall_num, current_proc_def ? current_proc_def->pid : 0);
            ctx->eax = -1; // Return error (e.g., ENOSYS)
            break;
    }

    // The return value is placed in ctx->eax by the case handlers.
    // The assembly stub will ensure this gets returned to the user process.
    return ctx->eax;
}