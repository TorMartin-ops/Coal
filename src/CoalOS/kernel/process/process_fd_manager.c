/**
 * @file process_fd_manager.c
 * @brief File descriptor table management for processes
 * 
 * Handles initialization and cleanup of process file descriptor tables.
 * Separated to follow Single Responsibility Principle.
 */

#include <kernel/process/process.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/fs/vfs/fs_limits.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>
#include <kernel/drivers/display/console_dev.h>

// Forward declaration
static void assign_standard_io_fds(pcb_t *proc);

/**
 * @brief Initializes the file descriptor table for a new process.
 * Sets all entries to NULL, indicating no files are open.
 * Should be called during process creation after the PCB is allocated.
 *
 * @param proc Pointer to the new process's PCB.
 */
void process_init_fds(pcb_t *proc) {
   // Use KERNEL_ASSERT for critical preconditions
   KERNEL_ASSERT(proc != NULL, "Cannot initialize FDs for NULL process");

   // Initialize the spinlock associated with this process's FD table
   spinlock_init(&proc->fd_table_lock);

   // Zero out the file descriptor table array.
   // While locking isn't strictly needed here if called only from the
   // single thread creating the process before it runs, it's harmless
   // and good defensive practice.
   uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);
   memset(proc->fd_table, 0, sizeof(proc->fd_table));
   spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

   // --- Initialize Standard I/O Descriptors ---
   // Initialize stdin (fd 0), stdout (fd 1), and stderr (fd 2)
   assign_standard_io_fds(proc);
}

/**
 * @brief Assigns standard I/O file descriptors to a process
 * @param proc Pointer to the process PCB
 */
static void assign_standard_io_fds(pcb_t *proc) {
    // Create sys_file_t structures for standard I/O
    
    // stdin (fd 0) - read-only console
    sys_file_t *stdin_sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (stdin_sf) {
        memset(stdin_sf, 0, sizeof(sys_file_t));
        stdin_sf->vfs_file = create_console_file(CONSOLE_STDIN_MODE);
        if (stdin_sf->vfs_file) {
            proc->fd_table[0] = stdin_sf;
        } else {
            kfree(stdin_sf);
            serial_printf("[FD Init] Failed to create stdin console file\n");
        }
    }
    
    // stdout (fd 1) - write-only console  
    sys_file_t *stdout_sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (stdout_sf) {
        memset(stdout_sf, 0, sizeof(sys_file_t));
        stdout_sf->vfs_file = create_console_file(CONSOLE_STDOUT_MODE);
        if (stdout_sf->vfs_file) {
            proc->fd_table[1] = stdout_sf;
        } else {
            kfree(stdout_sf);
            serial_printf("[FD Init] Failed to create stdout console file\n");
        }
    }
    
    // stderr (fd 2) - write-only console (same as stdout)
    sys_file_t *stderr_sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (stderr_sf) {
        memset(stderr_sf, 0, sizeof(sys_file_t));
        stderr_sf->vfs_file = create_console_file(CONSOLE_STDERR_MODE);
        if (stderr_sf->vfs_file) {
            proc->fd_table[2] = stderr_sf;
        } else {
            kfree(stderr_sf);
            serial_printf("[FD Init] Failed to create stderr console file\n");
        }
    }
    
    serial_printf("[FD Init] Standard I/O descriptors initialized for PID %u\n", proc->pid);
}

/**
 * @brief Closes all open file descriptors for a terminating process.
 * Iterates through the process's FD table and calls sys_close() for each open file.
 * Should be called during process termination *before* freeing the PCB memory.
 *
 * @param proc Pointer to the terminating process's PCB.
 */
void process_close_fds(pcb_t *proc) {
   KERNEL_ASSERT(proc != NULL, "Cannot close FDs for NULL process");
   serial_printf("[Proc %lu] Closing all file descriptors...\n", (unsigned long)proc->pid);

   // Acquire the lock for the FD table of the process being destroyed.
   // Even though the process isn't running, the reaper (e.g., idle task)
   // needs exclusive access during cleanup.
   uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

   // Iterate through the entire file descriptor table
   for (int fd = 0; fd < MAX_FD; fd++) {
       sys_file_t *sf = proc->fd_table[fd];

       if (sf != NULL) { // Check if the file descriptor is currently open
           serial_printf("  [Proc %lu] Closing fd %d (sys_file_t* %p, vfs_file* %p)\n",
                          (unsigned long)proc->pid, fd, sf, sf->vfs_file);

           // Clear the FD table entry FIRST while holding the lock
           proc->fd_table[fd] = NULL;

           // Release the lock *before* calling potentially blocking/complex operations
           // like vfs_close or kfree. This minimizes lock contention, although
           // in this specific cleanup context it might be less critical.
           spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

           // --- Perform cleanup outside the FD table lock ---
           // Call VFS close (safe to call now that FD entry is clear)
           int vfs_ret = vfs_close(sf->vfs_file); // vfs_close handles freeing sf->vfs_file->data and the vnode
           if (vfs_ret < 0) {
               serial_printf("   [Proc %lu] Warning: vfs_close for fd %d returned error %d.\n",
                              (unsigned long)proc->pid, fd, vfs_ret);
           }
           // Free the sys_file structure itself
           kfree(sf);
           // --- End cleanup outside lock ---

           // Re-acquire the lock to continue the loop safely
           irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

       } // end if (sf != NULL)
   } // end for

   // Release the lock after the loop finishes
   spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

   serial_printf("[Proc %lu] All FDs processed for closing.\n", (unsigned long)proc->pid);
}