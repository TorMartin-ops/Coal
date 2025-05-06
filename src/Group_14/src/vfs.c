/**
 * @file vfs.c
 * @brief Virtual File System (VFS) Core Implementation
 * @author Tor Martin Kohle
 * @version 1.3
 *
 * Implements the VFS layer for UiAOS, providing an abstraction over concrete
 * filesystem implementations. Key responsibilities include:
 * - Managing registration and lookup of filesystem drivers.
 * - Handling mount points via the mount_table module.
 * - Dispatching file operations (open, close, read, write, lseek, readdir, unlink)
 * to the appropriate underlying filesystem driver based on path resolution.
 * - Enforcing basic file access semantics and managing file handle state.
 *
 * Core Design Points:
 * - Path Resolution: Employs longest prefix matching to determine the correct
 * mount point for a given path.
 * - Driver Management: Maintains a simple linked list of registered drivers.
 * - Concurrency: Utilizes spinlocks to protect shared VFS structures, including
 * the global driver list, mount table (via its own API), and per-file
 * `file_t` structures (specifically for file offset and state during I/O).
 * - Error Handling: Propagates error codes from underlying drivers or returns
 * standardized FS_ERR_* / POSIX-style negative errno values.
 *
 * Notable Changes (v1.3):
 * - Introduced per-file spinlock (`file_t.lock`) for thread-safe file operations.
 * - Enhanced locking around file offset manipulation and driver calls in
 * `vfs_read`, `vfs_write`, and `vfs_lseek`.
 */

 #include "vfs.h"           // VFS interface (vfs_driver_t, file_t, vnode_t)
 #include "kmalloc.h"       // Kernel memory allocation (kmalloc, kfree)
 #include "terminal.h"      // Kernel console logging (terminal_printf)
 #include "string.h"        // Kernel string manipulation (strcmp, strlen, etc.)
 #include "types.h"         // Core OS types (ssize_t, off_t, uintptr_t)
 #include "sys_file.h"      // Standard file flags (O_*, SEEK_*)
 #include "fs_errno.h"      // Filesystem error codes (FS_ERR_*, E*)
 #include "fs_limits.h"     // Filesystem limits (MAX_PATH_LEN)
 #include "mount.h"         // Mount point structure (mount_t)
 #include "mount_table.h"   // Mount table management API
 #include "spinlock.h"      // Spinlock primitives
 #include <libc/limits.h>   // Standard limits (LONG_MAX, LONG_MIN)
 #include <libc/stddef.h>   // Standard definitions (NULL, size_t)
 #include <libc/stdbool.h>  // Boolean type (bool)
 #include <libc/stdarg.h>   // Variable arguments (for printf-style functions)
 #include "assert.h"        // Kernel assertion macros (KERNEL_ASSERT)
 #include "serial.h"        // Serial port logging for low-level debug
 
 /* Standard SEEK_SET, SEEK_CUR, SEEK_END definitions for lseek. */
 #ifndef SEEK_SET
 #define SEEK_SET 0
 #endif
 #ifndef SEEK_CUR
 #define SEEK_CUR 1
 #endif
 #ifndef SEEK_END
 #define SEEK_END 2
 #endif
 
 /* Maximum file offset value. */
 #ifndef OFF_T_MAX
 #define OFF_T_MAX LONG_MAX
 #endif
 #ifndef OFF_T_MIN
 #define OFF_T_MIN LONG_MIN
 #endif
 
 /* VFS Debug Logging Configuration */
 #ifndef VFS_DEBUG_LEVEL
 #define VFS_DEBUG_LEVEL 1 /* Default: Enable INFO logs. Set to 0 for none, >=2 for DEBUG. */
 #endif
 
 #if VFS_DEBUG_LEVEL >= 1
 #define VFS_LOG(fmt, ...) terminal_printf("[VFS INFO] " fmt "\n", ##__VA_ARGS__)
 #else
 #define VFS_LOG(fmt, ...) ((void)0)
 #endif
 #if VFS_DEBUG_LEVEL >= 2
 #define VFS_DEBUG_LOG(fmt, ...) terminal_printf("[VFS DEBUG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 #else
 #define VFS_DEBUG_LOG(fmt, ...) ((void)0)
 #endif
 #define VFS_WARN(fmt, ...) terminal_printf("[VFS WARN] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 #define VFS_ERROR(fmt, ...) terminal_printf("[VFS ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 
 
 /* --- Global VFS State --- */
 
 static vfs_driver_t *driver_list = NULL;    /* Head of the registered filesystem driver list. */
 static spinlock_t vfs_driver_lock;          /* Spinlock for synchronizing access to driver_list. */
 
 
 /* --- Forward Declarations for Static Helper Functions --- */
 
 static int check_driver_validity(vfs_driver_t *driver);
 static mount_t *find_best_mount_for_path(const char *path);
 static const char *get_relative_path(const char *path, mount_t *mnt);
 // add_mount_entry is now part of mount_table.c, not a static helper here.
 // static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv);
 static int vfs_mount_internal(const char *mp, const char *fs, const char *dev);
 static int vfs_unmount_internal(const char *mp);
 static int vfs_unmount_entry(mount_t *mnt);
 
 /*---------------------------------------------------------------------------
  * VFS Initialization and Driver Registration
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Initializes the VFS layer.
  * Sets up the driver list lock and initializes the mount table.
  * Must be called once during kernel boot before any VFS operations.
  */
 void vfs_init(void) {
     spinlock_init(&vfs_driver_lock);
     driver_list = NULL;
     mount_table_init(); // Initialize the global mount table manager
     VFS_LOG("Virtual File System initialized");
 }
 
 /**
  * @brief Validates a filesystem driver structure before registration.
  * Checks for presence of essential function pointers and a valid name.
  * @param driver Pointer to the driver structure to validate.
  * @return FS_SUCCESS if valid, or a negative FS_ERR_* code on failure.
  */
 static int check_driver_validity(vfs_driver_t *driver) {
     KERNEL_ASSERT(driver != NULL, "check_driver_validity: driver cannot be NULL");
     if (!driver->fs_name || driver->fs_name[0] == '\0') {
         VFS_ERROR("Driver registration check failed: Missing or empty fs_name");
         return -FS_ERR_INVALID_PARAM;
     }
     // Check essential function pointers
     if (!driver->mount) { VFS_ERROR("Driver '%s' check failed: Missing required 'mount'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->open)  { VFS_ERROR("Driver '%s' check failed: Missing required 'open'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->close) { VFS_ERROR("Driver '%s' check failed: Missing required 'close'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->read)  { VFS_ERROR("Driver '%s' check failed: Missing required 'read'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->write) { VFS_ERROR("Driver '%s' check failed: Missing required 'write'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->lseek) { VFS_ERROR("Driver '%s' check failed: Missing required 'lseek'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
 
     // Log warnings for missing optional functions (not fatal for registration)
     if (!driver->readdir) VFS_WARN("Driver '%s' info: Missing optional 'readdir'", driver->fs_name);
     if (!driver->unlink)  VFS_WARN("Driver '%s' info: Missing optional 'unlink'", driver->fs_name);
     if (!driver->unmount) VFS_WARN("Driver '%s' info: Missing optional 'unmount'", driver->fs_name);
     // Add checks for other optional ops (mkdir, rmdir, stat) if they become part of vfs_driver_t
 
     return FS_SUCCESS;
 }
 
 /**
  * @brief Registers a filesystem driver with the VFS.
  * Adds the driver to a global list, ensuring no duplicates by name.
  * @param driver Pointer to the driver structure to register.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code on failure.
  */
 int vfs_register_driver(vfs_driver_t *driver) {
     int check_result = check_driver_validity(driver);
     if (check_result != FS_SUCCESS) {
         return check_result;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     // Check for duplicate registration by name
     vfs_driver_t *current = driver_list;
     while (current) {
         if (current->fs_name && strcmp(current->fs_name, driver->fs_name) == 0) {
             spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
             VFS_ERROR("Driver '%s' already registered", driver->fs_name);
             return -FS_ERR_FILE_EXISTS; // Or a more specific "already registered" error
         }
         current = current->next;
     }
 
     // Add to head of the driver list
     driver->next = driver_list;
     driver_list = driver;
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     VFS_LOG("Registered filesystem driver: %s", driver->fs_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Unregisters a filesystem driver from the VFS.
  * Removes the driver from the global list.
  * @param driver Pointer to the driver structure to unregister.
  * @return FS_SUCCESS if found and removed, or a negative FS_ERR_* code.
  */
 int vfs_unregister_driver(vfs_driver_t *driver) {
     if (!driver || !driver->fs_name) {
         VFS_ERROR("Attempted to unregister NULL or invalid driver");
         return -FS_ERR_INVALID_PARAM;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     vfs_driver_t **prev_next_ptr = &driver_list;
     vfs_driver_t *curr = driver_list;
     bool found = false;
 
     while (curr) {
         if (curr == driver) {
             *prev_next_ptr = curr->next; // Unlink
             found = true;
             break;
         }
         prev_next_ptr = &curr->next;
         curr = curr->next;
     }
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     if (found) {
         VFS_LOG("Unregistered driver: %s", driver->fs_name);
         driver->next = NULL; // Clear the next pointer of the unregistered driver
         return FS_SUCCESS;
     } else {
         VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
         return -FS_ERR_NOT_FOUND;
     }
 }
 
 /**
  * @brief Finds a registered filesystem driver by its name.
  * @param fs_name The name of the filesystem driver (e.g., "FAT32", "ext2").
  * @return Pointer to the found driver structure, or NULL if not found.
  */
 vfs_driver_t *vfs_get_driver(const char *fs_name) {
     if (!fs_name || fs_name[0] == '\0') {
         VFS_ERROR("NULL or empty fs_name passed to vfs_get_driver");
         return NULL;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     vfs_driver_t *curr = driver_list;
     vfs_driver_t *found_driver = NULL;
     while (curr) {
         if (curr->fs_name && strcmp(curr->fs_name, fs_name) == 0) {
             found_driver = curr;
             break;
         }
         curr = curr->next;
     }
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     if (!found_driver) {
        VFS_DEBUG_LOG("Driver '%s' not found", fs_name);
     }
     return found_driver;
 }
 
 /**
  * @brief Lists all registered filesystem drivers to the kernel log.
  * Useful for debugging.
  */
 void vfs_list_drivers(void) {
     VFS_LOG("Registered filesystem drivers:");
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
     if (!driver_list) {
         VFS_LOG("  (none)");
     } else {
         vfs_driver_t *curr = driver_list;
         int count = 0;
         while (curr) {
             VFS_LOG("  %d: %s", ++count, curr->fs_name ? curr->fs_name : "[INVALID NAME]");
             curr = curr->next;
         }
         if (count == 0) { VFS_LOG("  (List head not null, but no drivers found - list corrupted?)"); }
         else { VFS_LOG("Total drivers: %d", count); }
     }
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 }
 
 /*---------------------------------------------------------------------------
  * Mount Table Helpers & Path Resolution
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Finds the most specific (longest matching prefix) mount_t entry
  * for a given absolute path.
  * @param path The absolute path to resolve.
  * @return Pointer to the best matching mount_t entry, or NULL if no suitable mount found.
  */
 static mount_t *find_best_mount_for_path(const char *path) {
     KERNEL_ASSERT(path && path[0] == '/', "find_best_mount_for_path: Invalid or non-absolute path");
     VFS_DEBUG_LOG("Resolving path: '%s'", path);
 
     mount_t *best_match = NULL;
     size_t best_len = 0;
     // Iterating mount_table_get_head() directly is okay for read-only if modifications are infrequent
     // or if mount_table_get_head() itself handles appropriate locking for safe traversal start.
     mount_t *curr = mount_table_get_head();
 
     while (curr) {
         KERNEL_ASSERT(curr->mount_point && curr->mount_point[0] == '/', "Invalid mount entry in list (non-absolute or NULL mount_point)");
         size_t current_mount_point_len = strlen(curr->mount_point);
         VFS_DEBUG_LOG("  Checking against mount: '%s' (len %lu)", curr->mount_point, (unsigned long)current_mount_point_len);
 
         if (strncmp(path, curr->mount_point, current_mount_point_len) == 0) {
             // Potential match. Check if it's an exact match or a subdirectory.
             bool is_exact_match = (path[current_mount_point_len] == '\0');
             bool is_subdir_match = (path[current_mount_point_len] == '/');
             // Special case for root mount "/": if mount_point is "/", any path is a "subdir" or exact.
             bool is_root_mount_and_path_is_subdir = (current_mount_point_len == 1 && !is_exact_match);
 
             if (is_exact_match || is_subdir_match || is_root_mount_and_path_is_subdir) {
                 if (current_mount_point_len >= best_len) { // Prefer longer (more specific) match
                     VFS_DEBUG_LOG("    -> New best match: '%s' (len %lu >= %lu)",
                                   curr->mount_point, (unsigned long)current_mount_point_len, (unsigned long)best_len);
                     best_match = curr;
                     best_len = current_mount_point_len;
                 }
             }
         }
         curr = curr->next;
     }
 
     if (best_match) { VFS_DEBUG_LOG("Resolved to mount point: '%s'", best_match->mount_point); }
     else { VFS_LOG("No suitable mount point found for path '%s'.", path); }
     return best_match;
  }
 
 /**
  * @brief Calculates the path relative to a given mount point.
  * Example: path="/mnt/data/file.txt", mnt->mount_point="/mnt/data" -> returns "/file.txt"
  * path="/file.txt", mnt->mount_point="/" -> returns "/file.txt"
  * @param path The absolute path.
  * @param mnt Pointer to the mount_t structure representing the mount point.
  * @return Pointer to the relative path segment within 'path', or "/" if path is the mount point itself.
  * Returns NULL on error (should not happen if inputs are valid).
  */
  static const char *get_relative_path(const char *path, mount_t *mnt) {
     KERNEL_ASSERT(path && mnt && mnt->mount_point, "get_relative_path: Invalid input");
     size_t mount_point_len = strlen(mnt->mount_point);
     KERNEL_ASSERT(strncmp(path, mnt->mount_point, mount_point_len) == 0, "Path does not start with mount point as expected");
 
     const char *relative_path_start = path + mount_point_len;
 
     // If mount_point is "/" (len 1), relative_path_start points to the rest of 'path'.
     // If path is also "/", relative_path_start is '\0', so return "/".
     // Otherwise, relative_path_start is correct (e.g. for "/foo", it's "foo", which is correct for driver).
     if (mount_point_len == 1 && mnt->mount_point[0] == '/') {
         return (*relative_path_start == '\0') ? "/" : relative_path_start;
     }
 
     // For other mount points (e.g. /mnt/data):
     // If path is "/mnt/data", relative_path_start is '\0', return "/".
     // If path is "/mnt/data/file.txt", relative_path_start is "/file.txt", return it.
     if (*relative_path_start == '\0') return "/";
     // No leading '/' means it's part of the mount point itself (e.g. path "/mnt" on mnt "/mnt")
     // If it starts with '/', it's already a relative path from that mount.
     if (*relative_path_start == '/') return relative_path_start;
 
 
     // This case should ideally not be reached if path resolution and mount point structure are correct.
     // It might indicate an issue like path="/mnt/datafile" with mount_point="/mnt/data"
     // where there's no separator. This is typically not a valid VFS path structure.
     VFS_ERROR("Path '%s' is not a clean subpath of mount '%s'", path, mnt->mount_point);
     return NULL;
  }
 
 /*---------------------------------------------------------------------------
  * Mount / Unmount Operations
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Internal VFS implementation for mounting a filesystem.
  * @param mp Mount point path (e.g., "/").
  * @param fs Filesystem type name (e.g., "FAT32").
  * @param dev Device identifier string (e.g., "hda").
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
 static int vfs_mount_internal(const char *mp, const char *fs_type_name, const char *dev_name) {
     KERNEL_ASSERT(mp && fs_type_name && dev_name, "vfs_mount_internal: Invalid NULL parameter");
     KERNEL_ASSERT(mp[0] == '/', "Mount point must be absolute");
     VFS_LOG("Mounting dev '%s' (type '%s') onto '%s'", dev_name, fs_type_name, mp);
 
     if (mount_table_find(mp) != NULL) { // mount_table_find handles its own locking
         VFS_ERROR("Mount point '%s' already in use.", mp);
         return -FS_ERR_BUSY;
     }
 
     vfs_driver_t *driver = vfs_get_driver(fs_type_name); // vfs_get_driver handles its own locking
     if (!driver) { VFS_ERROR("Filesystem driver '%s' not found.", fs_type_name); return -FS_ERR_NOT_FOUND; }
     if (!driver->mount) { VFS_ERROR("Driver '%s' does not support mount operation.", fs_type_name); return -FS_ERR_NOT_SUPPORTED; }
 
     VFS_DEBUG_LOG("Calling driver '%s' ->mount() for device '%s'", fs_type_name, dev_name);
     void *fs_context = driver->mount(dev_name); // Driver's mount function
     if (!fs_context) {
         VFS_ERROR("Driver '%s' mount failed for device '%s'.", fs_type_name, dev_name);
         return -FS_ERR_MOUNT; // Generic mount error
     }
     VFS_DEBUG_LOG("Driver mount successful, fs_context=%p", fs_context);
 
     // Use mount_filesystem from mount.c to handle mount_t creation and table addition
     // This avoids duplicating kmalloc and string copy logic here.
     // Note: mount_filesystem itself calls mount_table_add.
     // This function is now a wrapper around the public mount_filesystem.
     // For direct internal use, one might call a lower-level add_mount_entry.
     // Re-evaluating: vfs_mount_internal is called by vfs_mount_root.
     // The public mount_filesystem (in mount.c) also calls vfs_get_driver and driver->mount.
     // To avoid circular calls or redundant logic, vfs_mount_internal should directly
     // manage adding the entry to the mount_table if it's the core internal logic.
     // Let's assume mount_table_add is the primitive for adding an already prepared mount_t.
 
     // Allocate mount_t and copy mount_point string
     size_t mp_len = strlen(mp);
     char *mp_copy = (char *)kmalloc(mp_len + 1);
     if (!mp_copy) {
         VFS_ERROR("Failed kmalloc for mount point string copy ('%s')", mp);
         if (driver->unmount) driver->unmount(fs_context); // Attempt cleanup
         return -FS_ERR_OUT_OF_MEMORY;
     }
     strcpy(mp_copy, mp);
 
     mount_t *new_mount = (mount_t *)kmalloc(sizeof(mount_t));
     if (!new_mount) {
         kfree(mp_copy);
         VFS_ERROR("Failed kmalloc for mount_t for '%s'", mp);
         if (driver->unmount) driver->unmount(fs_context); // Attempt cleanup
         return -FS_ERR_OUT_OF_MEMORY;
     }
     new_mount->mount_point = mp_copy;
     new_mount->fs_name = driver->fs_name; // Persistent string from driver struct
     new_mount->fs_context = fs_context;
     new_mount->next = NULL; // Will be set by mount_table_add
 
     int add_result = mount_table_add(new_mount); // mount_table_add handles locking
     if (add_result != FS_SUCCESS) {
         VFS_ERROR("Failed to add to mount table for '%s' (err %d). Cleaning up.", mp, add_result);
         kfree(mp_copy);
         kfree(new_mount);
         if (driver->unmount) driver->unmount(fs_context);
         return add_result;
     }
 
     VFS_LOG("Mounted '%s' (dev '%s') on '%s' successfully.", fs_type_name, dev_name, mp);
     return FS_SUCCESS;
  }
 
 /**
  * @brief Internal VFS implementation for unmounting a filesystem.
  * @param mp Mount point path (e.g., "/").
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
 static int vfs_unmount_internal(const char *mp) {
     KERNEL_ASSERT(mp && mp[0] == '/', "vfs_unmount_internal: Invalid or non-absolute mount point");
     VFS_LOG("Unmount request for: '%s'", mp);
 
     mount_t *mnt = mount_table_find(mp); // mount_table_find handles its own locking
     if (!mnt) { VFS_ERROR("Mount point '%s' not found for unmount.", mp); return -FS_ERR_NOT_FOUND; }
 
     // Check for nested mounts that would prevent unmounting this one.
     // This check is simplified and potentially racy without holding a global VFS lock
     // that covers both mount_table_get_head and mount_table_remove.
     // For a robust SMP system, this check needs to be more carefully synchronized.
     bool busy = false;
     size_t mp_len = strlen(mp);
     mount_t *iter = mount_table_get_head(); // Potentially racy read
     while(iter) {
         if (iter != mnt && iter->mount_point && iter->mount_point[0] == '/') {
             size_t iter_len = strlen(iter->mount_point);
             if (iter_len > mp_len && strncmp(iter->mount_point, mp, mp_len) == 0) {
                 // Check if 'iter' is a direct sub-mount of 'mp'
                 if ((mp_len == 1 && mp[0] == '/') || iter->mount_point[mp_len] == '/') {
                     VFS_ERROR("Cannot unmount '%s': Busy (nested mount: '%s').", mp, iter->mount_point);
                     busy = true; break;
                 }
             }
         }
         iter = iter->next;
     }
     if (busy) { return -FS_ERR_BUSY; }
 
     return vfs_unmount_entry(mnt); // Perform the actual unmount logic
  }
 
 /**
  * @brief Core logic to unmount a filesystem, given its mount_t entry.
  * Calls the driver's unmount and removes the entry from the mount table.
  * @param mnt Pointer to the mount_t structure to unmount.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
 static int vfs_unmount_entry(mount_t *mnt) {
     KERNEL_ASSERT(mnt && mnt->fs_name && mnt->mount_point && mnt->fs_context, "vfs_unmount_entry: Invalid mount_t structure");
     VFS_LOG("Performing unmount for '%s' (FS: %s)", mnt->mount_point, mnt->fs_name);
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name); // Handles its own locking
     int driver_unmount_status = FS_SUCCESS;
 
     if (!driver) {
         VFS_ERROR("CRITICAL: Driver '%s' for mount '%s' disappeared during unmount!", mnt->fs_name, mnt->mount_point);
         driver_unmount_status = -FS_ERR_INTERNAL; // Filesystem driver gone
     } else if (driver->unmount) {
         VFS_DEBUG_LOG("Calling driver '%s' ->unmount() for context %p", mnt->fs_name, mnt->fs_context);
         driver_unmount_status = driver->unmount(mnt->fs_context);
         if (driver_unmount_status != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' unmount failed for '%s' (context %p, err %d)",
                       mnt->fs_name, mnt->mount_point, mnt->fs_context, driver_unmount_status);
         } else {
              VFS_DEBUG_LOG("Driver unmount successful for '%s'", mnt->mount_point);
         }
     } else {
         VFS_LOG("Driver '%s' for '%s' has no unmount function. FS context %p may leak.",
                 mnt->fs_name, mnt->mount_point, mnt->fs_context);
         // Not necessarily an error if unmount is optional and driver doesn't need cleanup
     }
 
     // Regardless of driver unmount status, attempt to remove from table.
     // mount_table_remove handles freeing mnt and mnt->mount_point string.
     char* mp_to_remove_name = (char*)mnt->mount_point; // Cache name for logging before mnt is freed
     VFS_DEBUG_LOG("Removing '%s' from mount table.", mp_to_remove_name);
     int table_remove_status = mount_table_remove(mp_to_remove_name); // Handles its own locking
 
     if (table_remove_status != FS_SUCCESS) {
          VFS_ERROR("CRITICAL: Failed to remove '%s' from mount table (err %d) after driver unmount attempt!",
                    mp_to_remove_name, table_remove_status);
          // Return the more critical error (table inconsistency vs. driver error)
          return (driver_unmount_status != FS_SUCCESS && driver_unmount_status != -FS_ERR_INTERNAL) ? driver_unmount_status : table_remove_status;
     }
 
     VFS_LOG("Unmounted and removed '%s' from table.", mp_to_remove_name);
     return driver_unmount_status; // Return the outcome of the driver's unmount operation
 }
 
 /**
  * @brief Mounts the root filesystem.
  * Wrapper around vfs_mount_internal, ensuring mount point is "/".
  * @param mp Mount point (must be "/").
  * @param fs_type Filesystem type name.
  * @param dev Device identifier.
  * @return FS_SUCCESS or negative error code.
  */
 int vfs_mount_root(const char *mp, const char *fs_type, const char *dev) {
     VFS_LOG("Request to mount root: dev '%s' (type '%s') on '%s'", dev, fs_type, mp);
     if (strcmp(mp, "/") != 0) {
         VFS_ERROR("Root mount point must be '/' (got '%s').", mp);
         return -FS_ERR_INVALID_PARAM;
     }
     return vfs_mount_internal(mp, fs_type, dev);
 }
 
 /**
  * @brief Unmounts the root filesystem.
  * Wrapper around vfs_unmount_internal for "/".
  * @return FS_SUCCESS or negative error code.
  */
 int vfs_unmount_root(void) {
     VFS_LOG("Request to unmount root ('/').");
     return vfs_unmount_internal("/");
 }
 
 /**
  * @brief Lists all currently mounted filesystems to the kernel log.
  */
 void vfs_list_mounts(void) {
     VFS_LOG("--- Mount Table Listing ---");
     mount_table_list(); // Delegates to mount_table module
     VFS_LOG("--- End Mount Table ---");
 }
 
 /**
  * @brief Shuts down the VFS layer.
  * Attempts to unmount all filesystems and clear the driver list.
  * @return FS_SUCCESS if all unmounts succeed, or the first error encountered.
  */
 int vfs_shutdown(void) {
     VFS_LOG("Shutting down VFS layer...");
     int final_result = FS_SUCCESS;
     int unmount_attempts = 0;
     const int max_attempts = 100; // Safeguard against ununmountable FS causing infinite loop
     mount_t *current_mount;
 
     // Iteratively unmount. mount_table_get_head() gives the current head.
     // vfs_unmount_entry() will remove the entry, so next call to get_head() gets the new head.
     while ((current_mount = mount_table_get_head()) != NULL && unmount_attempts < max_attempts) {
         unmount_attempts++;
         // Create a temporary copy of the mount point string for logging,
         // as current_mount and its contents will be freed by vfs_unmount_entry.
         char mp_copy[MAX_PATH_LEN];
         if (current_mount->mount_point) {
             strncpy(mp_copy, current_mount->mount_point, MAX_PATH_LEN - 1);
             mp_copy[MAX_PATH_LEN - 1] = '\0';
         } else {
             strcpy(mp_copy, "[INVALID/NULL Mount Point]"); // Should not happen
             VFS_ERROR("VFS Shutdown: Encountered mount entry with NULL mount_point!");
         }
 
         VFS_LOG("Attempting shutdown unmount for: '%s'", mp_copy);
         int result = vfs_unmount_entry(current_mount); // This will call driver unmount and remove from table
         if (result != FS_SUCCESS) {
             VFS_ERROR("Failed to unmount '%s' during shutdown (error %d).", mp_copy, result);
             if (final_result == FS_SUCCESS) final_result = result; // Store first error
             // If it failed to remove, it might still be in the list, potentially causing loop.
             // The max_attempts check is crucial here.
         }
     }
 
     if (unmount_attempts >= max_attempts && mount_table_get_head() != NULL) {
         VFS_ERROR("VFS Shutdown: Reached max unmount attempts (%d) but mounts still exist!", max_attempts);
         if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY;
         mount_table_list(); // Log remaining mounts
     }
 
     // Clear the VFS driver list
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
     driver_list = NULL;
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     if (final_result == FS_SUCCESS) { VFS_LOG("VFS shutdown complete."); }
     else { VFS_ERROR("VFS shutdown encountered errors (first error: %d).", final_result); }
     return final_result;
 }
 
 /*---------------------------------------------------------------------------
  * Core File Operations (Dispatch to Drivers)
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Opens a file or directory.
  * Resolves path to a mounted filesystem and calls the driver's open.
  * Initializes a file_t handle with a per-file lock.
  * @param path Absolute path to the file/directory.
  * @param flags Open flags (O_RDONLY, O_CREAT, etc.).
  * @return Pointer to a new file_t structure on success, NULL on failure.
  */
 file_t *vfs_open(const char *path, int flags) {
     serial_write("[vfs_open] Path='"); serial_write(path ? path : "NULL");
     serial_write("', Flags=0x"); serial_print_hex((uint32_t)flags); serial_write("\n");
 
     if (!path || path[0] != '/') {
         VFS_ERROR("vfs_open: Invalid or non-absolute path '%s'", path ? path : "NULL");
         return NULL;
     }
 
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { VFS_ERROR("vfs_open: No mount point found for path '%s'", path); return NULL; }
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { VFS_ERROR("vfs_open: Driver '%s' not found for mount '%s'", mnt->fs_name, mnt->mount_point); return NULL; }
 
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("vfs_open: Could not determine relative path for '%s' on mount '%s'", path, mnt->mount_point); return NULL; }
 
     serial_write("[vfs_open] Resolved: mount='"); serial_write(mnt->mount_point);
     serial_write("', driver='"); serial_write(driver->fs_name);
     serial_write("', rel_path='"); serial_write(relative_path); serial_write("'\n");
 
     if (!driver->open) { VFS_ERROR("vfs_open: Driver '%s' does not support open operation", driver->fs_name); return NULL; }
 
     serial_write("[vfs_open] Calling driver->open...\n");
     vnode_t *node = driver->open(mnt->fs_context, relative_path, flags); // Driver's open
     serial_write("[vfs_open] driver->open returned vnode="); serial_print_hex((uintptr_t)node); serial_write("\n");
 
     if (!node) {
         VFS_DEBUG_LOG("vfs_open: Driver open failed for '%s' on driver '%s'", relative_path, driver->fs_name);
         return NULL; // Driver should log specific error
     }
     // Ensure the driver correctly set the fs_driver field in the vnode
     if (node->fs_driver != driver) {
         VFS_ERROR("vfs_open: Driver '%s' returned vnode %p with mismatched fs_driver %p!",
                   driver->fs_name, node, node->fs_driver);
         // This is a driver bug. Attempt to clean up what the driver might have allocated.
         if (driver->close && node->data) { // Attempt a "close" on the potentially problematic node
             // Create a temporary file_t to pass to driver's close, if it expects one.
             // This is tricky as the vnode is not fully formed from VFS perspective.
             // Best effort cleanup.
             file_t temp_file_for_close = { .vnode = node, .flags = 0, .offset = 0 };
             driver->close(&temp_file_for_close);
         }
         kfree(node); // Free the vnode shell if driver didn't
         return NULL;
     }
 
     file_t *file = (file_t *)kmalloc(sizeof(file_t));
     if (!file) {
         VFS_ERROR("vfs_open: kmalloc failed for file_t structure");
         // Need to close the vnode that the driver successfully opened
         if (driver->close) {
             // Similar to above, create a temporary file_t for the driver's close
             file_t temp_file_for_close = { .vnode = node, .flags = flags, .offset = 0 };
             driver->close(&temp_file_for_close); // Driver is responsible for vnode->data
         }
         kfree(node); // Free the vnode shell itself
         return NULL;
     }
 
     file->vnode = node;
     file->flags = flags;
     file->offset = 0;
     spinlock_init(&file->lock); // Initialize the per-file lock
 
     serial_write("[vfs_open] Success. file="); serial_print_hex((uintptr_t)file);
     serial_write(", vnode="); serial_print_hex((uintptr_t)node);
     serial_write(", vnode->data="); serial_print_hex((uintptr_t)(node ? node->data : 0)); serial_write("\n");
     return file;
 }
 
 /**
  * @brief Closes an open file handle.
  * Calls the underlying driver's close operation and frees VFS resources.
  * @param file Pointer to the file_t structure to close.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
 int vfs_close(file_t *file) {
     if (!file) { VFS_ERROR("NULL file handle passed to vfs_close"); return -FS_ERR_INVALID_PARAM; }
     if (!file->vnode) { VFS_ERROR("vfs_close: File handle %p has NULL vnode!", file); kfree(file); return -FS_ERR_BAD_F; }
     if (!file->vnode->fs_driver) {
         VFS_ERROR("vfs_close: Vnode %p (from file %p) has NULL fs_driver!", file->vnode, file);
         kfree(file->vnode); // Free vnode shell
         kfree(file);        // Free file handle
         return -FS_ERR_BAD_F; // Or internal error
     }
 
     vfs_driver_t* driver = file->vnode->fs_driver;
     VFS_DEBUG_LOG("Closing file (vnode: %p, driver: %s)", file->vnode, driver->fs_name ? driver->fs_name : "[N/A]");
 
     int result = FS_SUCCESS;
     // The file->lock is for offset/VFS state. The driver's close might have its own internal locking
     // for filesystem-level structures. Generally, VFS file lock isn't held across driver->close.
 
     if (driver->close) {
         result = driver->close(file); // Driver is responsible for cleaning up vnode->data
         if (result != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' close operation failed (err %d) for file (vnode %p)",
                       driver->fs_name, result, file->vnode);
         }
     } else {
         VFS_WARN("Driver '%s' has no close function. Potential resource leak for vnode->data %p.",
                  driver->fs_name, file->vnode->data);
         // If no driver close, vnode->data might be leaked if driver allocated it.
     }
 
     // VFS layer frees its own structures associated with this open instance.
     kfree(file->vnode); // Free the vnode structure itself
     kfree(file);        // Free the file_t handle
 
     return result; // Return the result from the driver's close operation
 }
 
 /**
  * @brief Reads data from an open file.
  * Acquires the file's lock, calls the driver's read, and updates file offset.
  * @param file Pointer to the file_t structure.
  * @param buf Buffer to store read data.
  * @param len Number of bytes to read.
  * @return Number of bytes read, 0 on EOF, or negative FS_ERR_* on error.
  */
 int vfs_read(file_t *file, void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) { return -FS_ERR_BAD_F; }
     if (!buf && len > 0) { VFS_ERROR("vfs_read: NULL buffer with len > 0"); return -FS_ERR_INVALID_PARAM; }
     if (len == 0) return 0;
     if (!file->vnode->fs_driver->read) { VFS_ERROR("vfs_read: Driver does not support read"); return -FS_ERR_NOT_SUPPORTED; }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock); // Lock the file handle
 
     VFS_DEBUG_LOG("Read: file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
     int bytes_read = file->vnode->fs_driver->read(file, buf, len); // Driver uses/updates file->offset
 
     if (bytes_read > 0) {
         // Driver should have updated file->offset if it was used.
         // If driver does not update file->offset, VFS must do it here based on bytes_read.
         // Assuming driver updates file->offset directly as per common VFS patterns.
         // If not, add: file->offset += bytes_read; (after overflow check)
         VFS_DEBUG_LOG("Read OK: %d bytes, new offset presumed updated by driver to %ld", bytes_read, (long)file->offset);
     } else if (bytes_read == 0) {
         VFS_DEBUG_LOG("Read EOF: file=%p, offset=%ld", file, (long)file->offset);
     } else { // bytes_read < 0
         VFS_ERROR("Read FAIL: file=%p, driver error %d", file, bytes_read);
     }
 
     spinlock_release_irqrestore(&file->lock, irq_flags); // Unlock
     return bytes_read;
 }
 
 /**
  * @brief Writes data to an open file.
  * Acquires the file's lock, calls the driver's write, and updates file offset.
  * @param file Pointer to the file_t structure.
  * @param buf Buffer containing data to write.
  * @param len Number of bytes to write.
  * @return Number of bytes written, or negative FS_ERR_* on error.
  */
 int vfs_write(file_t *file, const void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) { return -FS_ERR_BAD_F; }
     if (!buf && len > 0) { VFS_ERROR("vfs_write: NULL buffer with len > 0"); return -FS_ERR_INVALID_PARAM; }
     if (len == 0) return 0;
 
     int access_mode = file->flags & O_ACCMODE;
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         VFS_ERROR("vfs_write: File not opened for writing (flags: 0x%x)", file->flags);
         return -FS_ERR_PERMISSION_DENIED;
     }
     if (!file->vnode->fs_driver->write) { VFS_ERROR("vfs_write: Driver does not support write"); return -FS_ERR_NOT_SUPPORTED; }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock); // Lock
 
     VFS_DEBUG_LOG("Write: file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
     int bytes_written = file->vnode->fs_driver->write(file, buf, len); // Driver uses/updates file->offset
 
     if (bytes_written > 0) {
         // Assuming driver updates file->offset. If not, add:
         // file->offset += bytes_written; (after overflow check)
         VFS_DEBUG_LOG("Write OK: %d bytes, new offset presumed updated by driver to %ld", bytes_written, (long)file->offset);
     } else if (bytes_written == 0 && len > 0) {
         VFS_WARN("Write 0 bytes: file=%p (requested %lu), possible disk full or other issue.", file, (unsigned long)len);
     } else if (bytes_written < 0) {
         VFS_ERROR("Write FAIL: file=%p, driver error %d", file, bytes_written);
     }
 
     spinlock_release_irqrestore(&file->lock, irq_flags); // Unlock
     return bytes_written;
 }
 
 /**
  * @brief Repositions the read/write offset of an open file.
  * Acquires the file's lock, calls the driver's lseek, and updates VFS file offset.
  * @param file Pointer to the file_t structure.
  * @param offset Offset value.
  * @param whence Reference point (SEEK_SET, SEEK_CUR, SEEK_END).
  * @return Resulting offset from file start on success, or negative FS_ERR_* on error.
  */
 off_t vfs_lseek(file_t *file, off_t offset, int whence) {
     if (!file || !file->vnode || !file->vnode->fs_driver) { return (off_t)-FS_ERR_BAD_F; }
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         VFS_ERROR("vfs_lseek: Invalid whence value (%d)", whence);
         return (off_t)-FS_ERR_INVALID_PARAM;
     }
     if (!file->vnode->fs_driver->lseek) { VFS_ERROR("vfs_lseek: Driver does not support lseek"); return (off_t)-FS_ERR_NOT_SUPPORTED; }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock); // Lock
 
     VFS_DEBUG_LOG("Lseek: file=%p, current_offset=%ld, req_offset=%ld, whence=%d",
                   file, (long)file->offset, (long)offset, whence);
 
     // The driver's lseek is responsible for validating the new offset against file size
     // and returning the new absolute offset or an error.
     // The VFS file->offset is passed to the driver, which might use it for SEEK_CUR.
     off_t new_absolute_offset = file->vnode->fs_driver->lseek(file, offset, whence);
 
     if (new_absolute_offset >= 0) {
         file->offset = new_absolute_offset; // Update VFS offset if driver call was successful
         VFS_DEBUG_LOG("Lseek OK: file=%p, new absolute offset=%ld", file, (long)new_absolute_offset);
     } else { // new_absolute_offset < 0 (error from driver)
         VFS_ERROR("Lseek FAIL: file=%p, driver error %ld", file, (long)new_absolute_offset);
     }
 
     spinlock_release_irqrestore(&file->lock, irq_flags); // Unlock
     return new_absolute_offset; // Return result from driver (new offset or error code)
 }
 
 /**
  * @brief Reads a directory entry from an open directory handle.
  * @param dir_file Open file_t handle representing the directory.
  * @param d_entry_out Pointer to VFS `struct dirent` to populate.
  * @param entry_index Logical index of the directory entry to retrieve (0-based).
  * @return FS_SUCCESS on success, -FS_ERR_NOT_FOUND at end of directory, or other negative error.
  */
 int vfs_readdir(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index) {
     if (!dir_file || !d_entry_out) { return -FS_ERR_INVALID_PARAM; }
     if (!dir_file->vnode || !dir_file->vnode->fs_driver) { return -FS_ERR_BAD_F; }
     // A robust check would involve checking if the vnode represents a directory,
     // possibly via a flag in vnode or file_t, or a type field in vnode->data.
     // if (!IS_DIRECTORY(dir_file->vnode)) return -FS_ERR_NOT_A_DIRECTORY;
 
     if (!dir_file->vnode->fs_driver->readdir) {
         VFS_ERROR("vfs_readdir: Driver does not support readdir operation.");
         return -FS_ERR_NOT_SUPPORTED;
     }
 
     // Locking for readdir is complex. If readdir updates state in file_t (like an internal offset),
     // file->lock should be used. If readdir is purely based on entry_index and filesystem state,
     // then only filesystem-level locks (within the driver) might be needed.
     // Assuming readdir might use/update file_t state for sequential reads if entry_index is progressive.
     uintptr_t irq_flags = spinlock_acquire_irqsave(&dir_file->lock);
 
     VFS_DEBUG_LOG("Readdir: dir_file=%p, index=%lu", dir_file, (unsigned long)entry_index);
     int result = dir_file->vnode->fs_driver->readdir(dir_file, d_entry_out, entry_index);
 
     spinlock_release_irqrestore(&dir_file->lock, irq_flags);
 
     if (result == FS_SUCCESS) {
         VFS_DEBUG_LOG("Readdir success: index %lu, name='%s'", (unsigned long)entry_index, d_entry_out->d_name);
     } else if (result == -FS_ERR_NOT_FOUND) { // FS_ERR_EOF is also common for end of directory
         VFS_DEBUG_LOG("Readdir: End of directory or entry not found at index %lu", (unsigned long)entry_index);
     } else {
         VFS_ERROR("Readdir: Driver readdir failed (err %d) for index %lu", result, (unsigned long)entry_index);
     }
     return result;
 }
 
 /**
  * @brief Deletes a name (file or empty directory) from the filesystem.
  * Resolves path and calls the driver's unlink operation.
  * @param path Absolute path to the item to delete.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
 int vfs_unlink(const char *path) {
     if (!path || path[0] != '/') {
         VFS_ERROR("vfs_unlink: Invalid or non-absolute path '%s'", path ? path : "NULL");
         return -FS_ERR_INVALID_PARAM;
     }
     VFS_DEBUG_LOG("Unlink request for path: '%s'", path);
 
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { VFS_ERROR("vfs_unlink: No mount point for path '%s'", path); return -FS_ERR_NOT_FOUND; }
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { VFS_ERROR("vfs_unlink: Driver '%s' not found for mount '%s'", mnt->fs_name, mnt->mount_point); return -FS_ERR_INTERNAL; }
 
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("vfs_unlink: Failed to get relative path for '%s' on mount '%s'", path, mnt->mount_point); return -FS_ERR_INTERNAL; }
 
     if (!driver->unlink) { VFS_ERROR("vfs_unlink: Driver '%s' does not support unlink.", driver->fs_name); return -FS_ERR_NOT_SUPPORTED; }
 
     VFS_DEBUG_LOG("Unlink: Using mount '%s', driver '%s', relative_path '%s'", mnt->mount_point, driver->fs_name, relative_path);
 
     // Filesystem-level locking (e.g., directory lock) should be handled within the driver's unlink.
     int result = driver->unlink(mnt->fs_context, relative_path);
 
     if (result == FS_SUCCESS) {
         VFS_LOG("Unlink successful for '%s' (relative '%s' on driver '%s')", path, relative_path, driver->fs_name);
     } else {
         VFS_ERROR("Unlink failed for '%s' (driver '%s' error %d)", path, driver->fs_name, result);
     }
     return result;
 }
 
 
 /*---------------------------------------------------------------------------
  * VFS Status and Utility Functions
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Checks if the VFS is initialized and has a root filesystem mounted.
  * @return true if ready, false otherwise.
  */
 bool vfs_is_ready(void) {
     // A VFS is "ready" if the root ("/") is mounted.
     return (mount_table_find("/") != NULL); // mount_table_find handles its own locking
 }
 
 /**
  * @brief Performs a basic self-test of the VFS layer.
  * Attempts to open and close the root directory.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code.
  */
  int vfs_self_test(void) {
     VFS_LOG("Running VFS self-test...");
     if (!vfs_is_ready()) {
         VFS_ERROR("VFS self-test FAILED: VFS not ready (root '/' not mounted).");
         return -FS_ERR_NOT_INIT;
     }
 
     VFS_LOG("VFS self-test: Attempting to open root directory '/'...");
     file_t *root_dir = vfs_open("/", O_RDONLY); // Open read-only
     if (!root_dir) {
         VFS_ERROR("VFS self-test FAILED: vfs_open failed for root '/'.");
         return -FS_ERR_IO; // Or a more specific error if vfs_open provides one
     }
     VFS_LOG("VFS self-test: Root directory opened successfully (file_t: %p).", root_dir);
 
     // Optional: A minimal readdir test could be added here if desired.
     // struct dirent entry;
     // int read_res = vfs_readdir(root_dir, &entry, 0); // Try to read first entry
     // if (read_res != FS_SUCCESS && read_res != -FS_ERR_NOT_FOUND) { ... error ... }
 
     VFS_LOG("VFS self-test: Attempting to close root directory...");
     int close_result = vfs_close(root_dir);
     if (close_result != FS_SUCCESS) {
         VFS_ERROR("VFS self-test FAILED: vfs_close failed for root (code: %d).", close_result);
         return close_result;
     }
     VFS_LOG("VFS self-test: Root directory closed successfully.");
 
     VFS_LOG("VFS self-test PASSED.");
     return FS_SUCCESS;
  }
 
 /**
  * @brief Checks if a given path exists in the VFS.
  * Attempts to open the path read-only; existence is confirmed if open succeeds.
  * @param path Absolute path to check.
  * @return true if path exists and is accessible, false otherwise.
  */
  bool vfs_path_exists(const char *path) {
     if (!path) return false;
     VFS_DEBUG_LOG("Path exists check for: '%s'", path);
     file_t *file = vfs_open(path, O_RDONLY); // Attempt to open read-only
     if (!file) {
         VFS_DEBUG_LOG("Path '%s' does not exist or is not accessible (vfs_open failed).", path);
         return false;
     }
     VFS_DEBUG_LOG("Path '%s' exists (vfs_open succeeded).", path);
     vfs_close(file); // Close the handle, ignore close result for existence check
     return true;
  }
 
 /**
  * @brief Dumps VFS debug information (registered drivers, mount points) to the kernel log.
  */
  void vfs_debug_dump(void) {
     VFS_LOG("========== VFS DEBUG INFORMATION ==========");
     vfs_list_drivers();  // List all registered filesystem drivers
     vfs_list_mounts();   // List all current mount points
     VFS_LOG("==========================================");
  }
