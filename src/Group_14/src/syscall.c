#include "syscall.h"
#include "terminal.h"

/**
 * Handles a system call by dispatching based on the syscall number.
 * In production, add proper pointer validation and error checking.
 */
int syscall_handler(syscall_context_t *ctx) {
    int ret = 0;
    uint32_t call_num = ctx->eax;

    switch (call_num) {
        case SYS_WRITE: {
            char *str = (char *)ctx->ebx;
            // Validate that str is a valid pointer in production.
            terminal_write(str);
            ret = 0;
            break;
        }
        case SYS_EXIT: {
            uint32_t exit_code = ctx->ebx;
            terminal_write("Process exited with code: ");
            // Optionally convert exit_code to string.
            while (1) { __asm__ volatile("hlt"); }
            break;
        }
        default:
            terminal_write("Unknown syscall invoked.\n");
            ret = -1;
            break;
    }
    return ret;
}
