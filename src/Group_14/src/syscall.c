/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations
 * @version 5.5 - Confirmed copy_to_user args; using opaque uaccess pointers.
 * @author Tor Martin Kohle & Gemini
 */

// --- Includes ---
#include "syscall.h"
#include "terminal.h"
#include "process.h"
#include "scheduler.h"
#include "sys_file.h"
#include "kmalloc.h"
#include "string.h"
#include "uaccess.h" // Now includes userptr_t, kernelptr_t definitions
#include "fs_errno.h"
#include "fs_limits.h"
#include "vfs.h"
#include "assert.h"
#include "serial.h"
#include "paging.h"
#include <libc/limits.h>
#include <libc/stdbool.h>
#include <libc/stddef.h>


// --- Constants ---
#define MAX_PUTS_LEN 256
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef MAX_SYSCALL_STR_LEN
#define MAX_SYSCALL_STR_LEN 256
#endif
#ifndef MAX_RW_CHUNK_SIZE
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define MAX_RW_CHUNK_SIZE PAGE_SIZE
#endif
#ifndef MAX_INPUT_LENGTH
#define MAX_INPUT_LENGTH 256 // Should match terminal.h
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
static int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen);
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
static int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen) {
    KERNEL_ASSERT(k_dst != NULL, "k_dst cannot be NULL in strncpy_from_user_safe");
    if (maxlen == 0) return -EINVAL;
    k_dst[0] = '\0';

    // Basic check, uaccess.c's access_ok will do more thorough VMA checks.
    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        return -EFAULT;
    }
    // access_ok is called by copy_from_user, no need to duplicate here if copy_from_user is robust.
    // However, checking for at least 1 byte readability upfront can be a quick filter.
    if (!access_ok(VERIFY_READ, u_src, 1)) { // Check at least one byte can be touched by access_ok
        return -EFAULT;
    }

    size_t len = 0;
    while (len < maxlen -1) { // Leave space for null terminator
        char current_char;
        // Cast u_src for pointer arithmetic if needed by copy_from_user's second param type if it's just `const void*`
        if (copy_from_user((kernelptr_t)&current_char, (const_userptr_t)((const char*)u_src + len), 1) != 0) {
            k_dst[len] = '\0'; // Null-terminate on partial copy due to fault
            return -EFAULT;
        }
        k_dst[len] = current_char;
        if (current_char == '\0') {
            return 0; // Success, null terminator copied
        }
        len++;
    }

    k_dst[len] = '\0'; // Maxlen-1 chars copied, ensure null termination
    
    // Check if the original string was actually longer (meaning truncation occurred)
    char next_char_check;
    if (copy_from_user((kernelptr_t)&next_char_check, (const_userptr_t)((const char*)u_src + len), 1) == 0 && next_char_check != '\0') {
        return -ENAMETOOLONG; // Original string was longer
    }
    // If copy_from_user failed above, it implies the accessible range ended exactly at u_src + len,
    // or there was another fault. If it succeeded and next_char_check is '\0', it fit perfectly.
    return 0; // String fit or was shorter and NUL terminated within maxlen
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

    userptr_t user_buf = (userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t bytes_read_for_user = 0; // Actual number of chars (excl NUL) for user
    char* k_line_buffer = NULL;

    // Removed most serial logs for brevity, assuming they are not needed once working.
    // Add them back if further debugging is required.

    if (count == 0) return -EINVAL; // Cannot even store NUL
    if (!access_ok(VERIFY_WRITE, user_buf, count)) return -EFAULT;

    // Kernel buffer should be at most MAX_INPUT_LENGTH, or smaller if user's buffer (count) is smaller.
    // MAX_INPUT_LENGTH is the physical limit of the underlying terminal input mechanism.
    size_t kernel_buffer_size = MIN(count > 0 ? count : MAX_INPUT_LENGTH, MAX_INPUT_LENGTH);
    if (kernel_buffer_size == 0) kernel_buffer_size = 1; // Ensure at least 1 for NUL if count was 0 but somehow passed

    k_line_buffer = kmalloc(kernel_buffer_size);
    if (!k_line_buffer) return -ENOMEM;

    // terminal_read_line_blocking fills k_line_buffer and returns actual characters read.
    // It should NUL-terminate within k_line_buffer up to kernel_buffer_size.
    ssize_t bytes_from_terminal_device = terminal_read_line_blocking(k_line_buffer, kernel_buffer_size);

    if (bytes_from_terminal_device < 0) { // Error from terminal_read_line_blocking
        kfree(k_line_buffer);
        return bytes_from_terminal_device;
    }

    // Determine actual number of payload characters to copy to user_buf.
    // Max is count-1 to leave space for NUL in user_buf.
    bytes_read_for_user = MIN((size_t)bytes_from_terminal_device, count - 1);

    // Copy payload to user.
    // Signature: copy_to_user(userptr_t u_dst, const_kernelptr_t k_src, size_t n)
    if (copy_to_user(user_buf, (const_kernelptr_t)k_line_buffer, bytes_read_for_user) != 0) {
        kfree(k_line_buffer);
        return -EFAULT; // Failed to copy payload
    }

    // NUL-terminate the string in user space.
    if (copy_to_user((userptr_t)((char*)user_buf + bytes_read_for_user), (const_kernelptr_t)"\0", 1) != 0) {
        // This is more serious, as user might get un-terminated string.
        kfree(k_line_buffer);
        return -EFAULT; // Failed to copy NUL
    }

    kfree(k_line_buffer);
    return (int32_t)bytes_read_for_user; // Return number of actual characters (excluding NUL)
}


static int32_t sys_read_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs) {
    (void)regs;
    int fd = (int)fd_arg;
    userptr_t user_buf = (userptr_t)user_buf_ptr;
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
        KERNEL_ASSERT(current_chunk_size > 0, "Read chunk size zero");

        ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);

        if (bytes_read_this_chunk < 0) {
            if (total_read > 0) break;
            total_read = bytes_read_this_chunk;
            break;
        }
        if (bytes_read_this_chunk == 0) break;

        if (copy_to_user((userptr_t)((char*)user_buf + total_read), (const_kernelptr_t)kbuf, (size_t)bytes_read_this_chunk) != 0) {
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
    const_userptr_t user_buf = (const_userptr_t)user_buf_ptr;
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
        KERNEL_ASSERT(current_chunk_size > 0, "Write chunk size zero");

        size_t not_copied_from_user = copy_from_user((kernelptr_t)kbuf, (const_userptr_t)((const char*)user_buf + total_written), current_chunk_size);
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
    const_userptr_t user_pathname = (const_userptr_t)user_pathname_ptr;
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
    off_t offset = (off_t)(int32_t)offset_arg; // Cast to signed 32-bit then to off_t
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
    const_userptr_t user_str_ptr = (const_userptr_t)user_str_ptr_arg;
    char kbuffer[MAX_PUTS_LEN];

    int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer));
    if (copy_err != 0) return copy_err;

    terminal_write(kbuffer); // sys_puts usually writes to stdout.
    // Standard puts adds a newline. If this sys_puts should too:
    // terminal_putchar('\n');
    return 0; // Return non-negative on success.
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
        KERNEL_PANIC_HALT("Syscall (not SYS_EXIT) executed without process context!");
        //This path should ideally not be reached if process management is robust.
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