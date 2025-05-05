/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations (Corrected Argument Handling)
 *
 * Version 4.2.1: Fixed access_ok checks and debug logs in sys_read/sys_write.
 */

 // --- Includes ---
 #include "syscall.h"
 #include "terminal.h"
 #include "process.h"
 #include "scheduler.h"
 #include "sys_file.h"
 #include "kmalloc.h"
 #include "string.h"
 #include "uaccess.h"    // Essential for pointer validation
 #include "fs_errno.h"
 #include "fs_limits.h"
 #include "vfs.h"
 #include "assert.h"
 #include "debug.h"      // Provides DEBUG_PRINTK
 #include "serial.h"
 #include "paging.h"     // Include for KERNEL_SPACE_VIRT_START
 // Make sure isr_frame.h (or equivalent) defining syscall_regs_t is included
 #include "isr_frame.h" // <<< MAKE SURE THIS IS CORRECTLY INCLUDED >>>

 // Define a max length for puts to prevent unbounded reads
 #define MAX_PUTS_LEN 256 // Example: Limit puts to 256 chars including null
 #define DEBUG_SYSCALL 1 // Keep debug on for now

 #if DEBUG_SYSCALL
 // Use the kernel's primary debug print mechanism for syscall logs
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Syscall] " fmt "\n", ##__VA_ARGS__)
 #else
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif

 // --- Constants ---
 #define STDIN_FILENO  0
 #define STDOUT_FILENO 1
 #define STDERR_FILENO 2
 #define MAX_SYSCALL_STR_LEN MAX_PATH_LEN
 #define MAX_RW_CHUNK_SIZE   PAGE_SIZE
 #ifndef MIN
 #define MIN(a, b) (((a) < (b)) ? (a) : (b))
 #endif
 #ifndef MAX
 #define MAX(a, b) (((a) > (b)) ? (a) : (b))
 #endif

 // --- Static Data ---
 // Assumes syscall_fn_t now matches the new signature:
 // int (*syscall_fn_t)(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *full_frame);
 static syscall_fn_t syscall_table[MAX_SYSCALLS];

 // --- Forward Declarations ---
 // *** MODIFIED SIGNATURES TO MATCH NEW syscall_fn_t ***
 static int sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 static int sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs);
 // Keep sys_not_implemented signature simple, as it doesn't use specific args
 static int sys_not_implemented(syscall_regs_t *regs);

 // Helper function
 static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen);
 typedef int (*syscall_fn_t)(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *full_frame);
 extern void serial_print_hex(uint32_t n);

 //-----------------------------------------------------------------------------
 // Syscall Initialization
 //-----------------------------------------------------------------------------
 void syscall_init(void) {
     serial_write(" FNC_ENTER: syscall_init\n");
     DEBUG_PRINTK("Initializing system call table (max %d syscalls)...\n", MAX_SYSCALLS);
     serial_write("  STEP: Looping to init table\n");
     for (int i = 0; i < MAX_SYSCALLS; i++) {
         // Cast sys_not_implemented if its signature doesn't match syscall_fn_t
         syscall_table[i] = (syscall_fn_t)sys_not_implemented;
     }
     serial_write("  STEP: Registering handlers\n");
     // Cast handlers to the new function pointer type
     syscall_table[SYS_EXIT]  = (syscall_fn_t)sys_exit_impl;
     syscall_table[SYS_READ]  = (syscall_fn_t)sys_read_impl;
     syscall_table[SYS_WRITE] = (syscall_fn_t)sys_write_impl;
     syscall_table[SYS_OPEN]  = (syscall_fn_t)sys_open_impl;
     syscall_table[SYS_CLOSE] = (syscall_fn_t)sys_close_impl;
     syscall_table[SYS_LSEEK] = (syscall_fn_t)sys_lseek_impl;
     syscall_table[SYS_GETPID]= (syscall_fn_t)sys_getpid_impl;
     syscall_table[SYS_PUTS]  = (syscall_fn_t)sys_puts_impl;
     serial_write("  STEP: Verifying SYS_EXIT assignment...\n");
     KERNEL_ASSERT(syscall_table[SYS_EXIT] == (syscall_fn_t)sys_exit_impl, "syscall_table[SYS_EXIT] assignment failed!");
     serial_write("  STEP: SYS_EXIT assignment OK.\n");
     DEBUG_PRINTK("System call table initialization complete.\n");
     serial_write(" FNC_EXIT: syscall_init\n");
 }

 //-----------------------------------------------------------------------------
 // Static Helper: Safe String Copy from User Space
 //-----------------------------------------------------------------------------
 static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
    // (Implementation remains the same as previous version)
     serial_write(" FNC_ENTER: strncpy_from_user_safe\n");
     KERNEL_ASSERT(k_dst != NULL, "Kernel destination buffer cannot be NULL");
     if (maxlen == 0) return -EINVAL;
     k_dst[0] = '\0';

     serial_write("  STEP: Basic u_src check\n");
     serial_write("   DBG: Checking u_src: "); serial_print_hex((uint32_t)u_src);
     serial_write(" against KERNEL_SPACE_VIRT_START: "); serial_print_hex(KERNEL_SPACE_VIRT_START);
     serial_write("\n");
     if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
         SYSCALL_DEBUG_PRINTK("  strncpy: Basic check failed (NULL or kernel addr %p)", u_src);
         serial_write("  RET: -EFAULT (bad u_src basic check)\n");
         return -EFAULT;
     }

     serial_write("  STEP: Calling access_ok\n");
     SYSCALL_DEBUG_PRINTK("  strncpy: Checking access_ok(READ, %p, 1 byte)...", u_src);
     // Check VMA permissions for reading at least one byte initially.
     // The copy_from_user will handle subsequent faults if the string crosses VMA boundaries.
     if (!access_ok(VERIFY_READ, u_src, 1)) {
         SYSCALL_DEBUG_PRINTK("  strncpy: access_ok failed for user buffer %p", u_src);
         serial_write("  RET: -EFAULT (access_ok failed)\n");
         return -EFAULT;
     }
     SYSCALL_DEBUG_PRINTK("  strncpy: access_ok passed for first byte.");

     SYSCALL_DEBUG_PRINTK("strncpy_from_user_safe: Copying from u_src=%p to k_dst=%p (maxlen=%u)", u_src, k_dst, maxlen);
     serial_write("  STEP: Entering copy loop\n");
     size_t len = 0;
     while (len < maxlen) {
         char current_char;
         // Copy one byte at a time, relying on exception handling in copy_from_user
         size_t not_copied = copy_from_user(&current_char, u_src + len, 1);
         if (not_copied > 0) {
             serial_write("   LOOP: Fault detected!\n");
             // Ensure null termination even on fault.
             k_dst[len > 0 ? len -1 : 0] = '\0';
             SYSCALL_DEBUG_PRINTK("  strncpy: Fault copying byte %u from %p. Returning -EFAULT", len, u_src + len);
             serial_write("  RET: -EFAULT (fault during copy)\n");
             return -EFAULT;
         }
         k_dst[len] = current_char;
         if (current_char == '\0') {
             SYSCALL_DEBUG_PRINTK("  strncpy: Found null terminator at length %u. Success.", len);
             serial_write("  RET: 0 (Success)\n");
             return 0; // Success
         }
         len++;
     }
     // If loop finished because maxlen was reached without finding null terminator
     serial_write("  STEP: Loop finished (maxlen reached)\n");
     k_dst[maxlen - 1] = '\0'; // Ensure null termination
     SYSCALL_DEBUG_PRINTK("  strncpy: String from %p exceeded maxlen %u. Returning -ENAMETOOLONG", u_src, maxlen);
     serial_write("  RET: -ENAMETOOLONG\n");
     return -ENAMETOOLONG; // String too long
 }

 //-----------------------------------------------------------------------------
 // Syscall Implementations
 //-----------------------------------------------------------------------------

 // Uses original signature as it doesn't need args beyond regs->eax
 static int sys_not_implemented(syscall_regs_t *regs) {
     serial_write(" FNC_ENTER: sys_not_implemented\n");
     KERNEL_ASSERT(regs != NULL, "NULL regs");
     pcb_t* current_proc = get_current_process();
     KERNEL_ASSERT(current_proc != NULL, "No process context");
     SYSCALL_DEBUG_PRINTK("PID %lu: Called unimplemented syscall %u. Returning -ENOSYS.", current_proc->pid, regs->eax);
     serial_write(" FNC_EXIT: sys_not_implemented (-ENOSYS)\n");
     return -ENOSYS;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_exit_impl\n");
     int exit_code = (int)arg1_ebx; // *** Use explicitly passed EBX value ***
     serial_write("  DBG: ExitCode="); serial_print_hex(exit_code); serial_write("\n");
     pcb_t* current_proc = get_current_process();
     KERNEL_ASSERT(current_proc != NULL, "sys_exit no process");
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_EXIT(exit_code=%d) called.", current_proc->pid, exit_code);
     serial_write("  STEP: Calling remove_current_task...\n");
     remove_current_task_with_code(exit_code);
     KERNEL_PANIC_HALT("FATAL: remove_current_task returned!");
     return 0;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
    (void)regs; // regs might be unused if only explicit args are needed now
    serial_write(" FNC_ENTER: sys_read_impl\n");
    int fd              = (int)arg1_ebx;
    void *user_buf      = (void*)arg2_ecx; // *** Use correct arg from dispatcher ***
    size_t count        = (size_t)arg3_edx; // *** Use correct arg from dispatcher ***
    uint32_t pid        = get_current_process() ? get_current_process()->pid : 0;

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_READ(fd=%d, buf=%p, count=%zu)", pid, fd, user_buf, count);
    if ((ssize_t)count < 0) {
        serial_write("  RET: -EINVAL (negative count)\n");
        return -EINVAL;
    }
    if (count == 0) {
        serial_write("  RET: 0 (zero count)\n");
        return 0;
    }

    // --- Corrected Pre-access_ok check ---
    serial_write("  STEP: Pre-access_ok check for WRITE..."); // Check user buffer writability
    serial_write(" fd="); serial_print_hex(fd);
    serial_write(" buf="); serial_print_hex((uintptr_t)user_buf); // *** Log actual user_buf ***
    serial_write(" count="); serial_print_hex(count); serial_write("\n"); // *** Log actual count ***

    // Check if the user buffer is WRITABLE (since kernel writes to it)
    // *** Pass the actual user_buf and count to access_ok ***
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)", user_buf);
        serial_write("  RET: -EFAULT (access_ok failed)\n"); // Log the failure reason
        return -EFAULT; // Return EFAULT as per POSIX for bad address
    }
    serial_write("  STEP: access_ok passed.\n"); // Log success

    // Allocate kernel buffer for chunking
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    char* kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        serial_write("  RET: -ENOMEM (kmalloc failed)\n");
        return -ENOMEM;
    }
    SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p (size %zu).", kbuf, chunk_alloc_size);

    ssize_t total_read = 0;
    int final_ret_val = 0;

    serial_write("  STEP: Entering read loop\n");
    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in sys_read");
        SYSCALL_DEBUG_PRINTK("  Loop: Reading chunk size %zu (total_read %zd)", current_chunk_size, total_read);

        // Call the underlying sys_file read implementation
        ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
        SYSCALL_DEBUG_PRINTK("   LOOP_READ: sys_read returned %zd", bytes_read_this_chunk);

        if (bytes_read_this_chunk < 0) { // Error from sys_file layer?
            final_ret_val = (total_read > 0) ? total_read : bytes_read_this_chunk; // Return bytes read so far or the error
            serial_write("  LOOP: Error from sys_read\n");
            goto sys_read_cleanup;
        }
        if (bytes_read_this_chunk == 0) { // EOF reached?
             serial_write("  LOOP: EOF reached\n");
            break; // Exit loop, return bytes read so far
        }

        // Copy the data read into the kernel buffer to the user buffer
        size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, bytes_read_this_chunk);
        SYSCALL_DEBUG_PRINTK("   LOOP_READ: copy_to_user returned %zu (not copied)", not_copied);
        size_t copied_back_this_chunk = bytes_read_this_chunk - not_copied;
        total_read += copied_back_this_chunk;

        if (not_copied > 0) { // Fault during copy back to user?
            final_ret_val = total_read; // Return bytes successfully copied back
            serial_write("  LOOP: Fault during copy_to_user\n");
            goto sys_read_cleanup;
        }
        // If we read less than requested in the chunk, it implies EOF or error on next read
        if ((size_t)bytes_read_this_chunk < current_chunk_size) {
            serial_write("  LOOP: Short read, breaking loop\n");
            break;
        }
    } // End while loop

    final_ret_val = total_read; // Success, return total bytes read

sys_read_cleanup:
    if (kbuf) kfree(kbuf);
    SYSCALL_DEBUG_PRINTK("  SYS_READ returning %d.", final_ret_val);
    serial_write(" FNC_EXIT: sys_read_impl\n");
    return final_ret_val;
}

 // *** USES NEW SIGNATURE ***
 static int sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
    (void)regs;
    serial_write(" FNC_ENTER: sys_write_impl\n");
    int fd                = (int)arg1_ebx;
    const void *user_buf  = (const void*)arg2_ecx; // *** Use correct arg from dispatcher ***
    size_t count          = (size_t)arg3_edx;     // *** Use correct arg from dispatcher ***
    uint32_t pid          = get_current_process() ? get_current_process()->pid : 0;

    SYSCALL_DEBUG_PRINTK("PID %lu: SYS_WRITE(fd=%d, buf=%p, count=%zu)", pid, fd, user_buf, count);
    if ((ssize_t)count < 0) {
        serial_write("  RET: -EINVAL (negative count)\n");
        return -EINVAL;
    }
    if (count == 0) {
        serial_write("  RET: 0 (zero count)\n");
        return 0;
    }

    // --- Corrected Pre-access_ok check ---
    serial_write("  STEP: Pre-access_ok check for READ..."); // Check user buffer readability
    serial_write(" fd="); serial_print_hex(fd);
    serial_write(" buf="); serial_print_hex((uintptr_t)user_buf); // *** Log actual user_buf ***
    serial_write(" count="); serial_print_hex(count); serial_write("\n"); // *** Log actual count ***

    // Check if the user buffer is READABLE (since kernel reads from it)
    // *** Pass the actual user_buf and count to access_ok ***
    if (!access_ok(VERIFY_READ, user_buf, count)) {
        SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)", user_buf);
        serial_write("  RET: -EFAULT (access_ok failed)\n"); // Log the failure reason
        return -EFAULT; // Return EFAULT as per POSIX for bad address
    }
     serial_write("  STEP: access_ok passed.\n"); // Log success

    // Allocate kernel buffer for chunking
    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    char* kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        serial_write("  RET: -ENOMEM (kmalloc failed)\n");
        return -ENOMEM;
    }
    SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p (size %zu).", kbuf, chunk_alloc_size);

    ssize_t total_written = 0;
    int final_ret_val = 0;

    serial_write("  STEP: Entering write loop\n");
    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in sys_write");
        SYSCALL_DEBUG_PRINTK("  Loop: Writing chunk size %zu (total_written %zd)", current_chunk_size, total_written);

        // Copy data from user buffer to kernel buffer
        size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
        SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: copy_from_user returned %zu (not copied)", not_copied);
        size_t copied_from_user_this_chunk = current_chunk_size - not_copied;

        // Only proceed to write if data was successfully copied from user
        if (copied_from_user_this_chunk > 0) {
            ssize_t bytes_written_this_chunk = 0;
            // Handle console/terminal output directly for efficiency
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_from_user_this_chunk);
                bytes_written_this_chunk = copied_from_user_this_chunk; // Assume terminal write succeeds fully
                SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: terminal_write_bytes returned (assumed %zd)", bytes_written_this_chunk);
            } else { // Handle file writes via sys_file layer
                bytes_written_this_chunk = sys_write(fd, kbuf, copied_from_user_this_chunk);
                SYSCALL_DEBUG_PRINTK("   LOOP_WRITE: sys_write returned %zd", bytes_written_this_chunk);
            }

            if (bytes_written_this_chunk < 0) { // Error from sys_file or terminal layer?
                final_ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk; // Return bytes written so far or the error
                 serial_write("  LOOP: Error during write operation\n");
                goto sys_write_cleanup;
            }
            total_written += bytes_written_this_chunk; // Accumulate total bytes written

            // If the underlying write wrote fewer bytes than copied from user (e.g., disk full), stop.
            if ((size_t)bytes_written_this_chunk < copied_from_user_this_chunk) {
                 serial_write("  LOOP: Short write, breaking loop\n");
                break;
            }
        }

        // If copy_from_user failed (not_copied > 0), return bytes written so far or EFAULT
        if (not_copied > 0) {
            final_ret_val = (total_written > 0) ? total_written : -EFAULT;
             serial_write("  LOOP: Fault during copy_from_user\n");
            goto sys_write_cleanup;
        }
    } // End while loop

    final_ret_val = total_written; // Success, return total bytes written

sys_write_cleanup:
    if (kbuf) kfree(kbuf);
    SYSCALL_DEBUG_PRINTK("  SYS_WRITE returning %d.", final_ret_val);
    serial_write(" FNC_EXIT: sys_write_impl\n");
    return final_ret_val;
}


 // *** USES NEW SIGNATURE ***
 static int sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_open_impl\n");
     const char *user_pathname = (const char*)arg1_ebx; // *** Use explicitly passed EBX ***
     int flags                 = (int)arg2_ecx;        // *** Use explicitly passed ECX ***
     int mode                  = (int)arg3_edx;        // *** Use explicitly passed EDX ***
     uint32_t pid              = get_current_process() ? get_current_process()->pid : 0;

     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_OPEN(path_user=%p, flags=0x%x, mode=0%o)", pid, user_pathname, flags, mode);
     char k_pathname[MAX_SYSCALL_STR_LEN];
     serial_write("  STEP: Calling strncpy_from_user_safe\n");
     int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname)); // Uses user_pathname (arg1_ebx)
     SYSCALL_DEBUG_PRINTK("  STEP: strncpy_from_user_safe returned %d", copy_err);
     if (copy_err != 0) {
         SYSCALL_DEBUG_PRINTK(" -> Error %d copying path from user %p", copy_err, user_pathname);
         serial_write("  RET: Error from strncpy\n");
         return copy_err;
     }
     SYSCALL_DEBUG_PRINTK("  Copied path to kernel: '%s'", k_pathname);

     serial_write("  STEP: Calling sys_open (underlying)\n");
     int fd = sys_open(k_pathname, flags, mode); // Call implementation in sys_file.c
     SYSCALL_DEBUG_PRINTK("  STEP: sys_open returned %d", fd);
     serial_write(" FNC_EXIT: sys_open_impl\n");
     return fd;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_close_impl\n");
     int fd = (int)arg1_ebx; // *** Use explicitly passed EBX ***
     uint32_t pid = get_current_process() ? get_current_process()->pid : 0;
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_CLOSE(fd=%d)", pid, fd);
     serial_write("  STEP: Calling sys_close (underlying)\n");
     int ret = sys_close(fd); // Call implementation in sys_file.c
     SYSCALL_DEBUG_PRINTK("  STEP: sys_close returned %d", ret);
     serial_write(" FNC_EXIT: sys_close_impl\n");
     return ret;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_lseek_impl\n");
     int fd        = (int)arg1_ebx;   // *** Use explicitly passed EBX ***
     off_t offset  = (off_t)arg2_ecx; // *** Use explicitly passed ECX ***
     int whence    = (int)arg3_edx;   // *** Use explicitly passed EDX ***
     uint32_t pid  = get_current_process() ? get_current_process()->pid : 0;
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_LSEEK(fd=%d, offset=%ld, whence=%d)", pid, fd, (long)offset, whence);
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -EINVAL;
     serial_write("  STEP: Calling sys_lseek (underlying)\n");
     off_t result_offset = sys_lseek(fd, offset, whence); // Call implementation in sys_file.c
     SYSCALL_DEBUG_PRINTK("  STEP: sys_lseek returned %ld", (long)result_offset);
     serial_write(" FNC_EXIT: sys_lseek_impl\n");
     return (int)result_offset;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)arg1_ebx; (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_getpid_impl\n");
     pcb_t* current_proc = get_current_process();
     KERNEL_ASSERT(current_proc != NULL, "sys_getpid no process");
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_GETPID() -> Returning PID %lu", current_proc->pid, current_proc->pid);
     serial_write(" FNC_EXIT: sys_getpid_impl\n");
     return (int)current_proc->pid;
 }

 // *** USES NEW SIGNATURE ***
 static int sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, syscall_regs_t *regs) {
     (void)arg2_ecx; (void)arg3_edx; (void)regs; // Mark unused
     serial_write(" FNC_ENTER: sys_puts_impl\n");
     const char *user_str_ptr = (const char *)arg1_ebx; // *** Use explicitly passed EBX ***
     uint32_t pid = get_current_process() ? get_current_process()->pid : 0;
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_PUTS(user_str=%p)", pid, user_str_ptr);
     char kbuffer[MAX_PUTS_LEN];
     SYSCALL_DEBUG_PRINTK("  Calling strncpy_from_user_safe(u_src=%p, k_dst=%p, maxlen=%u)...",
                          user_str_ptr, kbuffer, sizeof(kbuffer));
     int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer)); // Uses user_str_ptr (arg1_ebx)
     SYSCALL_DEBUG_PRINTK("  strncpy_from_user_safe returned %d.", copy_err);
     if (copy_err != 0) {
         serial_write(" [WARN syscall] sys_puts_impl: Invalid user pointer (strncpy failed code=");
         serial_print_hex((uint32_t)copy_err); serial_write(")\n");
         return copy_err; // Returns -EFAULT if NULL or bad addr, or -ENAMETOOLONG
     }
     SYSCALL_DEBUG_PRINTK("  String copied to kernel buffer: '%.*s'", (int)sizeof(kbuffer)-1, kbuffer);
     SYSCALL_DEBUG_PRINTK("  Calling terminal_write...");
     terminal_write(kbuffer);
     terminal_write("\n");
     SYSCALL_DEBUG_PRINTK("  terminal_write finished.");
     SYSCALL_DEBUG_PRINTK(" -> SYS_PUTS finished successfully.");
     serial_write(" FNC_EXIT: sys_puts_impl (OK)\n");
     return 0;
 }


 //-----------------------------------------------------------------------------
 // Main Syscall Dispatcher (Called by Assembly)
 //-----------------------------------------------------------------------------
 // Assembly passes: regs*, syscall_num (orig EAX), first_arg (orig EBX)
 void syscall_dispatcher(syscall_regs_t *regs, uint32_t syscall_num, uint32_t first_arg_ebx) {
     serial_write("SD: Enter\n");
     KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");
     SYSCALL_DEBUG_PRINTK("Dispatcher entered. Frame at %p.", regs);
     serial_write("SD: SysNum(Arg2)="); serial_print_hex(syscall_num); serial_write("\n");
     serial_write("SD: FirstArg(Arg3)="); serial_print_hex(first_arg_ebx); serial_write("\n");
     // Log register values saved by pusha for comparison/debugging
     serial_write("SD: Frame Check: regs->eax="); serial_print_hex(regs->eax); serial_write("\n");
     serial_write("SD: Frame Check: regs->ebx="); serial_print_hex(regs->ebx); serial_write("\n"); // Should match first_arg_ebx
     serial_write("SD: Frame Check: regs->ecx="); serial_print_hex(regs->ecx); serial_write("\n"); // Should be arg2
     serial_write("SD: Frame Check: regs->edx="); serial_print_hex(regs->edx); serial_write("\n"); // Should be arg3

     // Verify syscall number consistency
     if (syscall_num != regs->eax) {
         serial_write("!!! SYSCALL DISPATCHER WARNING: Mismatch between syscall_num arg and regs->eax !!!\n");
         syscall_num = regs->eax; // Trust the register frame saved by CPU/pusha
     }

     SYSCALL_DEBUG_PRINTK(" -> Processing syscall number: %u (0x%x)", syscall_num, syscall_num);

     serial_write("SD: GetProc\n");
     pcb_t* current_proc = get_current_process();
     serial_write("SD: ChkProc\n");
     if (!current_proc) KERNEL_PANIC_HALT("Syscall executed without process context!");

     int ret_val; // Return value from handler

     serial_write("SD: ChkBounds\n");
     if (syscall_num < MAX_SYSCALLS) {
         serial_write("SD: InBounds\n");
         serial_write("SD: LookupHnd\n");
         syscall_fn_t handler = syscall_table[syscall_num]; // Get handler using the potentially corrected syscall_num
         serial_write("SD: ChkHnd (New Logic)\n");
         if (handler) {
             // Check if it's the sys_not_implemented handler (which uses old signature)
             if (handler == (syscall_fn_t)sys_not_implemented) {
                 serial_write("SD: CallNI (New Logic)\n");
                 ret_val = sys_not_implemented(regs); // Call NI handler with original signature
                 serial_write("SD: NIRet (New Logic)\n");
             } else {
                 // Call the actual handler with the new signature, passing args explicitly
                 serial_write("SD: CallHnd (New Logic)\n");
                 // *** Pass arguments explicitly ***
                 // Assumes handlers now expect: (orig_ebx, orig_ecx, orig_edx, full_frame*)
                 // We get orig_ebx from first_arg_ebx (passed from asm)
                 // We get orig_ecx, orig_edx from the regs struct saved by pusha
                 ret_val = handler(first_arg_ebx, regs->ecx, regs->edx, regs);
                 serial_write("SD: HndRet (New Logic)\n");
             }
             SYSCALL_DEBUG_PRINTK(" -> Handler for %u returned %d (0x%x)", syscall_num, ret_val, (uint32_t)ret_val);
         } else { // NULL handler in table (shouldn't happen)
             serial_write("SD: ERR NullHnd (New Logic)\n");
             ret_val = -ENOSYS;
         }
     } else { // Syscall number out of bounds
         serial_write("SD: ERR Bounds\n");
         ret_val = -ENOSYS;
     }
     serial_write("SD: SetRet\n");
     SYSCALL_DEBUG_PRINTK(" -> C Dispatcher returning %d (0x%x) in EAX for assembly stub.", ret_val, (uint32_t)ret_val);

     // *** CRUCIAL FIX: Place return value into the frame where ASM stub expects it ***
     regs->eax = (uint32_t)ret_val;

     serial_write("SD: Exit\n");
 }