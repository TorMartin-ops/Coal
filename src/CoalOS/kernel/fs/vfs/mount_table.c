/**
 * mount_table.c - Manages the global list of mounted filesystems.
 *
 * Provides thread-safe operations to add, remove, find, and list mount points.
 * Ensures proper memory management for mount point strings.
 */

 #include <kernel/fs/vfs/mount_table.h>
 #include <kernel/fs/vfs/mount.h>          // For mount_t definition
 #include <kernel/memory/kmalloc.h>        // For memory allocation (kmalloc/kfree)
 #include <kernel/drivers/display/terminal.h>       // For logging/debug output
 #include <kernel/drivers/display/serial.h>        // For serial_printf
 #include <kernel/lib/string.h>         // For strcmp
 #include <kernel/core/types.h>
 #include <kernel/sync/spinlock.h>       // For spinlock synchronization
 #include <kernel/fs/vfs/fs_errno.h>       // For FS_ERR_* error codes
 
 // --- Globals ---
 
 // Head of the singly linked list of mount points
 static mount_t *g_mount_list_head = NULL;
 
 // Spinlock to protect access to the global mount list
 static spinlock_t g_mount_table_lock;
 
 // --- Initialization ---
 
 /**
  * @brief Initializes the mount table subsystem.
  * Must be called once before any other mount_table functions.
  */
 void mount_table_init(void) {
     g_mount_list_head = NULL;
     spinlock_init(&g_mount_table_lock);
     terminal_write("[MountTable] Initialized.\n");
 }
 
 // --- Public API Functions ---
 
 /**
  * @brief Adds a mount entry to the global mount table.
  * Assumes the mount_point string within 'mnt' was dynamically allocated
  * and ownership is transferred to the mount table.
  *
  * @param mnt Pointer to a mount_t structure to add. mount_point must be heap-allocated.
  * @return FS_SUCCESS on success, or a negative error code on failure.
  */
 int mount_table_add(mount_t *mnt) {
     if (!mnt || !mnt->mount_point || !mnt->fs_name || !mnt->fs_context) {
         terminal_write("[MountTable] Error: Attempted to add NULL or incomplete mount entry.\n");
         // If mnt is not NULL but invalid, should we free mnt or its mount_point?
         // Let's assume the caller handles freeing mnt if this function fails here.
         return -FS_ERR_INVALID_PARAM;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&g_mount_table_lock);
 
     // Check for duplicate mount point
     mount_t *iter = g_mount_list_head;
     while (iter) {
         if (strcmp(iter->mount_point, mnt->mount_point) == 0) {
             spinlock_release_irqrestore(&g_mount_table_lock, irq_flags);
             serial_printf("[MountTable] Error: Mount point '%s' already exists.\n", mnt->mount_point);
             // Caller is responsible for freeing the passed 'mnt' and its 'mount_point' string
             // if adding failed due to duplication.
             return -FS_ERR_FILE_EXISTS;
         }
         iter = iter->next;
     }
 
     // Add to front of the list
     mnt->next = g_mount_list_head;
     g_mount_list_head = mnt;
 
     spinlock_release_irqrestore(&g_mount_table_lock, irq_flags);
 
     serial_printf("[MountTable] Added mount: '%s' -> %s\n", mnt->mount_point, mnt->fs_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Removes a mount entry identified by the given mount point string.
  * Frees the mount_t structure AND the dynamically allocated mount_point string.
  *
  * @param mount_point The mount point string (e.g., "/").
  * @return FS_SUCCESS on success, FS_ERR_NOT_FOUND if not found, or other error code.
  */
 int mount_table_remove(const char *mount_point) {
     if (!mount_point) {
         terminal_write("[MountTable] Error: NULL mount point passed to remove.\n");
         return -FS_ERR_INVALID_PARAM;
     }
 
     int result = -FS_ERR_NOT_FOUND; // Assume not found initially
     uintptr_t irq_flags = spinlock_acquire_irqsave(&g_mount_table_lock);
 
     mount_t **prev_next_ptr = &g_mount_list_head;
     mount_t *curr = g_mount_list_head;
 
     while (curr) {
         if (strcmp(curr->mount_point, mount_point) == 0) {
             // Found the entry
             *prev_next_ptr = curr->next; // Unlink from list
 
             // IMPORTANT: Free the allocated mount_point string first
             // Check for NULL before freeing, although mount_table_add validates it.
             if (curr->mount_point) {
                 kfree((void *)curr->mount_point); // Cast away constness for kfree
             } else {
                  serial_printf("[MountTable] Warning: Mount entry being removed has NULL mount_point string.\n");
             }
 
             // Free the mount_t structure itself
             kfree(curr);
 
             serial_printf("[MountTable] Removed mount: '%s'\n", mount_point);
             result = FS_SUCCESS;
             goto exit_remove; // Exit loop once found and removed
         }
         prev_next_ptr = &curr->next;
         curr = curr->next;
     }
 
 exit_remove:
     spinlock_release_irqrestore(&g_mount_table_lock, irq_flags);
 
     if (result == -FS_ERR_NOT_FOUND) {
          serial_printf("[MountTable] Mount point '%s' not found for removal.\n", mount_point);
     }
     return result;
 }
 
 /**
  * @brief Searches for a mount entry by its exact mount point string.
  *
  * @param mount_point The mount point string to search for (e.g., "/").
  * @return Pointer to the found mount_t entry if found, or NULL otherwise.
  * The caller should NOT free the returned pointer.
  */
 mount_t *mount_table_find(const char *mount_point) {
     // If NULL is passed, conceptually return the list head for iteration.
     // This is a slight abuse of the function's name, but avoids needing a separate getter.
     if (mount_point == NULL) {
          // Return head without locking - RACY! Caller beware.
          // A safer approach would require locking or a dedicated getter.
          return g_mount_list_head;
     }
 
     mount_t *found = NULL;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&g_mount_table_lock);
 
     mount_t *iter = g_mount_list_head;
     while (iter) {
         if (strcmp(iter->mount_point, mount_point) == 0) {
             found = iter; // Found the entry
             break;
         }
         iter = iter->next;
     }
 
     spinlock_release_irqrestore(&g_mount_table_lock, irq_flags);
     return found;
 }
 
 /**
  * @brief Prints the current mount table entries to the kernel console.
  * Useful for debugging.
  */
 void mount_table_list(void) {
     terminal_write("[MountTable] Current Mount Entries:\n");
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&g_mount_table_lock);
     mount_t *iter = g_mount_list_head;
 
     if (!iter) {
         terminal_write("  (none)\n");
     } else {
         int count = 0;
         while (iter) {
             count++;
             terminal_printf("  %d: Mount Point: '%s'\n", count, iter->mount_point ? iter->mount_point : "<NULL>");
             terminal_printf("     FS Name:     %s\n", iter->fs_name ? iter->fs_name : "<NULL>");
             terminal_printf("     FS Context:  0x%p\n", iter->fs_context);
             // terminal_printf("     Next Ptr:    0x%p\n", iter->next); // Debug pointer itself
             iter = iter->next;
         }
         if (count == 0) { // Should match (!iter) case, but double-check
              terminal_write("  (none)\n");
         }
     }
 
     spinlock_release_irqrestore(&g_mount_table_lock, irq_flags);
 }
 
 /**
  * @brief Gets the head of the mount list for external iteration.
  * NOTE: Iterating this list without external locking is inherently unsafe
  * if entries can be added/removed concurrently.
  *
  * @return Pointer to the first mount_t entry, or NULL if the list is empty.
  */
 mount_t *mount_table_get_head(void) {
     // We could acquire/release lock here, but it doesn't protect the caller during iteration.
     // Returning the raw head pointer. Caller must be aware of concurrency implications.
     return g_mount_list_head;
 }