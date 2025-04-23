/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations
 *
 * Handles the dispatching of system calls invoked via the `int $0x80` instruction
 * and provides implementations for basic file operations and process exit.
 */

 #include "syscall.h"
 #include "terminal.h"    // For terminal_write_bytes, DEBUG_PRINTK via debug.h
 #include "process.h"     // For get_current_process(), pcb_t
 #include "scheduler.h"   // For remove_current_task_with_code()
 #include "sys_file.h"    // For underlying file operations (sys_open etc.)
 #include "kmalloc.h"     // For kmalloc, kfree
 #include "string.h"      // For memcpy/memset
 #include "uaccess.h"     // For access_ok, copy_from_user, copy_to_user, strncpy_from_user_safe
 #include "fs_errno.h"    // For error codes (EFAULT, ENOSYS, EBADF, EINVAL, ENOMEM, etc.)
 #include "fs_limits.h"   // For MAX_PATH_LEN, MAX_FD
 #include "vfs.h"         // For SEEK_SET, SEEK_CUR, SEEK_END definitions (needed by sys_file.h)
 #include "assert.h"      // For KERNEL_PANIC_HALT, KERNEL_ASSERT
 #include "debug.h"       // For DEBUG_PRINTK
 
 // --- Debug Configuration ---
 #define DEBUG_SYSCALL 0 // Set to 1 to enable syscall debug messages
 
 #if DEBUG_SYSCALL
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Syscall] " fmt, ##__VA_ARGS__)
 #else
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 // --- Constants ---
 // Standard file descriptors
 #define STDIN_FILENO  0
 #define STDOUT_FILENO 1
 #define STDERR_FILENO 2
 
 // Limits for kernel buffers used in syscalls
 #define MAX_SYSCALL_STR_LEN MAX_PATH_LEN // Limit for pathnames copied from user
 #define MAX_RW_CHUNK_SIZE   PAGE_SIZE    // Max bytes to copy in one kernel buffer (typically PAGE_SIZE=4096)
 
 // Ensure MIN macro is defined
 #ifndef MIN
 #define MIN(a, b) (((a) < (b)) ? (a) : (b))
 #endif
 
 // --- Static Data ---
 
 /** @brief The system call dispatch table. Indexed by syscall number. */
 static syscall_fn_t syscall_table[MAX_SYSCALLS];
 
 // --- Forward Declarations ---
 
 // Syscall implementations
 static int sys_exit_impl(syscall_regs_t *regs);
 static int sys_read_impl(syscall_regs_t *regs);
 static int sys_write_impl(syscall_regs_t *regs);
 static int sys_open_impl(syscall_regs_t *regs);
 static int sys_close_impl(syscall_regs_t *regs);
 static int sys_lseek_impl(syscall_regs_t *regs);
 static int sys_not_implemented(syscall_regs_t *regs);
 // Placeholder for future syscalls
 static int sys_getpid_impl(syscall_regs_t *regs);
 // Add declarations for other syscalls (fork, execve, brk etc.) as needed
 
 // Helper functions
 static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen);
 
 //-----------------------------------------------------------------------------
 // Syscall Initialization
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Initializes the system call dispatch table.
  * All entries are initially set to `sys_not_implemented`.
  * Specific handlers are then registered for implemented syscalls.
  */
 void syscall_init(void) {
     DEBUG_PRINTK("Initializing system call table (max %d syscalls)...\n", MAX_SYSCALLS);
 
     // Initialize all entries to point to the 'not implemented' function
     for (int i = 0; i < MAX_SYSCALLS; i++) {
         syscall_table[i] = sys_not_implemented;
     }
 
     // Register implemented system calls
     syscall_table[SYS_EXIT]  = sys_exit_impl;
     syscall_table[SYS_READ]  = sys_read_impl;
     syscall_table[SYS_WRITE] = sys_write_impl;
     syscall_table[SYS_OPEN]  = sys_open_impl;
     syscall_table[SYS_CLOSE] = sys_close_impl;
     syscall_table[SYS_LSEEK] = sys_lseek_impl;
 
     // Register placeholders for common syscalls
     syscall_table[SYS_GETPID] = sys_getpid_impl; // Example placeholder
     // syscall_table[SYS_FORK]   = sys_fork_impl; // Requires significant process/memory management additions
     // syscall_table[SYS_EXECVE] = sys_execve_impl; // Requires ELF loading, argument handling etc.
     // syscall_table[SYS_BRK]    = sys_brk_impl; // Requires VMA manipulation
 
     DEBUG_PRINTK("System call table initialization complete.\n");
 }
 
 //-----------------------------------------------------------------------------
 // Static Helper: Safe String Copy from User Space
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Copies a null-terminated string from user space safely.
  * Uses copy_from_user internally byte-by-byte to handle page faults gracefully.
  *
  * @param u_src User space virtual address of the string. Must be accessible.
  * @param k_dst Kernel space buffer to copy the string into. Must be valid kernel memory.
  * @param maxlen Maximum number of bytes to copy into k_dst (including null terminator).
  * The resulting kernel buffer is always null-terminated if maxlen > 0.
  * @return 0 on success.
  * @return -ENAMETOOLONG if the string length (excluding null terminator) is >= maxlen.
  * @return -EFAULT if a page fault occurs while accessing user memory `u_src`.
  * @return -EINVAL if maxlen is 0.
  */
 static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
     if (maxlen == 0) {
         return -EINVAL;
     }
     // Basic address check before proceeding (avoids obvious kernel faults)
     // Note: access_ok provides more detailed VMA checks, but copy_from_user handles faults.
     if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
          return -EFAULT; // Basic check for obviously bad user pointer
     }
 
     KERNEL_ASSERT(k_dst != NULL, "Kernel destination buffer cannot be NULL");
 
     size_t len = 0;
     while (len < maxlen) {
         char current_char;
         // Try to copy one byte
         size_t not_copied = copy_from_user(&current_char, u_src + len, 1);
 
         if (not_copied > 0) {
             // Fault occurred accessing user memory. Null-terminate kernel buffer safely.
             k_dst[len > 0 ? len -1 : 0] = '\0'; // Terminate at last potentially valid point
             SYSCALL_DEBUG_PRINTK("strncpy_from_user: Fault copying byte %u from %p\n", len, u_src + len);
             return -EFAULT;
         }
 
         // Store the copied byte
         k_dst[len] = current_char;
 
         // Check for null terminator
         if (current_char == '\0') {
             return 0; // Success: Found null terminator within maxlen.
         }
 
         len++; // Advance to next character
     }
 
     // Reached maxlen without finding null terminator.
     k_dst[maxlen - 1] = '\0'; // Ensure null termination.
     SYSCALL_DEBUG_PRINTK("strncpy_from_user: String from %p exceeded maxlen %u\n", u_src, maxlen);
     return -ENAMETOOLONG;
 }
 
 
 //-----------------------------------------------------------------------------
 // Syscall Implementations
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Handles unimplemented system calls.
  * @param regs Pointer to saved user registers (EAX contains the invalid syscall number).
  * @return -ENOSYS always.
  */
 static int sys_not_implemented(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_not_implemented");
     pcb_t* current_proc = get_current_process(); // Can be NULL early? No, checked in dispatcher.
     uint32_t pid = current_proc->pid;
     uint32_t syscall_num = regs->eax; // Invalid syscall number
 
     SYSCALL_DEBUG_PRINTK("PID %lu: Called unimplemented syscall %u.\n", pid, syscall_num);
     return -ENOSYS; // Function not implemented
 }
 
 /**
  * @brief Implements the exit() system call (SYS_EXIT).
  * Terminates the current process with the given exit code. This function does not return.
  *
  * @param regs Pointer to saved user registers. `regs->ebx` contains the exit code.
  * @return Does not return. If it somehow did, the kernel panics.
  */
 static int sys_exit_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_exit_impl");
 
     // Extract exit code from EBX (Argument 0)
     int exit_code = (int)regs->ebx; // Cast to signed int
 
     pcb_t* current_proc = get_current_process();
     KERNEL_ASSERT(current_proc != NULL, "sys_exit called without process context!");
     uint32_t pid = current_proc->pid;
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_EXIT called with exit_code %d.\n", pid, exit_code);
 
     // Call the scheduler function to terminate the task.
     // This function handles state changes, cleanup (eventually), and scheduling the next task.
     // It MUST NOT return to this context.
     remove_current_task_with_code(exit_code);
 
     // If remove_current_task_with_code returns, something is catastrophically wrong.
     KERNEL_PANIC_HALT("FATAL: remove_current_task_with_code returned in sys_exit!");
     return 0; // Unreachable code
 }
 
 /**
  * @brief Implements the read() system call (SYS_READ).
  * Reads data from a file descriptor into a user buffer.
  *
  * @param regs Pointer to saved user registers.
  * `regs->ebx` = file descriptor (int fd)
  * `regs->ecx` = user buffer pointer (void *user_buf)
  * `regs->edx` = number of bytes to read (size_t count)
  * @return Number of bytes successfully read and copied to user space (>= 0).
  * @return Negative errno on failure (e.g., -EBADF, -EFAULT, -ENOMEM, -EINVAL).
  */
 static int sys_read_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_read_impl");
 
     // Extract arguments
     int fd              = (int)regs->ebx;
     void *user_buf      = (void*)regs->ecx;
     size_t count        = (size_t)regs->edx;
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_READ(fd=%d, buf=%p, count=%u)\n",
                          get_current_process()->pid, fd, user_buf, count);
 
     // --- Argument Validation ---
     if ((ssize_t)count < 0) { // Check if count interpreted as signed is negative
         SYSCALL_DEBUG_PRINTK(" -> EINVAL (negative count)\n");
         return -EINVAL;
     }
     if (count == 0) {
         SYSCALL_DEBUG_PRINTK(" -> OK (count is 0)\n");
         return 0; // Reading 0 bytes is a no-op, success.
     }
     // Check user buffer writability *before* allocation/reading.
     // Checks VMA permissions and basic address validity.
     if (!access_ok(VERIFY_WRITE, user_buf, count)) {
         SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)\n", user_buf);
         return -EFAULT;
     }
 
     // Allocate kernel buffer for chunking reads
     // Avoids holding large stack buffers and simplifies fault handling.
     size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
     char* kbuf = kmalloc(chunk_alloc_size);
     if (!kbuf) {
         SYSCALL_DEBUG_PRINTK(" -> ENOMEM (kmalloc failed for kernel buffer)\n");
         return -ENOMEM;
     }
 
     ssize_t total_read = 0;
     int final_ret_val = 0; // Used to store return value in case of errors during loop
 
     // Loop to read data in chunks
     while (total_read < (ssize_t)count) {
         size_t current_chunk_size = MIN(chunk_alloc_size, count - total_read);
         KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in read loop");
 
         // Call the underlying file operation function (e.g., from sys_file.c)
         // This function handles FD validation within the process context.
         ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
 
         SYSCALL_DEBUG_PRINTK("   sys_read(%d, kbuf, %u) returned %d\n", fd, current_chunk_size, bytes_read_this_chunk);
 
         if (bytes_read_this_chunk < 0) {
             // Error from underlying sys_read (e.g., -EBADF, -EIO)
             // Return bytes successfully read so far, or the error if nothing was read yet.
             final_ret_val = (total_read > 0) ? total_read : bytes_read_this_chunk;
             SYSCALL_DEBUG_PRINTK(" -> Error %d from sys_read. Returning %d.\n", bytes_read_this_chunk, final_ret_val);
             goto sys_read_cleanup_and_exit;
         }
 
         if (bytes_read_this_chunk == 0) {
             // End Of File reached
             SYSCALL_DEBUG_PRINTK(" -> EOF reached.\n");
             break; // Exit loop, return total_read so far
         }
 
         // Copy the chunk read into kernel buffer back to user space
         // copy_to_user handles potential page faults during the write to user_buf.
         size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, bytes_read_this_chunk);
 
         if (not_copied > 0) {
             // Fault writing back to user space
             size_t copied_back_this_chunk = bytes_read_this_chunk - not_copied;
             total_read += copied_back_this_chunk;
             SYSCALL_DEBUG_PRINTK(" -> EFAULT copying back to user %p after reading %d bytes (total read: %d)\n",
                                  (char*)user_buf + total_read - copied_back_this_chunk, bytes_read_this_chunk, total_read);
             // Return bytes successfully read & copied back, or EFAULT if none were copied this time.
             final_ret_val = (total_read > 0) ? total_read : -EFAULT;
             goto sys_read_cleanup_and_exit;
         }
 
         // Successfully read and copied back this chunk
         total_read += bytes_read_this_chunk;
 
         // If sys_read returned fewer bytes than requested, it implies EOF or device limit, stop reading.
         if ((size_t)bytes_read_this_chunk < current_chunk_size) {
             SYSCALL_DEBUG_PRINTK(" -> Short read (%d < %u), assuming EOF/limit.\n", bytes_read_this_chunk, current_chunk_size);
             break;
         }
     } // end while
 
     // Success case: return total bytes read and copied
     final_ret_val = total_read;
     SYSCALL_DEBUG_PRINTK(" -> OK (Total bytes read: %d)\n", final_ret_val);
 
 sys_read_cleanup_and_exit:
     if (kbuf) {
         kfree(kbuf);
     }
     return final_ret_val;
 }
 
 
 /**
  * @brief Implements the write() system call (SYS_WRITE).
  * Writes data from a user buffer to a file descriptor.
  *
  * @param regs Pointer to saved user registers.
  * `regs->ebx` = file descriptor (int fd)
  * `regs->ecx` = user buffer pointer (const void *user_buf)
  * `regs->edx` = number of bytes to write (size_t count)
  * @return Number of bytes successfully written (>= 0). Can be less than count.
  * @return Negative errno on failure (e.g., -EBADF, -EFAULT, -ENOMEM, -EINVAL).
  */
 static int sys_write_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_write_impl");
 
     // Extract arguments
     int fd                = (int)regs->ebx;
     const void *user_buf  = (const void*)regs->ecx;
     size_t count          = (size_t)regs->edx;
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_WRITE(fd=%d, buf=%p, count=%u)\n",
                          get_current_process()->pid, fd, user_buf, count);
 
     // --- Argument Validation ---
     if ((ssize_t)count < 0) {
         SYSCALL_DEBUG_PRINTK(" -> EINVAL (negative count)\n");
         return -EINVAL;
     }
     if (count == 0) {
          SYSCALL_DEBUG_PRINTK(" -> OK (count is 0)\n");
         return 0;
     }
     // Check user buffer readability.
     if (!access_ok(VERIFY_READ, user_buf, count)) {
         SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)\n", user_buf);
         return -EFAULT;
     }
 
     // Allocate kernel buffer for chunking writes
     size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
     char* kbuf = kmalloc(chunk_alloc_size);
     if (!kbuf) {
         SYSCALL_DEBUG_PRINTK(" -> ENOMEM (kmalloc failed for kernel buffer)\n");
         return -ENOMEM;
     }
 
     ssize_t total_written = 0;
     int final_ret_val = 0;
 
     // Special case: Directly write to console FDs (STDOUT/STDERR) using terminal_write_bytes
     // This bypasses the VFS layer for console output efficiency.
     if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
         SYSCALL_DEBUG_PRINTK("   Handling write to STDOUT/STDERR (fd=%d)\n", fd);
         while(total_written < (ssize_t)count) {
             size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
             KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in console write loop");
 
             // Copy chunk from user space to kernel buffer
             size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
             size_t copied_this_chunk = current_chunk_size - not_copied;
 
             // Write the copied chunk to the terminal
             if (copied_this_chunk > 0) {
                 // TODO: terminal_write_bytes might block if the terminal buffer is full.
                 // A more advanced implementation might handle this non-blockingly or buffer.
                 terminal_write_bytes(kbuf, copied_this_chunk); // Assume this handles underlying device write
                 total_written += copied_this_chunk;
             }
 
             // Check if copy_from_user faulted
             if (not_copied > 0) {
                 SYSCALL_DEBUG_PRINTK(" -> EFAULT copying from user %p during console write (total written: %d)\n",
                                      (char*)user_buf + total_written, total_written);
                 // Return bytes successfully written so far, or EFAULT if none were copied this time.
                 final_ret_val = (total_written > 0) ? total_written : -EFAULT;
                 goto sys_write_cleanup_and_exit;
             }
             // If we copied less than a full chunk but had no error, we must have finished.
             if (copied_this_chunk < current_chunk_size) {
                  break;
             }
         }
         final_ret_val = total_written; // Success for console write
     }
     // Handle writing to actual files via the underlying sys_write (VFS)
     else {
          SYSCALL_DEBUG_PRINTK("   Handling write to file fd=%d\n", fd);
          while(total_written < (ssize_t)count) {
             size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
             KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in file write loop");
 
             // Copy chunk from user space to kernel buffer
             size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
             size_t copied_this_chunk = current_chunk_size - not_copied;
 
             // Write the copied chunk using the file system layer function
             if (copied_this_chunk > 0) {
                 ssize_t bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk);
                 SYSCALL_DEBUG_PRINTK("   sys_write(%d, kbuf, %u) returned %d\n", fd, copied_this_chunk, bytes_written_this_chunk);
 
                 if (bytes_written_this_chunk < 0) {
                     // Error from underlying sys_write (e.g., -EBADF, -ENOSPC, -EIO)
                     final_ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk;
                      SYSCALL_DEBUG_PRINTK(" -> Error %d from sys_write. Returning %d.\n", bytes_written_this_chunk, final_ret_val);
                     goto sys_write_cleanup_and_exit;
                 }
                 // Accumulate bytes successfully written by the underlying layer
                 total_written += bytes_written_this_chunk;
 
                 // If the FS wrote fewer bytes than we gave it, it might be full or hit a limit. Stop.
                 if ((size_t)bytes_written_this_chunk < copied_this_chunk) {
                      SYSCALL_DEBUG_PRINTK(" -> Short write (%d < %u), assuming FS limit/error.\n", bytes_written_this_chunk, copied_this_chunk);
                      break;
                 }
             }
 
             // Check if copy_from_user faulted
             if (not_copied > 0) {
                  SYSCALL_DEBUG_PRINTK(" -> EFAULT copying from user %p during file write (total written: %d)\n",
                                      (char*)user_buf + total_written, total_written);
                 final_ret_val = (total_written > 0) ? total_written : -EFAULT;
                 goto sys_write_cleanup_and_exit;
             }
             // If we copied less than a full chunk but had no error, we must have finished.
              if (copied_this_chunk < current_chunk_size) {
                  break;
              }
         }
         final_ret_val = total_written; // Success for file write
     }
 
     SYSCALL_DEBUG_PRINTK(" -> OK (Total bytes written: %d)\n", final_ret_val);
 
 sys_write_cleanup_and_exit:
     if (kbuf) {
         kfree(kbuf);
     }
     return final_ret_val;
 }
 
 
 /**
  * @brief Implements the open() system call (SYS_OPEN).
  * Opens or creates a file and returns a file descriptor.
  *
  * @param regs Pointer to saved user registers.
  * `regs->ebx` = user pathname pointer (const char *user_pathname)
  * `regs->ecx` = flags (int flags)
  * `regs->edx` = mode (int mode) - used only if O_CREAT is set.
  * @return File descriptor (int fd >= 0) on success.
  * @return Negative errno on failure (e.g., -EFAULT, -ENAMETOOLONG, -ENOENT, -ENOMEM).
  */
 static int sys_open_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_open_impl");
 
     // Extract arguments
     const char *user_pathname = (const char*)regs->ebx;
     int flags                 = (int)regs->ecx;
     int mode                  = (int)regs->edx; // Mode is relevant only if O_CREAT is specified
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_OPEN(path=%p, flags=0x%x, mode=0%o)\n",
                          get_current_process()->pid, user_pathname, flags, mode);
 
     // Allocate kernel buffer for path. Stack allocation is usually fine if MAX_SYSCALL_STR_LEN is reasonable.
     char k_pathname[MAX_SYSCALL_STR_LEN];
 
     // Safely copy pathname from user space using the helper function.
     // This handles potential faults and length limits.
     int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, MAX_SYSCALL_STR_LEN);
     if (copy_err != 0) {
          SYSCALL_DEBUG_PRINTK(" -> Error %d copying path from user %p\n", copy_err, user_pathname);
         // Return the specific error from strncpy_from_user (-EFAULT or -ENAMETOOLONG)
         return copy_err;
     }
     SYSCALL_DEBUG_PRINTK("   Copied path: '%s'\n", k_pathname);
 
     // Call the underlying sys_open function (e.g., in sys_file.c).
     // This function interacts with the VFS to open/create the file and allocates
     // a file descriptor within the current process's context.
     int fd = sys_open(k_pathname, flags, mode);
 
     // sys_open returns fd (>= 0) or negative errno on failure
     SYSCALL_DEBUG_PRINTK("   sys_open returned fd = %d\n", fd);
     return fd;
 }
 
 /**
  * @brief Implements the close() system call (SYS_CLOSE).
  * Closes an open file descriptor.
  *
  * @param regs Pointer to saved user registers.
  * `regs->ebx` = file descriptor (int fd)
  * @return 0 on success.
  * @return Negative errno on failure (e.g., -EBADF).
  */
 static int sys_close_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_close_impl");
 
     // Extract file descriptor argument
     int fd = (int)regs->ebx;
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_CLOSE(fd=%d)\n", get_current_process()->pid, fd);
 
     // Call the underlying sys_close function (e.g., in sys_file.c).
     // This handles VFS close operations, freeing the sys_file structure,
     // and marking the FD slot as available in the process's FD table.
     int ret = sys_close(fd); // Returns 0 or negative errno
 
     SYSCALL_DEBUG_PRINTK("   sys_close returned %d\n", ret);
     return ret;
 }
 
 /**
  * @brief Implements the lseek() system call (SYS_LSEEK).
  * Repositions the read/write offset of an open file descriptor.
  *
  * @param regs Pointer to saved user registers.
  * `regs->ebx` = file descriptor (int fd)
  * `regs->ecx` = offset (off_t offset)
  * `regs->edx` = whence (int whence - SEEK_SET, SEEK_CUR, SEEK_END)
  * @return The resulting offset location from the beginning of the file (>= 0) on success.
  * @return Negative errno on failure (e.g., -EBADF, -EINVAL, -ESPIPE).
  */
 static int sys_lseek_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_lseek_impl");
 
     // Extract arguments
     int fd        = (int)regs->ebx;
     off_t offset  = (off_t)regs->ecx; // Assuming off_t is compatible with uint32_t
     int whence    = (int)regs->edx;
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_LSEEK(fd=%d, offset=%ld, whence=%d)\n",
                          get_current_process()->pid, fd, (long)offset, whence); // Use %ld for off_t? Check type.
 
     // Basic validation for whence (the underlying VFS function might do more)
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
          SYSCALL_DEBUG_PRINTK(" -> EINVAL (invalid whence value %d)\n", whence);
         return -EINVAL;
     }
 
     // Call the underlying sys_lseek function (e.g., in sys_file.c).
     // This handles FD validation and calls the appropriate VFS function.
     off_t result_offset = sys_lseek(fd, offset, whence);
 
     // Returns new offset (>=0) or negative errno.
     SYSCALL_DEBUG_PRINTK("   sys_lseek returned offset = %ld (or error %ld)\n", (long)result_offset, (long)result_offset);
     // Cast result back to int for syscall return. Check potential overflow if off_t > 32 bits.
     // For i386, off_t is likely int32_t or uint32_t, so casting is usually safe.
     return (int)result_offset;
 }
 
 /**
  * @brief Implements the getpid() system call (SYS_GETPID).
  * Returns the process ID of the calling process.
  *
  * @param regs Pointer to saved user registers (unused for this syscall).
  * @return Process ID (PID) of the current process. This syscall always succeeds.
  */
 static int sys_getpid_impl(syscall_regs_t *regs) {
     KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_getpid_impl");
     (void)regs; // Explicitly mark regs as unused
 
     pcb_t* current_proc = get_current_process();
     KERNEL_ASSERT(current_proc != NULL, "sys_getpid called without process context!");
 
     SYSCALL_DEBUG_PRINTK("PID %lu: SYS_GETPID() -> %lu\n", current_proc->pid, current_proc->pid);
     return (int)current_proc->pid;
 }
 
 
 //-----------------------------------------------------------------------------
 // Main Syscall Dispatcher (Called by Assembly)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief The C handler called by the assembly syscall stub (`syscall_handler_asm`).
  *
  * Dispatches the system call based on the number provided in `regs->eax`.
  * It validates the syscall number, retrieves the current process context,
  * calls the appropriate handler function from the `syscall_table`, and sets
  * the return value in `regs->eax`.
  *
  * @param regs Pointer to the saved register state (`syscall_regs_t` / `isr_frame_t`)
  * pushed onto the kernel stack by the assembly handler.
  */
  void syscall_dispatcher(syscall_regs_t *regs) {
     // Use assertion for critical preconditions
     KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");
 
     // Use SYSCALL_DEBUG_PRINTK for debug messages
     SYSCALL_DEBUG_PRINTK("Dispatcher entered.");
 
     uint32_t syscall_num = regs->eax;
     SYSCALL_DEBUG_PRINTK(" -> Syscall Number: %u (0x%x)", syscall_num, syscall_num);
 
     // Get current process context
     pcb_t* current_proc = get_current_process();
     if (!current_proc) {
         // This should ideally not happen during a syscall from a valid process.
         // Panicking might be appropriate, as allowing SYS_EXIT without context is risky.
         SYSCALL_DEBUG_PRINTK(" -> FATAL: No process context during syscall %u!", syscall_num);
         KERNEL_PANIC_HALT("Syscall executed without process context!");
         // Unreachable, but for completeness:
         // regs->eax = (uint32_t)-EFAULT;
         // return;
     }
     uint32_t current_pid = current_proc->pid; // Safe to get PID now
     SYSCALL_DEBUG_PRINTK(" -> Caller PID: %lu", current_pid);
 
 
     // Validate syscall number against table bounds
     int ret_val; // Use signed int for return value (can be negative errno)
     if (syscall_num < MAX_SYSCALLS) {
         syscall_fn_t handler = syscall_table[syscall_num];
         SYSCALL_DEBUG_PRINTK(" -> Handler lookup for %u: %p", syscall_num, handler);
 
         if (handler) {
             // Call the specific syscall implementation
             ret_val = handler(regs);
             SYSCALL_DEBUG_PRINTK(" -> Handler for %u returned %d", syscall_num, ret_val);
         } else {
             // This case should ideally not happen if the table is initialized correctly.
             SYSCALL_DEBUG_PRINTK(" -> Error: NULL handler found for syscall %u.", syscall_num);
             ret_val = -ENOSYS; // Should default to sys_not_implemented anyway
         }
     } else {
         SYSCALL_DEBUG_PRINTK(" -> Error: Invalid syscall number %u (>= %u).", syscall_num, MAX_SYSCALLS);
         ret_val = -ENOSYS; // Error: System call number out of range
     }
 
     // Set the return value in the EAX register slot saved on the stack frame.
     // The assembly handler will restore this value into EAX before iret.
     regs->eax = (uint32_t)ret_val;
     SYSCALL_DEBUG_PRINTK(" -> Set return EAX to %d (0x%x)", ret_val, (uint32_t)ret_val);
     SYSCALL_DEBUG_PRINTK("Dispatcher exiting.");
 }