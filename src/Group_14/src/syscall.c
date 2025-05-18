/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations
 * @version 5.3 - Corrected copy_to_user arguments in sys_read_terminal_line_impl and added NUL term.
 * @author Tor Martin Kohle & Gemini
 *
 * Implements the C-level system call dispatcher and backend logic for
 * essential syscalls (open, read, write, close, exit, etc.). This layer
 * bridges user-space requests with kernel-side file system and process
 * management, including robust user memory access and validation.
 */

// --- Includes ---
#include "syscall.h"
#include "terminal.h"       // For sys_puts, STDOUT/STDERR via sys_write, terminal_read_line_blocking
#include "process.h"
#include "scheduler.h"
#include "sys_file.h"       // Kernel-level file operations
#include "kmalloc.h"
#include "string.h"
#include "uaccess.h"        // copy_from_user, copy_to_user, access_ok
#include "fs_errno.h"       // Standardized error codes (EINVAL, EFAULT, etc.)
#include "fs_limits.h"      // MAX_PATH_LEN
#include "vfs.h"
#include "assert.h"
#include "serial.h"         // Low-level serial output for critical debug
#include "paging.h"         // KERNEL_SPACE_VIRT_START, PAGE_SIZE
#include <libc/limits.h>
#include <libc/stdbool.h>
#include <libc/stddef.h>    // For NULL, size_t
// #include "debug.h" // DEBUG_PRINTK can be enabled via build flags

// --- Constants ---
#define MAX_PUTS_LEN 256
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Ensure MAX_SYSCALL_STR_LEN and MAX_RW_CHUNK_SIZE are defined.
// MAX_PATH_LEN typically comes from fs_limits.h.
// MAX_INPUT_LENGTH from terminal.h
// PAGE_SIZE from paging.h.
#ifndef MAX_SYSCALL_STR_LEN
#define MAX_SYSCALL_STR_LEN 256 // Fallback if not in fs_limits.h
#endif
#ifndef MAX_RW_CHUNK_SIZE
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096 // Define if not available
#endif
#define MAX_RW_CHUNK_SIZE PAGE_SIZE
#endif
#ifndef MAX_INPUT_LENGTH // Fallback if not in terminal.h (it is defined there)
#define MAX_INPUT_LENGTH 256
#endif


// --- Utility Macros ---
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// --- Static Data ---
static syscall_fn_t syscall_table[MAX_SYSCALLS];

// --- Forward Declarations of Syscall Implementations ---
static int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_read_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
static int32_t sys_write_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
static int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags, uint32_t mode, isr_frame_t *regs);
static int32_t sys_close_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_lseek_impl(uint32_t fd, uint32_t offset, uint32_t whence, isr_frame_t *regs);
static int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_puts_impl(uint32_t user_str_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen);
static int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, isr_frame_t *regs);



//-----------------------------------------------------------------------------
// Syscall Initialization
//-----------------------------------------------------------------------------
void syscall_init(void) {
    serial_write("[Syscall] Initializing table...\n");
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented;
    }
    syscall_table[SYS_EXIT]   = sys_exit_impl;
    syscall_table[SYS_READ]   = sys_read_impl;
    syscall_table[SYS_WRITE]  = sys_write_impl;
    syscall_table[SYS_OPEN]   = sys_open_impl;
    syscall_table[SYS_CLOSE]  = sys_close_impl;
    syscall_table[SYS_LSEEK]  = sys_lseek_impl;
    syscall_table[SYS_GETPID] = sys_getpid_impl;
    syscall_table[SYS_PUTS]   = sys_puts_impl;
    syscall_table[SYS_READ_TERMINAL_LINE] = sys_read_terminal_line_impl;

    KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "SYS_EXIT assignment sanity check failed!");
    serial_write("[Syscall] Table initialized.\n");
}

//-----------------------------------------------------------------------------
// Static Helper: Safe String Copy from User Space
//-----------------------------------------------------------------------------
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
    KERNEL_ASSERT(k_dst != NULL, "k_dst cannot be NULL in strncpy_from_user_safe");
    if (maxlen == 0) return -EINVAL;
    k_dst[0] = '\0';

    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        return -EFAULT;
    }
    if (!access_ok(VERIFY_READ, u_src, 1)) {
        return -EFAULT;
    }

    size_t len = 0;
    while (len < maxlen -1) { // Ensure space for null terminator
        char current_char;
        if (copy_from_user(&current_char, u_src + len, 1) != 0) {
            k_dst[len] = '\0';
            return -EFAULT;
        }
        k_dst[len] = current_char;
        if (current_char == '\0') {
            return 0;
        }
        len++;
    }

    k_dst[len] = '\0';
    char next_char_check; // Check if original string was longer
    if (copy_from_user(&next_char_check, u_src + len, 1) == 0 && next_char_check != '\0') {
        return -ENAMETOOLONG;
    }
    return 0;
}

//-----------------------------------------------------------------------------
// Syscall Implementations
//-----------------------------------------------------------------------------
static int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3;
    serial_write("[Syscall] WARNING: Unimplemented syscall #");
    serial_print_hex(regs->eax);
    serial_write(" called by PID ");
    pcb_t* proc = get_current_process();
    serial_print_hex(proc ? proc->pid : 0xFFFFFFFF);
    serial_write("\n");
    return -ENOSYS;
}

static int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    remove_current_task_with_code(code);
    KERNEL_PANIC_HALT("sys_exit returned!");
    return 0;
}

static int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count_arg, uint32_t arg3, isr_frame_t *regs) {
    (void)arg3; (void)regs;

    void *user_buf = (void*)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t bytes_to_copy_to_user = 0;
    char* k_line_buffer = NULL;

    // This log was present in the panic, indicating the function was entered.
    // serial_write("[Syscall] sys_read_terminal_line_impl: Enter\n");

    if (count == 0) {
        // serial_write("[Syscall] sys_read_terminal_line_impl: Error - Count is 0.\n");
        return -EINVAL;
    }
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        // serial_write("[Syscall] sys_read_terminal_line_impl: Error - User buffer access denied (EFAULT).\n");
        return -EFAULT;
    }

    size_t kernel_buffer_size = MIN(count, MAX_INPUT_LENGTH);
     if (kernel_buffer_size == 0 && count > 0) kernel_buffer_size = 1;


    k_line_buffer = kmalloc(kernel_buffer_size);
    if (!k_line_buffer) {
        // serial_write("[Syscall] sys_read_terminal_line_impl: Error - kmalloc failed for kernel buffer (ENOMEM).\n");
        return -ENOMEM;
    }
    // Log from panic: Kernel buffer allocated. Size: 00000100 Addr: D000B418.
    // serial_write("[Syscall] sys_read_terminal_line_impl: Kernel buffer allocated. Size: ");
    // serial_print_hex((uint32_t)kernel_buffer_size);
    // serial_write(" Addr: "); serial_print_hex((uintptr_t)k_line_buffer); serial_write(".\n");

    // serial_write("[Syscall] sys_read_terminal_line_impl: Calling terminal_read_line_blocking...\n");
    ssize_t bytes_read_from_terminal = terminal_read_line_blocking(k_line_buffer, kernel_buffer_size);

    // Log from panic: terminal_read_line_blocking returned: 0000000F ('asdwa e ASD ASD').
    // serial_write("[Syscall] sys_read_terminal_line_impl: terminal_read_line_blocking returned: ");
    // serial_print_hex((uint32_t)bytes_read_from_terminal);
    // if (bytes_read_from_terminal >= 0) {
    //     serial_write(" ('"); serial_write(k_line_buffer); serial_write("')");
    // }
    // serial_write(".\n");


    if (bytes_read_from_terminal < 0) {
        kfree(k_line_buffer);
        return bytes_read_from_terminal;
    }

    bytes_to_copy_to_user = MIN((size_t)bytes_read_from_terminal, count - 1);

    // Log from panic: Attempting to copy to user. Count: 0000000F UserBuf Addr: 080483C0.
    // serial_write("[Syscall] sys_read_terminal_line_impl: Attempting to copy to user. Count: ");
    // serial_print_hex((uint32_t)bytes_to_copy_to_user);
    // serial_write(" UserBuf Addr: "); serial_print_hex((uintptr_t)user_buf); serial_write(".\n");

    if (bytes_to_copy_to_user >= 0) {
        // *** CORRECTED ARGUMENT ORDER FOR copy_to_user as per review ***
        // Prototype: int copy_to_user(void *user_dst, const void *k_src, size_t n);
        if (copy_to_user(user_buf, k_line_buffer, bytes_to_copy_to_user) != 0) {
            // serial_write("[Syscall] sys_read_terminal_line_impl: Error - copy_to_user data failed (EFAULT).\n");
            kfree(k_line_buffer);
            return -EFAULT;
        }
        // Explicitly NUL-terminate in user space.
        if (copy_to_user((char*)user_buf + bytes_to_copy_to_user, "\0", 1) != 0) {
            // serial_write("[Syscall] sys_read_terminal_line_impl: Error - copy_to_user null terminator failed (EFAULT).\n");
            kfree(k_line_buffer);
            return -EFAULT;
        }
    }

    // serial_write("[Syscall] sys_read_terminal_line_impl: Successfully copied to user. Actual chars (excl NUL): ");
    // serial_print_hex((uint32_t)bytes_to_copy_to_user);
    // serial_write(".\n");

    kfree(k_line_buffer);
    // serial_write("[Syscall] sys_read_terminal_line_impl: Kernel buffer freed. Exiting.\n");

    return (int32_t)bytes_to_copy_to_user;
}


static int32_t sys_read_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    void *user_buf = (void*)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_read = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    if (!access_ok(VERIFY_WRITE, user_buf, count)) return -EFAULT;

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Read chunk size became zero in loop");

        ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);

        if (bytes_read_this_chunk < 0) {
            if (total_read > 0) break;
            total_read = bytes_read_this_chunk;
            break;
        }
        if (bytes_read_this_chunk == 0) break;

        if (copy_to_user((char*)user_buf + total_read, kbuf, (size_t)bytes_read_this_chunk) != 0) {
            if (total_read > 0) break;
            total_read = -EFAULT;
            break;
        }
        total_read += bytes_read_this_chunk;
        if ((size_t)bytes_read_this_chunk < current_chunk_size) break;
    }
    if (kbuf) kfree(kbuf);
    return total_read;
}

static int32_t sys_write_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    const void *user_buf = (const void*)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_written = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    if (!access_ok(VERIFY_READ, user_buf, count)) return -EFAULT;

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Write chunk size became zero in loop");

        size_t not_copied_from_user = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
        size_t copied_this_chunk_from_user = current_chunk_size - not_copied_from_user;

        if (copied_this_chunk_from_user > 0) {
            ssize_t bytes_written_this_chunk;
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_this_chunk_from_user);
                bytes_written_this_chunk = copied_this_chunk_from_user;
            } else {
                bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk_from_user);
            }

            if (bytes_written_this_chunk < 0) {
                if (total_written > 0) break;
                total_written = bytes_written_this_chunk;
                break;
            }
            total_written += bytes_written_this_chunk;
            if ((size_t)bytes_written_this_chunk < copied_this_chunk_from_user) break;
        }

        if (not_copied_from_user > 0) {
             if (total_written > 0) break;
             total_written = -EFAULT;
             break;
        }
        if (copied_this_chunk_from_user == 0 && not_copied_from_user == 0 && current_chunk_size > 0) {
            if (total_written > 0) break;
            total_written = -EFAULT;
            break;
        }
    }
    if (kbuf) kfree(kbuf);
    return total_written;
}

static int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags_arg, uint32_t mode_arg, isr_frame_t *regs) {
    (void)regs;
    const char *user_pathname = (const char*)user_pathname_ptr;
    int flags = (int)flags_arg;
    int mode = (int)mode_arg;
    char k_pathname[MAX_SYSCALL_STR_LEN];

    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) return copy_err;

    return sys_open(k_pathname, flags, mode);
}

static int32_t sys_close_impl(uint32_t fd_arg, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    int fd = (int)fd_arg;
    return sys_close(fd);
}

static int32_t sys_lseek_impl(uint32_t fd_arg, uint32_t offset_arg, uint32_t whence_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    off_t offset = (off_t)(int32_t)offset_arg;
    int whence = (int)whence_arg;
    return sys_lseek(fd, offset, whence);
}

static int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "get_current_process returned NULL in sys_getpid");
    return (int32_t)current_proc->pid;
}

static int32_t sys_puts_impl(uint32_t user_str_ptr_arg, uint32_t arg2, uint32_t arg3, isr_frame_t *regs) {
    (void)arg2; (void)arg3; (void)regs;
    const char *user_str_ptr = (const char *)user_str_ptr_arg;
    char kbuffer[MAX_PUTS_LEN];

    int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer));
    if (copy_err != 0) return copy_err;

    terminal_write(kbuffer);
    return 0;
}

//-----------------------------------------------------------------------------
// Main Syscall Dispatcher
//-----------------------------------------------------------------------------
int32_t syscall_dispatcher(isr_frame_t *regs) {
    KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");

    uint32_t syscall_num = regs->eax;
    uint32_t arg1_ebx    = regs->ebx;
    uint32_t arg2_ecx    = regs->ecx;
    uint32_t arg3_edx    = regs->edx;
    int32_t ret_val;

    pcb_t* current_proc = get_current_process();
    if (!current_proc && syscall_num != SYS_EXIT) {
        KERNEL_PANIC_HALT("Syscall (not exit) executed without process context!");
        return -EFAULT; 
    }

    if (syscall_num < MAX_SYSCALLS && syscall_table[syscall_num] != NULL) {
        ret_val = syscall_table[syscall_num](arg1_ebx, arg2_ecx, arg3_edx, regs);
    } else {
        ret_val = -ENOSYS;
    }

    regs->eax = (uint32_t)ret_val;
    return ret_val;
}