/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations (Corrected v4.8 - Return Value Fix)
 * @version 4.8
 *
 * Implements the system call C-level dispatcher and the backend functions
 * for various system calls like open, read, write, close, exit, etc.
 * Handles user<->kernel memory copying and validation.
 *
 * Changes v4.8:
 * - CORRECTED return value handling in sys_open_impl, sys_read_impl,
 * sys_write_impl, sys_close_impl, sys_lseek_impl. They now return the
 * actual result from the underlying sys_* functions, not arguments.
 * - Removed redundant final_ret_val variables where the direct return
 * from the underlying sys_* function is sufficient.
 */

// --- Includes ---
#include "syscall.h"        // Includes isr_frame.h implicitly now
#include "terminal.h"
#include "process.h"
#include "scheduler.h"
#include "sys_file.h"
#include "kmalloc.h"
#include "string.h"
#include "uaccess.h"        // Essential for pointer validation
#include "fs_errno.h"       // Error codes (EINVAL, EFAULT, ENOSYS, etc.)
#include "fs_limits.h"
#include "vfs.h"
#include "assert.h"
#include "debug.h"          // Provides DEBUG_PRINTK
#include "serial.h"
#include "paging.h"         // Include for KERNEL_SPACE_VIRT_START
#include <libc/limits.h>    // INT32_MIN, INT32_MAX (assuming int is 32-bit)

// Define a max length for puts to prevent unbounded reads
#define MAX_PUTS_LEN 256
#define DEBUG_SYSCALL 1 // Keep debug on for now

#if DEBUG_SYSCALL
// Use the kernel's primary debug print mechanism for syscall logs
#define SYSCALL_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Syscall] " fmt "\n", ##__VA_ARGS__)
#else
#define SYSCALL_DEBUG_PRINTK(fmt, ...) ((void)0)
#endif
#define SYSCALL_ERROR(fmt, ...) DEBUG_PRINTK("[Syscall ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)


// --- Constants ---
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define MAX_SYSCALL_STR_LEN MAX_PATH_LEN // Max length for paths copied from user
#define MAX_RW_CHUNK_SIZE   PAGE_SIZE    // Max bytes to copy in one chunk

// --- Utility Macros ---
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

// --- Static Data ---
// Syscall handler function table
static syscall_fn_t syscall_table[MAX_SYSCALLS];

// --- Forward Declarations ---
// Syscall implementation functions
static int sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int sys_not_implemented(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);

// Helper function for safe user string copy
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen);
extern void serial_print_hex(uint32_t n); // Assumed available from serial.h

//-----------------------------------------------------------------------------
// Syscall Initialization
//-----------------------------------------------------------------------------

/**
 * @brief Initializes the system call table, mapping syscall numbers to handlers.
 */
void syscall_init(void) {
    serial_write(" FNC_ENTER: syscall_init\n");
    DEBUG_PRINTK("Initializing system call table (max %d syscalls)...\n", MAX_SYSCALLS);
    serial_write("  STEP: Looping to init table\n");
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented; // Default to not implemented
    }
    serial_write("  STEP: Registering handlers\n");
    syscall_table[SYS_EXIT]   = sys_exit_impl;
    syscall_table[SYS_READ]   = sys_read_impl;
    syscall_table[SYS_WRITE]  = sys_write_impl;
    syscall_table[SYS_OPEN]   = sys_open_impl;
    syscall_table[SYS_CLOSE]  = sys_close_impl;
    syscall_table[SYS_LSEEK]  = sys_lseek_impl;
    syscall_table[SYS_GETPID] = sys_getpid_impl;
    syscall_table[SYS_PUTS]   = sys_puts_impl;
    // Add other syscalls here...

    serial_write("  STEP: Verifying SYS_EXIT assignment...\n");
    KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "syscall_table[SYS_EXIT] assignment failed!");
    serial_write("  STEP: SYS_EXIT assignment OK.\n");
    DEBUG_PRINTK("System call table initialization complete.\n");
    serial_write(" FNC_EXIT: syscall_init\n");
}

//-----------------------------------------------------------------------------
// Static Helper: Safe String Copy from User Space
//-----------------------------------------------------------------------------

/**
 * @brief Safely copies a null-terminated string from user space to kernel space.
 * Performs boundary checks and uses copy_from_user for fault handling.
 * @param u_src User space source address.
 * @param k_dst Kernel space destination buffer.
 * @param maxlen Maximum number of bytes to copy (including null terminator).
 * @return 0 on success, EFAULT on access error, -ENAMETOOLONG if maxlen reached before null terminator.
 */
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
    serial_write(" FNC_ENTER: strncpy_from_user_safe\n");
    KERNEL_ASSERT(k_dst != NULL, "Kernel destination buffer cannot be NULL");
    if (maxlen == 0) return EINVAL;
    k_dst[0] = '\0'; // Ensure null termination on error/empty

    // Basic pointer check (NULL or kernel address)
    serial_write("  STEP: Basic u_src check\n");
    serial_write("   DBG: Checking u_src: "); serial_print_hex((uint32_t)u_src);
    serial_write(" against KERNEL_SPACE_VIRT_START: "); serial_print_hex(KERNEL_SPACE_VIRT_START); serial_write("\n");
    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        SYSCALL_ERROR("strncpy: Basic check failed (NULL or kernel addr %p)", u_src);
        serial_write("  RET: EFAULT (bad u_src basic check)\n");
        return EFAULT;
    }

    // Check accessibility using access_ok (verifies VMA existence and permissions)
    serial_write("  STEP: Calling access_ok\n");
    SYSCALL_DEBUG_PRINTK("  strncpy: Checking access_ok(READ, %p, 1 byte)...", u_src);
    // Verify read access for the *potential maximum length* to avoid faults mid-string.
    // Checking only 1 byte is insufficient if the string crosses page boundaries.
    // Let's check the first byte, and rely on copy_from_user's fault handling for the rest.
    // A more robust check might iterate or check the whole maxlen if performance allows.
    if (!access_ok(VERIFY_READ, u_src, 1)) {
        SYSCALL_ERROR("strncpy: access_ok failed for user buffer %p", u_src);
        serial_write("  RET: EFAULT (access_ok failed)\n");
        return EFAULT;
    }
    SYSCALL_DEBUG_PRINTK("  strncpy: access_ok passed for first byte.");

    // Copy byte by byte, checking for null terminator and faults
    SYSCALL_DEBUG_PRINTK("strncpy_from_user_safe: Copying from u_src=%p to k_dst=%p (maxlen=%zu)", u_src, k_dst, maxlen);
    serial_write("  STEP: Entering copy loop\n");
    size_t len = 0;
    while (len < maxlen) {
        char current_char;
        // copy_from_user handles page faults via exception table
        size_t not_copied = copy_from_user(&current_char, u_src + len, 1);
        if (not_copied > 0) {
            serial_write("  LOOP: Fault detected!\n");
            k_dst[len > 0 ? len - 1 : 0] = '\0'; // Try to null-terminate what we got
            SYSCALL_ERROR("strncpy: Fault copying byte %zu from %p. Returning EFAULT", len, u_src + len);
            serial_write("  RET: EFAULT (fault during copy)\n");
            return EFAULT;
        }

        k_dst[len] = current_char;

        if (current_char == '\0') {
            SYSCALL_DEBUG_PRINTK("  strncpy: Found null terminator at length %zu. Success.", len);
            serial_write("  RET: 0 (Success)\n");
            return 0; // Success
        }
        len++;
    }

    // Reached maxlen without finding null terminator
    serial_write("  STEP: Loop finished (maxlen reached)\n");
    k_dst[maxlen - 1] = '\0'; // Ensure null termination
    SYSCALL_ERROR("strncpy: String from %p exceeded maxlen %zu. Returning -ENAMETOOLONG", u_src, maxlen);
    serial_write("  RET: -ENAMETOOLONG\n");
    return -ENAMETOOLONG;
}

//-----------------------------------------------------------------------------
// Syscall Implementations
//-----------------------------------------------------------------------------

/**
 * @brief Handles unimplemented system calls.
 * @return ENOSYS always.
 */
static int sys_not_implemented(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg1_ebx; (void)arg2_ecx; (void)arg3_edx; // Mark unused arguments
    serial_write(" FNC_ENTER: sys_not_implemented\n");
    KERNEL_ASSERT(regs != NULL, "NULL regs");
    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "No process context");
    SYSCALL_ERROR("PID %lu: Called unimplemented syscall %u. Returning ENOSYS.",
                  (unsigned long)current_proc->pid, (unsigned int)regs->eax);
    serial_write(" FNC_EXIT: sys_not_implemented (ENOSYS)\n");
    return ENOSYS; // System call not implemented
}

/**
 * @brief Handles the exit() system call. Terminates the current process.
 * This function should never return.
 */
static int sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_exit_impl\n");
    int exit_code = (int)arg1_ebx; // Exit code is in EBX
    serial_write("  DBG: ExitCode="); serial_print_hex((uint32_t)exit_code); serial_write("\n");

    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "sys_exit called without process context");
    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_EXIT(exit_code=%d) called.", (unsigned long)current_proc->pid, exit_code);

    serial_write("  STEP: Calling remove_current_task_with_code...\n");
    // This function marks the task as ZOMBIE and calls schedule(), it should not return here.
    remove_current_task_with_code(exit_code);

    // Should never be reached
    KERNEL_PANIC_HALT("FATAL: remove_current_task_with_code returned in sys_exit!");
    return 0; // Unreachable code
}

static int sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_read_impl\n");
    int fd                 = (int)arg1_ebx;
    void *user_buf         = (void*)arg2_ecx;
    size_t count           = (size_t)arg3_edx;
    uint32_t pid           = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val     = 0; // Final return value (positive count or negative errno)
    ssize_t total_read     = 0; // Track bytes successfully read AND copied to user
    char* kbuf             = NULL; // Kernel temporary buffer

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_READ(fd=%d, buf=%p, count=%zu)", (unsigned long)pid, fd, user_buf, count);

    // --- 1. Validate Arguments ---
    if ((ssize_t)count < 0) {
        SYSCALL_ERROR("PID %lu: SYS_READ failed - negative count %ld", (unsigned long)pid, (long)count);
        serial_write("  RET: -EINVAL (negative count)\n");
        return -EINVAL;
    }
    if (count == 0) {
        serial_write("  RET: 0 (zero count)\n");
        return 0;
    }
    serial_write("  STEP: Pre-access_ok check for WRITE..."); serial_write(" fd="); serial_print_hex((uint32_t)fd);
    serial_write(" buf="); serial_print_hex((uintptr_t)user_buf); serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        SYSCALL_ERROR("PID %lu: SYS_READ failed - EFAULT (access_ok failed for user buffer %p)", (unsigned long)pid, user_buf);
        serial_write("  RET: EFAULT (access_ok failed)\n");
        return EFAULT;
    }
    serial_write("  STEP: access_ok passed.\n");

    // --- 2. Allocate Kernel Buffer ---
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        SYSCALL_ERROR("PID %lu: SYS_READ failed - ENOMEM (kmalloc failed for size %zu)", (unsigned long)pid, chunk_alloc_size);
        serial_write("  RET: -ENOMEM (kmalloc failed)\n");
        return -ENOMEM;
    }
    SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p (size %zu).", kbuf, chunk_alloc_size);

    // --- 3. Read Loop (Chunked) ---
    serial_write("  STEP: Entering read loop\n");
    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in sys_read loop");
        SYSCALL_DEBUG_PRINTK("  Loop: Reading chunk size %zu (total_read %zd)", current_chunk_size, total_read);

        // Call sys_file backend to read into kernel buffer
        ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
        SYSCALL_DEBUG_PRINTK("   LOOP_READ: sys_read returned %zd", bytes_read_this_chunk);

        if (bytes_read_this_chunk < 0) {
            serial_write("  LOOP: Error from sys_read\n");
            actual_ret_val = bytes_read_this_chunk; // *** Prioritize returning the error code ***
            goto sys_read_cleanup;
        }
        if (bytes_read_this_chunk == 0) {
            serial_write("  LOOP: EOF reached\n");
            actual_ret_val = total_read; // Return total successfully read so far
            goto sys_read_cleanup;
        }

        // Copy data from kernel buffer to user buffer
        size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, bytes_read_this_chunk);
        SYSCALL_DEBUG_PRINTK("   LOOP_READ: copy_to_user returned %zu (not copied)", not_copied);

        if (not_copied > 0) {
            serial_write("  LOOP: Fault during copy_to_user\n");
            SYSCALL_ERROR("PID %lu: SYS_READ failed - EFAULT during copy_to_user (processed %zd bytes OK before fault)", (unsigned long)pid, total_read);
            actual_ret_val = EFAULT; // *** Prioritize returning EFAULT ***
            goto sys_read_cleanup;
        }

        // Successfully copied this chunk
        total_read += bytes_read_this_chunk;

        // Check if the underlying read returned fewer bytes than requested (EOF for this chunk)
        if ((size_t)bytes_read_this_chunk < current_chunk_size) {
            serial_write("  LOOP: Short read from sys_read, breaking loop\n");
            break; // Reached EOF on the underlying read
        }
    }

    actual_ret_val = total_read; // Success, return total bytes read

sys_read_cleanup:
    if (kbuf) kfree(kbuf);
    SYSCALL_DEBUG_PRINTK("  SYS_READ returning %d.", actual_ret_val);
    serial_write(" FNC_EXIT: sys_read_impl\n");
    return actual_ret_val; // Return total bytes read or negative errno
}


/**
 * @brief Handles the write() system call. Writes data to a file descriptor.
 * Copies data from user space buffer to kernel space before writing via sys_write backend.
 */
static int sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_write_impl\n");
    int fd                 = (int)arg1_ebx;
    const void *user_buf   = (const void*)arg2_ecx;
    size_t count           = (size_t)arg3_edx;
    uint32_t pid           = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val     = 0; // *** Store the ACTUAL return value ***

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_WRITE(fd=%d, buf=%p, count=%zu)", (unsigned long)pid, fd, user_buf, count);

    // --- 1. Validate Arguments ---
    if ((ssize_t)count < 0) {
        SYSCALL_ERROR("PID %lu: SYS_WRITE failed - negative count %ld", (unsigned long)pid, (long)count);
        serial_write("  RET: EINVAL (negative count)\n");
        return EINVAL;
    }
    if (count == 0) {
        serial_write("  RET: 0 (zero count)\n");
        return 0;
    }
    serial_write("  STEP: Pre-access_ok check for READ..."); serial_write(" fd="); serial_print_hex((uint32_t)fd);
    serial_write(" buf="); serial_print_hex((uintptr_t)user_buf); serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
    if (!access_ok(VERIFY_READ, user_buf, count)) {
        SYSCALL_ERROR("PID %lu: SYS_WRITE failed - EFAULT (access_ok failed for user buffer %p)", (unsigned long)pid, user_buf);
        serial_write("  RET: EFAULT (access_ok failed)\n");
        return EFAULT;
    }
    serial_write("  STEP: access_ok passed.\n");

    // --- 2. Allocate Kernel Buffer ---
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    char* kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        SYSCALL_ERROR("PID %lu: SYS_WRITE failed - ENOMEM (kmalloc failed for size %zu)", (unsigned long)pid, chunk_alloc_size);
        serial_write("  RET: ENOMEM (kmalloc failed)\n");
        return ENOMEM;
    }
    SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p (size %zu).", kbuf, chunk_alloc_size);

    // --- 3. Write Loop (Chunked) ---
    ssize_t total_written = 0;
    // Removed final_ret_val, use actual_ret_val
    serial_write("  STEP: Entering write loop\n");
    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in sys_write loop");
        SYSCALL_DEBUG_PRINTK("  Loop: Writing chunk size %zu (total_written %zd)", current_chunk_size, total_written);

        size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
        SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: copy_from_user returned %zu (not copied)", not_copied);

        size_t copied_from_user_this_chunk = current_chunk_size - not_copied;

        if (copied_from_user_this_chunk > 0) {
            ssize_t bytes_written_this_chunk = 0;
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_from_user_this_chunk);
                bytes_written_this_chunk = copied_from_user_this_chunk;
                SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: terminal_write_bytes returned (assumed %zd)", bytes_written_this_chunk);
            } else {
                bytes_written_this_chunk = sys_write(fd, kbuf, copied_from_user_this_chunk); // Call sys_file backend
                SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: sys_write returned %zd", bytes_written_this_chunk);
            }

            if (bytes_written_this_chunk < 0) {
                serial_write("  LOOP: Error during write operation\n");
                actual_ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk; // Return partial write or error
                goto sys_write_cleanup;
            }

            total_written += bytes_written_this_chunk;

            if ((size_t)bytes_written_this_chunk < copied_from_user_this_chunk) {
                serial_write("  LOOP: Short write, breaking loop\n");
                break; // Underlying write didn't complete the chunk (e.g., disk full)
            }
        }

        if (not_copied > 0) {
            serial_write("  LOOP: Fault during copy_from_user\n");
            SYSCALL_ERROR("PID %lu: SYS_WRITE failed - EFAULT during copy_from_user (wrote %zd total)", (unsigned long)pid, total_written);
            actual_ret_val = (total_written > 0) ? total_written : EFAULT; // Return partial write or error
            goto sys_write_cleanup;
        }
    }

    actual_ret_val = total_written; // Success, return total bytes written

sys_write_cleanup:
    if (kbuf) kfree(kbuf);
    SYSCALL_DEBUG_PRINTK("  SYS_WRITE returning %d.", actual_ret_val);
    serial_write(" FNC_EXIT: sys_write_impl\n");
    return actual_ret_val; // *** RETURN THE ACTUAL RESULT ***
}

/**
 * @brief Handles the open() system call. Copies path from user, calls sys_open backend.
 */
static int sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_open_impl\n");
    const char *user_pathname = (const char*)arg1_ebx;
    int flags                 = (int)arg2_ecx;
    int mode                  = (int)arg3_edx; // Mode is relevant only for O_CREAT
    uint32_t pid              = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val        = 0; // *** Store the ACTUAL return value ***

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_OPEN(path_user=%p, flags=0x%x, mode=0%o)", (unsigned long)pid, user_pathname, flags, mode);

    // Allocate kernel buffer for pathname
    char k_pathname[MAX_SYSCALL_STR_LEN];
    serial_write("  STEP: Calling strncpy_from_user_safe\n");
    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname));
    SYSCALL_DEBUG_PRINTK("  STEP: strncpy_from_user_safe returned %d", copy_err);

    if (copy_err != 0) {
        SYSCALL_ERROR("PID %lu: SYS_OPEN failed - Error %d copying path from user %p", (unsigned long)pid, copy_err, user_pathname);
        serial_write("  RET: Error from strncpy\n");
        return copy_err; // Return EFAULT or -ENAMETOOLONG
    }
    SYSCALL_DEBUG_PRINTK("  Copied path to kernel: '%s'", k_pathname);

    // Call the backend sys_open function
    serial_write("  STEP: Calling sys_open (underlying)\n");
    actual_ret_val = sys_open(k_pathname, flags, mode); // sys_open handles VFS interaction
    SYSCALL_DEBUG_PRINTK("  STEP: sys_open returned %d", actual_ret_val);
    serial_write(" FNC_EXIT: sys_open_impl\n");
    return actual_ret_val; // *** RETURN THE ACTUAL RESULT ***
}

/**
 * @brief Handles the close() system call. Calls sys_close backend.
 */
static int sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_close_impl\n");
    int fd = (int)arg1_ebx;
    uint32_t pid = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val = 0; // *** Store the ACTUAL return value ***

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_CLOSE(fd=%d)", (unsigned long)pid, fd);

    serial_write("  STEP: Calling sys_close (underlying)\n");
    actual_ret_val = sys_close(fd); // sys_close handles VFS interaction and FD table
    SYSCALL_DEBUG_PRINTK("  STEP: sys_close returned %d", actual_ret_val);
    serial_write(" FNC_EXIT: sys_close_impl\n");
    return actual_ret_val; // *** RETURN THE ACTUAL RESULT ***
}

/**
 * @brief Handles the lseek() system call. Calls sys_lseek backend.
 */
static int sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_lseek_impl\n");
    int fd         = (int)arg1_ebx;
    off_t offset   = (off_t)arg2_ecx; // off_t might be 64-bit later, but passed as 32-bit here
    int whence     = (int)arg3_edx;
    uint32_t pid   = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val = 0; // *** Store the ACTUAL return value ***

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_LSEEK(fd=%d, offset=%ld, whence=%d)", (unsigned long)pid, fd, (long)offset, whence);

    // Basic validation of whence
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         SYSCALL_ERROR("PID %lu: SYS_LSEEK failed - Invalid whence %d", (unsigned long)pid, whence);
         return EINVAL;
    }

    serial_write("  STEP: Calling sys_lseek (underlying)\n");
    off_t result_offset = sys_lseek(fd, offset, whence); // sys_lseek handles VFS
    SYSCALL_DEBUG_PRINTK("  STEP: sys_lseek returned %ld", (long)result_offset);
    serial_write(" FNC_EXIT: sys_lseek_impl\n");

    // Check for negative error code before casting to int
    if (result_offset < 0) {
        // Ensure the error code fits within int range if off_t is larger
        if (result_offset < INT32_MIN) {
            SYSCALL_ERROR("PID %lu: SYS_LSEEK error %ld overflowed int return type", (unsigned long)pid, (long)result_offset);
            actual_ret_val = EINVAL; // Return EINVAL for overflow condition
        } else {
            actual_ret_val = (int)result_offset; // Return negative error code
        }
    } else {
        // Check if positive offset fits within int range
        if (result_offset > INT32_MAX) {
            SYSCALL_ERROR("PID %lu: SYS_LSEEK result %ld overflowed int return type", (unsigned long)pid, (long)result_offset);
            actual_ret_val = EINVAL; // Return EINVAL for overflow condition
        } else {
            actual_ret_val = (int)result_offset; // Return positive offset
        }
    }
    return actual_ret_val; // *** RETURN THE ACTUAL RESULT ***
}

/**
 * @brief Handles the getpid() system call. Returns the current process ID.
 */
static int sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg1_ebx; (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_getpid_impl\n");
    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "sys_getpid called without process context");
    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_GETPID() -> Returning PID %lu", (unsigned long)current_proc->pid, (unsigned long)current_proc->pid);
    serial_write(" FNC_EXIT: sys_getpid_impl\n");
    return (int)current_proc->pid; // PID is uint32_t, should fit in int - Correct return
}

/**
 * @brief Handles the puts() system call (non-standard, writes string to terminal).
 */
static int sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
    serial_write(" FNC_ENTER: sys_puts_impl\n");
    const char *user_str_ptr = (const char *)arg1_ebx;
    uint32_t pid = get_current_process() ? get_current_process()->pid : 0;
    int actual_ret_val = 0; // *** Store the ACTUAL return value ***

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_PUTS(user_str=%p)", (unsigned long)pid, user_str_ptr);

    // Allocate kernel buffer for the string
    char kbuffer[MAX_PUTS_LEN]; // Use stack buffer for simplicity
    SYSCALL_DEBUG_PRINTK("  Calling strncpy_from_user_safe(u_src=%p, k_dst=%p, maxlen=%zu)...",
                         user_str_ptr, kbuffer, sizeof(kbuffer));
    int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer));
    SYSCALL_DEBUG_PRINTK("  strncpy_from_user_safe returned %d.", copy_err);

    if (copy_err != 0) {
        SYSCALL_ERROR("PID %lu: SYS_PUTS failed - Error %d copying string from user %p", (unsigned long)pid, copy_err, user_str_ptr);
        serial_write(" [WARN syscall] sys_puts_impl: Invalid user pointer (strncpy failed code=");
        serial_print_hex((uint32_t)copy_err); serial_write(")\n");
        return copy_err; // Return EFAULT or -ENAMETOOLONG
    }

    SYSCALL_DEBUG_PRINTK("  String copied to kernel buffer: '%.*s'", (int)sizeof(kbuffer)-1, kbuffer);
    SYSCALL_DEBUG_PRINTK("  Calling terminal_write...");
    // Write the string and a newline to the terminal
    terminal_write(kbuffer);
    terminal_write("\n");
    SYSCALL_DEBUG_PRINTK("  terminal_write finished.");
    SYSCALL_DEBUG_PRINTK(" -> SYS_PUTS finished successfully.");
    serial_write(" FNC_EXIT: sys_puts_impl (OK)\n");
    actual_ret_val = 0; // Success for puts is usually 0 or non-negative
    return actual_ret_val; // *** RETURN THE ACTUAL RESULT ***
}

//-----------------------------------------------------------------------------
// Main Syscall Dispatcher
//-----------------------------------------------------------------------------

/**
 * @brief Central dispatcher for system calls invoked via interrupt.
 * Extracts arguments from the interrupt frame, finds the appropriate handler
 * in the syscall_table, calls it, and places the return value back into the
 * frame for the assembly stub.
 * @param regs Pointer to the interrupt stack frame containing saved registers.
 */
void syscall_dispatcher(isr_frame_t *regs) {
    serial_write("SD: Enter (v4.6)\n"); // Version bump
    KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");
    SYSCALL_DEBUG_PRINTK("Dispatcher entered. Frame at %p.", regs);

    // Extract syscall number and arguments from the saved register state
    uint32_t syscall_num = regs->eax;
    uint32_t arg1_ebx    = regs->ebx;
    uint32_t arg2_ecx    = regs->ecx;
    uint32_t arg3_edx    = regs->edx;

    // Log received arguments
    serial_write("SD: Syscall Num (from regs->eax @ offset 28): "); serial_print_hex(syscall_num); serial_write("\n");
    serial_write("SD: Arg 1 (from regs->ebx @ offset 16): "); serial_print_hex(arg1_ebx); serial_write("\n");
    serial_write("SD: Arg 2 (from regs->ecx @ offset 24): "); serial_print_hex(arg2_ecx); serial_write("\n");
    serial_write("SD: Arg 3 (from regs->edx @ offset 20): "); serial_print_hex(arg3_edx); serial_write("\n");
    SYSCALL_DEBUG_PRINTK(" -> Processing syscall number: %u (0x%x)", syscall_num, syscall_num);

    // Ensure we are running in a process context
    serial_write("SD: GetProc\n");
    pcb_t* current_proc = get_current_process();
    serial_write("SD: ChkProc\n");
    if (!current_proc) KERNEL_PANIC_HALT("Syscall executed without process context!");

    int ret_val; // Return value from the syscall handler

    // Validate syscall number and find handler
    serial_write("SD: ChkBounds\n");
    if (syscall_num < MAX_SYSCALLS) {
        serial_write("SD: InBounds\n");
        serial_write("SD: LookupHnd\n");
        syscall_fn_t handler = syscall_table[syscall_num];
        serial_write("SD: ChkHnd\n");
        if (handler) {
            // Call the appropriate handler function
            serial_write("SD: CallHnd\n");
            ret_val = handler(arg1_ebx, arg2_ecx, arg3_edx, regs); // *** Store the result ***
            serial_write("SD: HndRet\n");
            SYSCALL_DEBUG_PRINTK(" -> Handler for %u returned %d (0x%x)", syscall_num, ret_val, (uint32_t)ret_val);
        } else {
            // Should not happen if table initialized correctly, but handle defensively
            serial_write("SD: ERR NullHnd\n");
             SYSCALL_ERROR("PID %lu: Syscall %u has NULL handler in table!", (unsigned long)current_proc->pid, syscall_num);
            ret_val = ENOSYS;
        }
    } else {
        // Syscall number out of bounds
        serial_write("SD: ERR Bounds\n");
        SYSCALL_ERROR("PID %lu: Invalid syscall number %u (>= MAX_SYSCALLS %d)", (unsigned long)current_proc->pid, syscall_num, MAX_SYSCALLS);
        ret_val = ENOSYS; // Or EINVAL? ENOSYS seems more appropriate.
    }

    // Store the return value back into the EAX field of the stack frame
    // The assembly stub will restore this into EAX before returning to user space.
    serial_write("SD: SetRet\n");
    SYSCALL_DEBUG_PRINTK(" -> C Dispatcher returning %d (0x%x) in EAX for assembly stub.", ret_val, (uint32_t)ret_val);
    regs->eax = (uint32_t)ret_val; // *** Place the actual return value here ***

    serial_write("SD: Exit\n");
} 