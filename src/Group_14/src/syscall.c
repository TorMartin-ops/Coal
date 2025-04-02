#include "syscall.h"
#include "terminal.h"
#include "scheduler.h"    // For removing tasks, scheduling next, etc.
#include "kmalloc.h"
#include "string.h"       // For strlen, memcpy, etc.
#include "types.h"


// Example: maximum syscalls
#define MAX_SYSCALL 32

// Common boundary for higher–half kernel at 3GB = 0xC0000000
#define USER_SPACE_LIMIT  0xC0000000

// Spinlock for concurrency if SMP or interrupts. 
// Simplified. Real code might do inline assembly, etc.
static volatile int syscall_lock = 0;
static inline void lock_syscall(void) {
    // naive spinlock
    while (__atomic_test_and_set(&syscall_lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}
static inline void unlock_syscall(void) {
    __atomic_clear(&syscall_lock, __ATOMIC_RELEASE);
}

// ----- Utility: user pointer checks -----

/**
 * is_user_pointer
 *
 * Checks if [ptr..ptr+len) is below USER_SPACE_LIMIT and doesn’t wrap around.
 * In a real OS, also check page directory for ring=3, etc.
 */
static bool is_user_pointer(const void *ptr, size_t len) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end   = start + len;
    // Wrap around check
    if (end < start) return false;
    // Must be strictly below 0xC0000000
    if (end >= USER_SPACE_LIMIT) return false;
    return true;
}

/**
 * copy_from_user
 *
 * Copies 'len' bytes from user-space pointer 'src' to kernel buffer 'dest'.
 * Returns true if success, false if invalid pointer or out-of-bounds.
 */
static bool copy_from_user(void *dest, const void *src, size_t len) {
    if (!is_user_pointer(src, len)) {
        return false;
    }
    // Simplified. Real OS might do page–by–page checks, fault handlers, etc.
    memcpy(dest, src, len);
    return true;
}

// ----- Implementation of sample syscalls -----

/**
 * sys_write
 * 
 * Syscall #1. Writes a user string to the kernel terminal. 
 * 
 * EBX => user pointer to string
 * ECX => maximum length
 * Return => number of chars written or negative on error
 */
static int sys_write(syscall_context_t *ctx) {
    char *usr_str   = (char *)ctx->ebx;
    uint32_t usr_len= ctx->ecx;  // optional length

    if (usr_str == NULL) {
        return -1; // invalid param
    }
    if (usr_len > 65536) {
        // some arbitrary limit to avoid huge copy
        usr_len = 65536;
    }

    // We do a small local buffer for demonstration
    static char local_buf[1024];
    if (usr_len >= sizeof(local_buf)) {
        usr_len = sizeof(local_buf)-1;
    }
    // Copy the user string into local kernel buffer
    if (!copy_from_user(local_buf, usr_str, usr_len)) {
        return -2; // invalid pointer
    }
    local_buf[usr_len] = '\0'; // ensure null terminator

    // Print it
    terminal_write(local_buf);

    // Return number of bytes "written"
    return (int)usr_len;
}

/**
 * sys_exit
 *
 * Syscall #2. Terminates the current process with an exit code.
 * 
 * EBX => exit code
 * No return because we remove the task or schedule next.
 */
static int sys_exit(syscall_context_t *ctx) {
    // For demonstration, we remove the current task from the scheduler
    uint32_t code = ctx->ebx;

    // Optionally log
    terminal_write("[syscall] sys_exit code=");
    // convert code to decimal
    char buf[16];
    // a small function to convert code -> decimal. 
    // e.g. your convert_uint_to_string
    // or do a simpler function. 
    // We'll do a quick naive approach:

    {
        int pos=0;
        if (code == 0) {
            buf[pos++]='0';
        } else {
            unsigned tmp = code;
            char rev[16];
            int rpos=0;
            while (tmp>0 && rpos<(int)sizeof(rev)) {
                rev[rpos++] = (char)('0' + (tmp%10));
                tmp/=10;
            }
            // reverse
            while(rpos>0) {
                buf[pos++]=rev[--rpos];
            }
        }
        buf[pos] = '\0';
    }

    terminal_write(buf);
    terminal_write("\n");

    // remove the current process
    remove_current_task_with_code(code);

    // Not reached
    while(1) {
        __asm__ volatile("hlt");
    }
    return 0; // unreachable
}

/**
 * sys_unknown
 *
 * Called if an unimplemented or unknown syscall is invoked.
 */
static int sys_unknown(syscall_context_t *ctx) {
    terminal_write("[syscall] Unknown call.\n");
    return -1;
}

// ----- Syscall Table -----

#define SYSCALL_NUM 16
// We define a table, up to 16 syscalls for example
static int (*syscall_table[SYSCALL_NUM])(syscall_context_t *ctx) = {
    sys_unknown, // index 0 => unknown
    sys_write,   // index 1 => SYS_WRITE
    sys_exit,    // index 2 => SYS_EXIT
    // ... fill the rest with sys_unknown or other calls ...
};

/**
 * syscall_handler
 *
 * The main dispatcher for all system calls. 
 * The assembly stub pushes the context and calls here with '-nostdlib' environment, 
 * no automatic ring 3 checks unless you configured them in the IDT.
 *
 * concurrency disclaimers:
 *   - If SMP or interrupts, typically do a spinlock or disable ints around scheduling.
 *   - If ring 3, we rely on 'int 0x80' or 'sysenter' for transitions.
 *
 * @param ctx snapshot of CPU registers (EAX=call_num, EBX..=args, etc.)
 * @return 'ctx->eax' is the new EAX on return to user
 */
int syscall_handler(syscall_context_t *ctx) {
    // Acquire a spinlock if you want concurrency protection
    lock_syscall();

    // read call num
    uint32_t num = ctx->eax;
    int ret = 0;

    if (num < SYSCALL_NUM) {
        // call table
        ret = syscall_table[num](ctx);
    } else {
        ret = sys_unknown(ctx);
    }

    // put return value in EAX
    ctx->eax = ret;

    unlock_syscall();
    return ret;
}
