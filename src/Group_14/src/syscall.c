#include "syscall.h"
#include "terminal.h"

/**
 * syscall_handler
 *
 * Called by our assembly syscall stub (syscall_handler_asm) after it saves registers.
 * The 'ctx' pointer is a snapshot of the register state at interrupt time. We can
 * read/write 'ctx->eax', 'ctx->ebx', etc. to get/return values.
 */
int syscall_handler(syscall_context_t *ctx) {
    int ret = 0;                // Default return value
    uint32_t call_num = ctx->eax;

    switch (call_num) {
        case SYS_WRITE: {
            // For SYS_WRITE, EBX is the pointer to string
            char *str = (char *)ctx->ebx;
            // In a real OS, validate 'str' is in user memory, etc.
            terminal_write(str);
            ret = 0;
            break;
        }
        case SYS_EXIT: {
            uint32_t exit_code = ctx->ebx;
            terminal_write("Process exited with code: ");
            // In a real OS, convert exit_code to decimal/hex string here.
            // Then kill the process or remove it from scheduler.
            // For demonstration, we halt the CPU forever:
            while (1) {
                __asm__ volatile("hlt");
            }
            // not reached
            break;
        }
        default:
            terminal_write("Unknown syscall invoked.\n");
            ret = -1;
            break;
    }

    // Store the return value in EAX so that upon 'iret', user sees it in EAX.
    ctx->eax = ret;
    return ret;
}
