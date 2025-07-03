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

   // --- Optional: Initialize Standard I/O Descriptors ---
   // If your kernel provides standard I/O handles (e.g., via a console device driver),
   // you would allocate sys_file_t structures for them and place them in fd_table[0], [1], [2] here.
   // This requires interacting with your device/console driver API.
   // Example Placeholder:
   // assign_standard_io_fds(proc); // Hypothetical function
   // ----------------------------------------------------
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