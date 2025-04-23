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
 #include "serial.h"      // For serial_write and serial_print_hex
 
 // --- Debug Configuration ---
 // Enable full syscall debug logging via DEBUG_PRINTK (optional)
 #define DEBUG_SYSCALL 1 // <<< Set to 1 to enable logging >>>
 
 #if DEBUG_SYSCALL
 // Ensure DEBUG_PRINTK is defined in debug.h and works correctly
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Syscall] " fmt "\n", ##__VA_ARGS__)
 #else
 #define SYSCALL_DEBUG_PRINTK(fmt, ...) ((void)0) // No-op if disabled
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
 
 // External helper for printing hex via serial
 // Ensure its prototype is available (e.g., in serial.h or debug.h)
 extern void serial_print_hex(uint32_t n);
 
 //-----------------------------------------------------------------------------
 // Syscall Initialization
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Initializes the system call dispatch table.
  * All entries are initially set to `sys_not_implemented`.
  * Specific handlers are then registered for implemented syscalls.
  */
 void syscall_init(void) {
      serial_write(" FNC_ENTER: syscall_init\n");
      DEBUG_PRINTK("Initializing system call table (max %d syscalls)...\n", MAX_SYSCALLS);
      serial_write("  STEP: Looping to init table\n");
      // Initialize all entries to point to the 'not implemented' function
      for (int i = 0; i < MAX_SYSCALLS; i++) {
          syscall_table[i] = sys_not_implemented;
      }
      serial_write("  STEP: Registering handlers\n");
      // Register implemented system calls
      syscall_table[SYS_EXIT]  = sys_exit_impl; // <<< ENSURE THIS IS CORRECT
      syscall_table[SYS_READ]  = sys_read_impl;
      syscall_table[SYS_WRITE] = sys_write_impl;
      syscall_table[SYS_OPEN]  = sys_open_impl;
      syscall_table[SYS_CLOSE] = sys_close_impl;
      syscall_table[SYS_LSEEK] = sys_lseek_impl;
      syscall_table[SYS_GETPID] = sys_getpid_impl;
 
      // --- **FIX**: Verification Assertion ---
      // Explicitly check if the assignment for SYS_EXIT worked immediately.
      // If this assertion fails, the problem is right here in syscall_init
      // (e.g., sys_exit_impl isn't defined correctly, or SYS_EXIT isn't 1).
      serial_write("  STEP: Verifying SYS_EXIT assignment...\n");
      KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "syscall_table[SYS_EXIT] assignment failed! Check definitions.");
      KERNEL_ASSERT(syscall_table[SYS_EXIT] != sys_not_implemented, "syscall_table[SYS_EXIT] incorrectly points to sys_not_implemented!");
      serial_write("  STEP: SYS_EXIT assignment OK.\n");
      // --- END **FIX** ---
 
 
      // --- Pointer Logging for Debugging ---
      serial_write(" DBG:InitPtrs:\n");
      serial_write("  NI@"); serial_print_hex((uint32_t)sys_not_implemented); serial_write("\n");
      serial_write("  EX@"); serial_print_hex((uint32_t)sys_exit_impl); serial_write("\n");
      serial_write("  T0@"); serial_print_hex((uint32_t)syscall_table[0]); serial_write("\n"); // Should be NI
      serial_write("  T1@"); serial_print_hex((uint32_t)syscall_table[1]); serial_write("\n"); // Should be EX (verified above)
      serial_write("  T3@"); serial_print_hex((uint32_t)syscall_table[SYS_READ]); serial_write("\n"); // Should be sys_read_impl
      // Also log via DEBUG_PRINTK if enabled/working
      #if DEBUG_SYSCALL
      DEBUG_PRINTK("   sys_not_implemented func ptr = %p\n", sys_not_implemented);
      DEBUG_PRINTK("   sys_exit_impl       func ptr = %p\n", sys_exit_impl);
      DEBUG_PRINTK("   syscall_table[0]    func ptr = %p\n", syscall_table[0]);
      DEBUG_PRINTK("   syscall_table[1]    func ptr = %p\n", syscall_table[1]);
      DEBUG_PRINTK("   syscall_table[3]    func ptr = %p\n", syscall_table[SYS_READ]);
      #endif
      // --- END Logging ---
 
 
      DEBUG_PRINTK("System call table initialization complete.\n");
      serial_write(" FNC_EXIT: syscall_init\n");
 }
 
 //-----------------------------------------------------------------------------
 // Static Helper: Safe String Copy from User Space
 //-----------------------------------------------------------------------------
 // ... (strncpy_from_user_safe function remains the same as the previous verbose version)
 static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
      serial_write(" FNC_ENTER: strncpy_from_user_safe\n");
      serial_write("  STEP: Checking maxlen\n");
      if (maxlen == 0) {
           serial_write("  RET: -EINVAL (maxlen=0)\n");
          return -EINVAL;
      }
      serial_write("  STEP: Basic u_src check\n");
      // Basic address check
      if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
          serial_write("  RET: -EFAULT (bad u_src)\n");
          return -EFAULT;
      }
 
      KERNEL_ASSERT(k_dst != NULL, "Kernel destination buffer cannot be NULL");
      SYSCALL_DEBUG_PRINTK("strncpy_from_user_safe: Copying from u_src=%p to k_dst=%p (maxlen=%u)\n", u_src, k_dst, maxlen);
      serial_write("  STEP: Entering copy loop\n");
      size_t len = 0;
      while (len < maxlen) {
           serial_write("   LOOP: Top\n");
          char current_char;
          // Try to copy one byte
          serial_write("   LOOP: Calling copy_from_user\n");
          SYSCALL_DEBUG_PRINTK("  strncpy: Attempting copy_from_user for byte %u at %p\n", len, u_src + len);
          size_t not_copied = copy_from_user(&current_char, u_src + len, 1);
           serial_write("   LOOP: copy_from_user returned\n");
 
          serial_write("   LOOP: Checking not_copied\n");
          if (not_copied > 0) {
              serial_write("   LOOP: Fault detected!\n");
              // Fault occurred accessing user memory. Null-terminate kernel buffer safely.
              k_dst[len > 0 ? len -1 : 0] = '\0'; // Terminate at last potentially valid point
              SYSCALL_DEBUG_PRINTK("  strncpy: Fault copying byte %u from %p. Returning -EFAULT\n", len, u_src + len);
              serial_write("  RET: -EFAULT (fault during copy)\n");
              return -EFAULT;
          }
 
          // Store the copied byte
           serial_write("   LOOP: Storing char\n");
          k_dst[len] = current_char;
          SYSCALL_DEBUG_PRINTK("  strncpy: Copied byte %u = '%c' (0x%x)\n", len, (current_char >= ' ' ? current_char : '?'), current_char);
 
 
          // Check for null terminator
          serial_write("   LOOP: Checking null terminator\n");
          if (current_char == '\0') {
              SYSCALL_DEBUG_PRINTK("  strncpy: Found null terminator at length %u. Success.\n", len);
              serial_write("  RET: 0 (Success)\n");
              return 0; // Success: Found null terminator within maxlen.
          }
 
           serial_write("   LOOP: Incrementing len\n");
          len++; // Advance to next character
      }
      serial_write("  STEP: Loop finished (maxlen reached)\n");
      // Reached maxlen without finding null terminator.
      k_dst[maxlen - 1] = '\0'; // Ensure null termination.
      SYSCALL_DEBUG_PRINTK("  strncpy: String from %p exceeded maxlen %u. Returning -ENAMETOOLONG\n", u_src, maxlen);
      serial_write("  RET: -ENAMETOOLONG\n");
      return -ENAMETOOLONG;
 }
 
 //-----------------------------------------------------------------------------
 // Syscall Implementations
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Handles unimplemented system calls.
  */
 static int sys_not_implemented(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_not_implemented\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_not_implemented");
      serial_write("  STEP: Getting process\n");
      pcb_t* current_proc = get_current_process();
      KERNEL_ASSERT(current_proc != NULL, "No process context in sys_not_implemented");
      uint32_t pid = current_proc->pid;
      uint32_t syscall_num = regs->eax;
 
      SYSCALL_DEBUG_PRINTK("PID %lu: Called unimplemented syscall %u. Returning -ENOSYS.\n", pid, syscall_num);
      serial_write(" FNC_EXIT: sys_not_implemented (-ENOSYS)\n");
      return -ENOSYS; // Function not implemented
 }
 
 /**
  * @brief Implements the exit() system call (SYS_EXIT).
  */
 static int sys_exit_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_exit_impl\n"); // <<< Check if this appears!
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_exit_impl");
 
      // Extract exit code from EBX (Argument 0)
      serial_write("  STEP: Extracting exit code\n");
      int exit_code = (int)regs->ebx;
      serial_write("  DBG: ExitCode="); serial_print_hex(exit_code); serial_write("\n");
 
 
      serial_write("  STEP: Getting process\n");
      pcb_t* current_proc = get_current_process();
      KERNEL_ASSERT(current_proc != NULL, "sys_exit called without process context!");
      uint32_t pid = current_proc->pid;
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_EXIT(exit_code=%d) called.\n", pid, exit_code);
      serial_write("  STEP: Calling remove_current_task...\n");
      SYSCALL_DEBUG_PRINTK("  Calling remove_current_task_with_code(%d)...\n", exit_code);
 
      remove_current_task_with_code(exit_code);
 
      // Should not return
      serial_write("  STEP: ERROR! remove_current_task returned!\n");
      SYSCALL_DEBUG_PRINTK("  FATAL ERROR: remove_current_task_with_code returned!\n");
      KERNEL_PANIC_HALT("FATAL: remove_current_task_with_code returned in sys_exit!");
      serial_write(" FNC_EXIT: sys_exit_impl (PANIC!)\n"); // Should be unreachable
      return 0; // Unreachable code
 }
 
 /**
  * @brief Implements the read() system call (SYS_READ).
  */
 // ... (sys_read_impl function remains the same as the previous verbose version)
 static int sys_read_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_read_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_read_impl");
 
      // Extract arguments
      serial_write("  STEP: Extracting args (fd, buf, count)\n");
      int fd              = (int)regs->ebx;
      void *user_buf      = (void*)regs->ecx;
      size_t count        = (size_t)regs->edx;
      uint32_t pid        = get_current_process() ? get_current_process()->pid : 0; // Handle potential NULL
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_READ(fd=%d, buf=%p, count=%u)\n", pid, fd, user_buf, count);
 
      // Argument Validation
      serial_write("  STEP: Validating count\n");
      if ((ssize_t)count < 0) { // Check if count interpreted as signed is negative
          SYSCALL_DEBUG_PRINTK(" -> EINVAL (negative count %d)\n", count);
          serial_write("  RET: -EINVAL (negative count)\n");
          return -EINVAL;
      }
      if (count == 0) {
          SYSCALL_DEBUG_PRINTK(" -> OK (count is 0)\n");
           serial_write("  RET: 0 (count=0)\n");
          return 0; // Reading 0 bytes is a no-op, success.
      }
 
      // Check user buffer writability
      serial_write("  STEP: Calling access_ok\n");
      SYSCALL_DEBUG_PRINTK("  Checking access_ok(WRITE, %p, %u)...\n", user_buf, count);
      bool access_ok_res = access_ok(VERIFY_WRITE, user_buf, count);
      serial_write("  STEP: access_ok returned\n");
      if (!access_ok_res) {
          SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)\n", user_buf);
          serial_write("  RET: -EFAULT (access_ok failed)\n");
          return -EFAULT;
      }
      SYSCALL_DEBUG_PRINTK("  access_ok passed.\n");
 
 
      // Allocate kernel buffer
      size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
      serial_write("  STEP: Calling kmalloc\n");
      SYSCALL_DEBUG_PRINTK("  Allocating kernel buffer (size %u)...\n", chunk_alloc_size);
      char* kbuf = kmalloc(chunk_alloc_size);
      serial_write("  STEP: kmalloc returned\n");
      if (!kbuf) {
          SYSCALL_DEBUG_PRINTK(" -> ENOMEM (kmalloc failed for kernel buffer)\n");
          serial_write("  RET: -ENOMEM (kmalloc failed)\n");
          return -ENOMEM;
      }
       SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p.\n", kbuf);
 
 
      ssize_t total_read = 0;
      int final_ret_val = 0; // Used to store return value in case of errors during loop
 
      // Loop to read data in chunks
      serial_write("  STEP: Entering read loop\n");
      SYSCALL_DEBUG_PRINTK("  Entering read loop (total_read=%d, count=%u)...\n", total_read, count);
      while (total_read < (ssize_t)count) {
           serial_write("   LOOP_READ: Top\n");
          size_t current_chunk_size = MIN(chunk_alloc_size, count - total_read);
          KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in read loop");
          SYSCALL_DEBUG_PRINTK("  Loop: Requesting chunk size %u (total_read %d)\n", current_chunk_size, total_read);
 
          // Call underlying sys_read
          serial_write("   LOOP_READ: Calling sys_read (underlying)\n");
          SYSCALL_DEBUG_PRINTK("   Calling sys_read(%d, kbuf=%p, current_chunk_size=%u)...\n", fd, kbuf, current_chunk_size);
          ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
          serial_write("   LOOP_READ: sys_read returned\n");
          SYSCALL_DEBUG_PRINTK("   sys_read returned %d\n", bytes_read_this_chunk);
 
          serial_write("   LOOP_READ: Checking result\n");
          if (bytes_read_this_chunk < 0) {
              serial_write("   LOOP_READ: Error from sys_read\n");
              final_ret_val = (total_read > 0) ? total_read : bytes_read_this_chunk;
              SYSCALL_DEBUG_PRINTK(" -> Error %d from underlying sys_read. Breaking loop. Returning %d.\n", bytes_read_this_chunk, final_ret_val);
              goto sys_read_cleanup_and_exit; // Use goto for single cleanup point
          }
          if (bytes_read_this_chunk == 0) {
              serial_write("   LOOP_READ: EOF from sys_read\n");
              SYSCALL_DEBUG_PRINTK(" -> EOF reached by underlying sys_read. Breaking loop.\n");
              break; // Exit loop
          }
 
          // Copy back to user
          serial_write("   LOOP_READ: Calling copy_to_user\n");
          SYSCALL_DEBUG_PRINTK("   Calling copy_to_user(dst=%p, src=%p, size=%d)...\n", (char*)user_buf + total_read, kbuf, bytes_read_this_chunk);
          size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, bytes_read_this_chunk);
          serial_write("   LOOP_READ: copy_to_user returned\n");
          SYSCALL_DEBUG_PRINTK("   copy_to_user returned not_copied=%u\n", not_copied);
 
          serial_write("   LOOP_READ: Checking not_copied\n");
          if (not_copied > 0) {
               serial_write("   LOOP_READ: Fault during copy_to_user\n");
              size_t copied_back_this_chunk = bytes_read_this_chunk - not_copied;
              total_read += copied_back_this_chunk;
              SYSCALL_DEBUG_PRINTK(" -> EFAULT during copy_to_user (copied %u/%d bytes this chunk). Total read: %d. Breaking loop.\n",
                                   copied_back_this_chunk, bytes_read_this_chunk, total_read);
              final_ret_val = (total_read > 0) ? total_read : -EFAULT;
              goto sys_read_cleanup_and_exit;
          }
 
          // Successfully read and copied back
          serial_write("   LOOP_READ: Chunk success\n");
          total_read += bytes_read_this_chunk;
          SYSCALL_DEBUG_PRINTK("   Chunk processed successfully. total_read=%d\n", total_read);
 
          serial_write("   LOOP_READ: Checking short read\n");
          if ((size_t)bytes_read_this_chunk < current_chunk_size) {
              serial_write("   LOOP_READ: Short read detected\n");
              SYSCALL_DEBUG_PRINTK(" -> Short read from underlying sys_read (%d < %u). Breaking loop.\n", bytes_read_this_chunk, current_chunk_size);
              break;
          }
          serial_write("   LOOP_READ: Bottom\n");
      } // end while
      serial_write("  STEP: Exited read loop\n");
 
      final_ret_val = total_read;
      SYSCALL_DEBUG_PRINTK(" -> Read loop finished. OK (Total bytes read: %d)\n", final_ret_val);
 
 sys_read_cleanup_and_exit:
      serial_write("  STEP: Cleanup\n");
      if (kbuf) {
          serial_write("   ACTION: Freeing kbuf\n");
          SYSCALL_DEBUG_PRINTK("  Freeing kernel buffer %p.\n", kbuf);
          kfree(kbuf);
      }
      SYSCALL_DEBUG_PRINTK("  SYS_READ returning %d.\n", final_ret_val);
      serial_write(" FNC_EXIT: sys_read_impl\n");
      return final_ret_val;
 }
 
 /**
  * @brief Implements the write() system call (SYS_WRITE).
  */
 // ... (sys_write_impl function remains the same as the previous verbose version)
 static int sys_write_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_write_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_write_impl");
 
      // Extract arguments
      serial_write("  STEP: Extracting args (fd, buf, count)\n");
      int fd                = (int)regs->ebx;
      const void *user_buf  = (const void*)regs->ecx;
      size_t count          = (size_t)regs->edx;
      uint32_t pid          = get_current_process() ? get_current_process()->pid : 0;
 
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_WRITE(fd=%d, buf=%p, count=%u)\n", pid, fd, user_buf, count);
 
      // Argument Validation
      serial_write("  STEP: Validating count\n");
      if ((ssize_t)count < 0) {
          SYSCALL_DEBUG_PRINTK(" -> EINVAL (negative count %d)\n", count);
          serial_write("  RET: -EINVAL (negative count)\n");
          return -EINVAL;
      }
      if (count == 0) {
           SYSCALL_DEBUG_PRINTK(" -> OK (count is 0)\n");
           serial_write("  RET: 0 (count=0)\n");
          return 0;
      }
 
      // Check user buffer readability
      serial_write("  STEP: Calling access_ok\n");
      SYSCALL_DEBUG_PRINTK("  Checking access_ok(READ, %p, %u)...\n", user_buf, count);
      bool access_ok_res = access_ok(VERIFY_READ, user_buf, count);
      serial_write("  STEP: access_ok returned\n");
      if (!access_ok_res) {
          SYSCALL_DEBUG_PRINTK(" -> EFAULT (access_ok failed for user buffer %p)\n", user_buf);
           serial_write("  RET: -EFAULT (access_ok failed)\n");
          return -EFAULT;
      }
      SYSCALL_DEBUG_PRINTK("  access_ok passed.\n");
 
 
      // Allocate kernel buffer
      size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
      serial_write("  STEP: Calling kmalloc\n");
      SYSCALL_DEBUG_PRINTK("  Allocating kernel buffer (size %u)...\n", chunk_alloc_size);
      char* kbuf = kmalloc(chunk_alloc_size);
      serial_write("  STEP: kmalloc returned\n");
      if (!kbuf) {
          SYSCALL_DEBUG_PRINTK(" -> ENOMEM (kmalloc failed for kernel buffer)\n");
           serial_write("  RET: -ENOMEM (kmalloc failed)\n");
          return -ENOMEM;
      }
      SYSCALL_DEBUG_PRINTK("  Kernel buffer allocated at %p.\n", kbuf);
 
 
      ssize_t total_written = 0;
      int final_ret_val = 0;
 
      // Handle console FDs
      serial_write("  STEP: Checking fd type (console?)\n");
      if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
          serial_write("  STEP: Console fd detected. Entering write loop.\n");
          SYSCALL_DEBUG_PRINTK("  Handling write to STDOUT/STDERR (fd=%d)\n", fd);
          while(total_written < (ssize_t)count) {
              serial_write("   LOOP_WRITE_CON: Top\n");
              size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
              KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in console write loop");
              SYSCALL_DEBUG_PRINTK("  Loop: Requesting chunk size %u (total_written %d)\n", current_chunk_size, total_written);
 
              // Copy from user
              serial_write("   LOOP_WRITE_CON: Calling copy_from_user\n");
              SYSCALL_DEBUG_PRINTK("   Calling copy_from_user(dst=%p, src=%p, size=%u)...\n", kbuf, (char*)user_buf + total_written, current_chunk_size);
              size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
              serial_write("   LOOP_WRITE_CON: copy_from_user returned\n");
              SYSCALL_DEBUG_PRINTK("   copy_from_user returned not_copied=%u\n", not_copied);
              size_t copied_this_chunk = current_chunk_size - not_copied;
 
              // Write to terminal
              serial_write("   LOOP_WRITE_CON: Checking copied_this_chunk\n");
              if (copied_this_chunk > 0) {
                  serial_write("   LOOP_WRITE_CON: Calling terminal_write_bytes\n");
                  SYSCALL_DEBUG_PRINTK("   Calling terminal_write_bytes(buf=%p, size=%u)...\n", kbuf, copied_this_chunk);
                  terminal_write_bytes(kbuf, copied_this_chunk);
                  serial_write("   LOOP_WRITE_CON: terminal_write_bytes returned\n");
                  SYSCALL_DEBUG_PRINTK("   terminal_write_bytes finished.\n");
                  total_written += copied_this_chunk;
              }
 
              // Check for copy fault
              serial_write("   LOOP_WRITE_CON: Checking not_copied\n");
              if (not_copied > 0) {
                   serial_write("   LOOP_WRITE_CON: Fault during copy_from_user\n");
                  SYSCALL_DEBUG_PRINTK(" -> EFAULT during copy_from_user (copied %u/%u bytes this chunk). Total written: %d. Breaking loop.\n",
                                       copied_this_chunk, current_chunk_size, total_written);
                  final_ret_val = (total_written > 0) ? total_written : -EFAULT;
                  goto sys_write_cleanup_and_exit;
              }
 
              serial_write("   LOOP_WRITE_CON: Checking short copy\n");
              if (copied_this_chunk < current_chunk_size) {
                   serial_write("   LOOP_WRITE_CON: Short copy detected\n");
                   SYSCALL_DEBUG_PRINTK("   Short copy (%u < %u) from user, assuming end of request.\n", copied_this_chunk, current_chunk_size);
                   break;
              }
               SYSCALL_DEBUG_PRINTK("   Chunk processed successfully. total_written=%d\n", total_written);
               serial_write("   LOOP_WRITE_CON: Bottom\n");
          }
           serial_write("  STEP: Console write loop finished\n");
          final_ret_val = total_written;
      }
      // Handle file FDs
      else {
           serial_write("  STEP: File fd detected. Entering write loop.\n");
           SYSCALL_DEBUG_PRINTK("  Handling write to file fd=%d\n", fd);
           while(total_written < (ssize_t)count) {
               serial_write("   LOOP_WRITE_FILE: Top\n");
              size_t current_chunk_size = MIN(chunk_alloc_size, count - total_written);
              KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in file write loop");
              SYSCALL_DEBUG_PRINTK("  Loop: Requesting chunk size %u (total_written %d)\n", current_chunk_size, total_written);
 
              // Copy from user
              serial_write("   LOOP_WRITE_FILE: Calling copy_from_user\n");
              SYSCALL_DEBUG_PRINTK("   Calling copy_from_user(dst=%p, src=%p, size=%u)...\n", kbuf, (char*)user_buf + total_written, current_chunk_size);
              size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
              serial_write("   LOOP_WRITE_FILE: copy_from_user returned\n");
              SYSCALL_DEBUG_PRINTK("   copy_from_user returned not_copied=%u\n", not_copied);
              size_t copied_this_chunk = current_chunk_size - not_copied;
 
              // Write using sys_write
              serial_write("   LOOP_WRITE_FILE: Checking copied_this_chunk\n");
              if (copied_this_chunk > 0) {
                  serial_write("   LOOP_WRITE_FILE: Calling sys_write (underlying)\n");
                  SYSCALL_DEBUG_PRINTK("   Calling sys_write(%d, kbuf=%p, size=%u)...\n", fd, kbuf, copied_this_chunk);
                  ssize_t bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk);
                  serial_write("   LOOP_WRITE_FILE: sys_write returned\n");
                  SYSCALL_DEBUG_PRINTK("   sys_write returned %d\n", bytes_written_this_chunk);
 
                  serial_write("   LOOP_WRITE_FILE: Checking result\n");
                  if (bytes_written_this_chunk < 0) {
                       serial_write("   LOOP_WRITE_FILE: Error from sys_write\n");
                      final_ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk;
                       SYSCALL_DEBUG_PRINTK(" -> Error %d from underlying sys_write. Breaking loop. Returning %d.\n", bytes_written_this_chunk, final_ret_val);
                      goto sys_write_cleanup_and_exit;
                  }
 
                  total_written += bytes_written_this_chunk;
 
                  serial_write("   LOOP_WRITE_FILE: Checking short write\n");
                  if ((size_t)bytes_written_this_chunk < copied_this_chunk) {
                      serial_write("   LOOP_WRITE_FILE: Short write detected\n");
                       SYSCALL_DEBUG_PRINTK(" -> Short write from underlying sys_write (%d < %u). Breaking loop.\n", bytes_written_this_chunk, copied_this_chunk);
                       break;
                  }
              }
 
              // Check for copy fault
              serial_write("   LOOP_WRITE_FILE: Checking not_copied\n");
              if (not_copied > 0) {
                   serial_write("   LOOP_WRITE_FILE: Fault during copy_from_user\n");
                   SYSCALL_DEBUG_PRINTK(" -> EFAULT during copy_from_user (copied %u/%u bytes this chunk). Total written: %d. Breaking loop.\n",
                                        copied_this_chunk, current_chunk_size, total_written);
                  final_ret_val = (total_written > 0) ? total_written : -EFAULT;
                  goto sys_write_cleanup_and_exit;
              }
 
               serial_write("   LOOP_WRITE_FILE: Checking short copy\n");
               if (copied_this_chunk < current_chunk_size) {
                    serial_write("   LOOP_WRITE_FILE: Short copy detected\n");
                    SYSCALL_DEBUG_PRINTK("   Short copy (%u < %u) from user, assuming end of request.\n", copied_this_chunk, current_chunk_size);
                    break;
               }
                SYSCALL_DEBUG_PRINTK("   Chunk processed successfully. total_written=%d\n", total_written);
                serial_write("   LOOP_WRITE_FILE: Bottom\n");
          }
          serial_write("  STEP: File write loop finished\n");
          final_ret_val = total_written;
      }
 
      SYSCALL_DEBUG_PRINTK(" -> Write loop finished. OK (Total bytes written: %d)\n", final_ret_val);
 
 sys_write_cleanup_and_exit:
      serial_write("  STEP: Cleanup\n");
      if (kbuf) {
           serial_write("   ACTION: Freeing kbuf\n");
          SYSCALL_DEBUG_PRINTK("  Freeing kernel buffer %p.\n", kbuf);
          kfree(kbuf);
      }
      SYSCALL_DEBUG_PRINTK("  SYS_WRITE returning %d.\n", final_ret_val);
      serial_write(" FNC_EXIT: sys_write_impl\n");
      return final_ret_val;
 }
 
 
 /**
  * @brief Implements the open() system call (SYS_OPEN).
  */
 static int sys_open_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_open_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_open_impl");
 
      // Extract arguments
      serial_write("  STEP: Extracting args (path, flags, mode)\n");
      const char *user_pathname = (const char*)regs->ebx;
      int flags                 = (int)regs->ecx;
      int mode                  = (int)regs->edx;
      uint32_t pid              = get_current_process() ? get_current_process()->pid : 0;
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_OPEN(path_user=%p, flags=0x%x, mode=0%o)\n", pid, user_pathname, flags, mode);
 
      // Allocate kernel buffer for path.
      char k_pathname[MAX_SYSCALL_STR_LEN];
 
      // Safely copy pathname from user space.
      serial_write("  STEP: Calling strncpy_from_user_safe\n");
      SYSCALL_DEBUG_PRINTK("  Calling strncpy_from_user_safe...\n");
      int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, MAX_SYSCALL_STR_LEN);
      serial_write("  STEP: strncpy_from_user_safe returned\n");
      if (copy_err != 0) {
           SYSCALL_DEBUG_PRINTK(" -> Error %d copying path from user %p\n", copy_err, user_pathname);
           serial_write("  RET: Error from strncpy\n");
          return copy_err; // Return -EFAULT or -ENAMETOOLONG
      }
      SYSCALL_DEBUG_PRINTK("  Copied path to kernel: '%s'\n", k_pathname);
 
      // Call the underlying sys_open function.
      serial_write("  STEP: Calling sys_open (underlying)\n");
      SYSCALL_DEBUG_PRINTK("  Calling sys_open(kpath='%s', flags=0x%x, mode=0%o)...\n", k_pathname, flags, mode);
      int fd = sys_open(k_pathname, flags, mode);
      serial_write("  STEP: sys_open returned\n");
      SYSCALL_DEBUG_PRINTK("  sys_open returned fd = %d\n", fd);
 
      serial_write(" FNC_EXIT: sys_open_impl\n");
      return fd;
 }
 
 /**
  * @brief Implements the close() system call (SYS_CLOSE).
  */
 static int sys_close_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_close_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_close_impl");
 
      // Extract file descriptor argument
      serial_write("  STEP: Extracting fd arg\n");
      int fd = (int)regs->ebx;
      uint32_t pid = get_current_process() ? get_current_process()->pid : 0;
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_CLOSE(fd=%d)\n", pid, fd);
 
      // Call the underlying sys_close function.
      serial_write("  STEP: Calling sys_close (underlying)\n");
      SYSCALL_DEBUG_PRINTK("  Calling sys_close(%d)...\n", fd);
      int ret = sys_close(fd); // Returns 0 or negative errno
      serial_write("  STEP: sys_close returned\n");
      SYSCALL_DEBUG_PRINTK("  sys_close returned %d\n", ret);
 
      serial_write(" FNC_EXIT: sys_close_impl\n");
      return ret;
 }
 
 /**
  * @brief Implements the lseek() system call (SYS_LSEEK).
  */
 static int sys_lseek_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_lseek_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_lseek_impl");
 
      // Extract arguments
      serial_write("  STEP: Extracting args (fd, offset, whence)\n");
      int fd        = (int)regs->ebx;
      off_t offset  = (off_t)regs->ecx;
      int whence    = (int)regs->edx;
      uint32_t pid  = get_current_process() ? get_current_process()->pid : 0;
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_LSEEK(fd=%d, offset=%ld, whence=%d)\n", pid, fd, (long)offset, whence);
 
      // Basic validation for whence
      serial_write("  STEP: Validating whence\n");
      if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
           SYSCALL_DEBUG_PRINTK(" -> EINVAL (invalid whence value %d)\n", whence);
           serial_write("  RET: -EINVAL (bad whence)\n");
          return -EINVAL;
      }
 
      // Call the underlying sys_lseek function.
      serial_write("  STEP: Calling sys_lseek (underlying)\n");
      SYSCALL_DEBUG_PRINTK("  Calling sys_lseek(%d, %ld, %d)...\n", fd, (long)offset, whence);
      off_t result_offset = sys_lseek(fd, offset, whence);
      serial_write("  STEP: sys_lseek returned\n");
      SYSCALL_DEBUG_PRINTK("  sys_lseek returned offset = %ld (or error %ld)\n", (long)result_offset, (long)result_offset);
 
      serial_write(" FNC_EXIT: sys_lseek_impl\n");
      return (int)result_offset;
 }
 
 /**
  * @brief Implements the getpid() system call (SYS_GETPID).
  */
 static int sys_getpid_impl(syscall_regs_t *regs) {
      serial_write(" FNC_ENTER: sys_getpid_impl\n");
      KERNEL_ASSERT(regs != NULL, "NULL regs passed to sys_getpid_impl");
      (void)regs; // Explicitly mark regs as unused
 
      serial_write("  STEP: Getting process\n");
      pcb_t* current_proc = get_current_process();
      KERNEL_ASSERT(current_proc != NULL, "sys_getpid called without process context!");
 
      SYSCALL_DEBUG_PRINTK("PID %lu: SYS_GETPID() -> Returning PID %lu\n", current_proc->pid, current_proc->pid);
      serial_write(" FNC_EXIT: sys_getpid_impl\n");
      return (int)current_proc->pid;
 }
 
 
 //-----------------------------------------------------------------------------
 // Main Syscall Dispatcher (Called by Assembly)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief The C handler called by the assembly syscall stub (`syscall_handler_asm`).
  * Dispatches the system call based on the number provided in `regs->eax`.
  */
  void syscall_dispatcher(syscall_regs_t *regs) {
      // Use low-level serial write for entry/exit
      serial_write("SD: Enter\n");
 
      // Use assertion for critical preconditions
      KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");
 
      // Use SYSCALL_DEBUG_PRINTK for higher-level debug messages
      SYSCALL_DEBUG_PRINTK("Dispatcher entered. Frame at %p.", regs);
 
      uint32_t syscall_num = regs->eax;
      SYSCALL_DEBUG_PRINTK(" -> Extracted syscall number: %u (0x%x)", syscall_num, syscall_num);
      SYSCALL_DEBUG_PRINTK(" -> Raw Args: EBX=0x%x, ECX=0x%x, EDX=0x%x, ESI=0x%x, EDI=0x%x",
                           regs->ebx, regs->ecx, regs->edx, regs->esi, regs->edi);
 
      // Get current process context
      serial_write("SD: GetProc\n");
      pcb_t* current_proc = get_current_process();
      SYSCALL_DEBUG_PRINTK(" --> current_proc = %p", current_proc);
 
      serial_write("SD: ChkProc\n");
      if (!current_proc) {
          serial_write("SD: ERR NoProc!\n");
          SYSCALL_DEBUG_PRINTK(" -> FATAL: No process context during syscall %u!", syscall_num);
          KERNEL_PANIC_HALT("Syscall executed without process context!");
      }
      uint32_t current_pid = current_proc->pid;
      SYSCALL_DEBUG_PRINTK(" -> Caller PID: %lu", current_pid);
 
      // Validate syscall number
      int ret_val;
      serial_write("SD: ChkBounds\n");
      SYSCALL_DEBUG_PRINTK(" --> Checking syscall number bounds (%u < %u)...", syscall_num, MAX_SYSCALLS);
      if (syscall_num < MAX_SYSCALLS) {
          serial_write("SD: InBounds\n");
          serial_write("SD: LookupHnd\n");
          syscall_fn_t handler = syscall_table[syscall_num];
          SYSCALL_DEBUG_PRINTK(" ---> Handler lookup result: Addr=%p", handler);
 
          // --- Pointer Logging via Serial ---
          serial_write(" DBG:DispatchPtrs:\n");
          serial_write("  Hnd@"); serial_print_hex((uint32_t)handler); serial_write("\n");
          serial_write("  NI @"); serial_print_hex((uint32_t)sys_not_implemented); serial_write("\n");
          // --- END Logging ---
 
          // --- Start Corrected Handler Logic ---
          serial_write("SD: ChkHnd (New Logic)\n");
          SYSCALL_DEBUG_PRINTK(" ---> Checking handler validity and type...");
 
          if (handler) { // First, ensure handler is not NULL
              SYSCALL_DEBUG_PRINTK(" ---> Handler is not NULL.");
              // --- Added: Pointer Comparison Logging ---
              serial_write(" DBG:Compare Hnd vs NI:\n");
              serial_write("  Hnd@"); serial_print_hex((uint32_t)handler); serial_write("\n");
              serial_write("  NI @"); serial_print_hex((uint32_t)sys_not_implemented); serial_write("\n");
              // --- END Added ---
              if (handler == sys_not_implemented) {
                  // Handler points specifically to sys_not_implemented
                  serial_write("SD: CallNI (New Logic)\n");
                  SYSCALL_DEBUG_PRINTK(" ---> Handler is sys_not_implemented. Calling it...");
                  ret_val = sys_not_implemented(regs); // Explicitly call for clarity
                  serial_write("SD: NIRet (New Logic)\n");
                  SYSCALL_DEBUG_PRINTK(" ---> sys_not_implemented returned %d", ret_val);
              } else {
                  // Handler is valid and not sys_not_implemented
                  serial_write("SD: CallHnd (New Logic)\n");
                  SYSCALL_DEBUG_PRINTK(" ---> Handler is implemented (%p). Calling it...", handler);
                  ret_val = handler(regs); // Call the actual implementation
                  serial_write("SD: HndRet (New Logic)\n");
                  SYSCALL_DEBUG_PRINTK(" -> Handler for %u returned %d (0x%x)", syscall_num, ret_val, (uint32_t)ret_val);
              }
          } else {
              // Handler was NULL in the table (shouldn't happen after syscall_init)
              serial_write("SD: ERR NullHnd (New Logic)\n");
              SYSCALL_DEBUG_PRINTK(" ---> Error: NULL handler found for syscall %u in table! Returning -ENOSYS.", syscall_num);
              ret_val = -ENOSYS;
          }
          // --- End Corrected Handler Logic ---
 
      }
      // Handle syscall number out of bounds
      else {
          serial_write("SD: ERR Bounds\n");
          SYSCALL_DEBUG_PRINTK(" --> Syscall number is out of bounds (>= %u). Returning -ENOSYS.", MAX_SYSCALLS);
          ret_val = -ENOSYS; // Error: System call number out of range
      }
 
      // Set return value
      serial_write("SD: SetRet\n");
      SYSCALL_DEBUG_PRINTK(" --> Preparing to set return value in regs->eax...");
      regs->eax = (uint32_t)ret_val;
      SYSCALL_DEBUG_PRINTK(" -> Set return EAX to %d (0x%x)", ret_val, (uint32_t)ret_val);
 
      serial_write("SD: Exit\n");
      SYSCALL_DEBUG_PRINTK("Dispatcher exiting.");
 }