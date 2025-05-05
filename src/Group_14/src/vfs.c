/**
 * @file vfs.c
 * @brief Virtual File System Implementation (Reviewed v1.2 - Build Fixes)
 * @version 1.2
 *
 * Provides a unified interface over different filesystem drivers.
 * Manages filesystem driver registration and mount points via the mount_table.
 * Implements core VFS operations (open, close, read, write, lseek, etc.)
 * by dispatching calls to the appropriate underlying filesystem driver.
 *
 * Changes v1.2:
 * - Included serial.h for serial logging functions.
 * - Replaced undeclared OFF_T_MAX with LONG_MAX from libc/limits.h.
 *
 * Key Aspects & Considerations:
 * - Mount Point Resolution: Uses longest prefix matching.
 * - Driver Management: Simple linked list for registered drivers.
 * - Locking: Uses spinlocks for global driver list and mount table modifications.
 * **Concurrency Limitation:** File operations (read, write, lseek) on individual
 * file handles (`file_t`) or vnodes (`vnode_t`) are NOT inherently thread-safe
 * by the VFS layer itself. Concurrent operations on the same file from
 * multiple threads/CPUs could lead to race conditions (e.g., on file->offset)
 * unless the underlying driver implements its own internal locking or the
 * calling code provides external synchronization. A production VFS often
 * requires per-file or per-vnode locking mechanisms.
 * - Error Handling: Primarily propagates errors from underlying drivers or
 * returns standard FS_ERR_* / POSIX errno codes.
 * - Missing Features: Lacks advanced features like permissions, ownership,
 * directory creation/deletion, symbolic links, stat, caching layers beyond
 * buffer cache, etc.
 */

 #include "vfs.h"           // Declares vfs_driver_t, file_t, vnode_t etc.
 #include "kmalloc.h"       // Kernel memory allocation
 #include "terminal.h"      // Kernel logging/printing
 #include "string.h"        // Kernel string functions (strcmp, strlen, strncpy, etc.)
 #include "types.h"         // Core types (ssize_t, off_t, etc.)
 #include "sys_file.h"      // SEEK_*, O_* flags
 #include "fs_errno.h"      // FS_ERR_* / POSIX errno codes
 #include "fs_limits.h"     // MAX_PATH_LEN definition
 #include "mount.h"         // mount_t definition
 #include "mount_table.h"   // Global mount table functions
 #include "spinlock.h"      // Spinlock definitions and functions
 #include <libc/limits.h>   // LONG_MAX, LONG_MIN etc. (Assumed available)
 #include <libc/stddef.h>   // NULL, size_t (Assumed available)
 #include <libc/stdbool.h>  // bool (Assumed available)
 #include <libc/stdarg.h>   // varargs for printf (Assumed available)
 #include "assert.h"        // KERNEL_ASSERT
 #include "serial.h"        // <<< ADDED: Include for serial_write, serial_print_hex
 
 /* Define SEEK macros if not already defined (should be in sys_file.h ideally) */
 #ifndef SEEK_SET
 #define SEEK_SET 0
 #endif
 #ifndef SEEK_CUR
 #define SEEK_CUR 1
 #endif
 #ifndef SEEK_END
 #define SEEK_END 2
 #endif
 
 /* Define OFF_T_MAX based on libc/limits.h LONG_MAX if not already defined */
 #ifndef OFF_T_MAX
 #define OFF_T_MAX LONG_MAX // <<< ADDED: Define based on off_t being long
 #endif
 #ifndef OFF_T_MIN
 #define OFF_T_MIN LONG_MIN // <<< ADDED: Define based on off_t being long
 #endif
 
 
 /* Debug macro - define VFS_DEBUG_LEVEL >= 1 to enable verbose logging */
 #ifndef VFS_DEBUG_LEVEL
 #define VFS_DEBUG_LEVEL 0 // 0: Errors only, 1: Info, 2: Detailed Trace
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
 
 
 /* --- Global State --- */
 
 // Linked list of registered filesystem drivers
 static vfs_driver_t *driver_list = NULL;
 
 // Spinlock to protect access to the driver_list
 static spinlock_t vfs_driver_lock;
 
 
 /* --- Forward Declarations --- */
 
 static int check_driver_validity(vfs_driver_t *driver);
 static mount_t *find_best_mount_for_path(const char *path);
 static const char *get_relative_path(const char *path, mount_t *mnt);
 static int vfs_mount_internal(const char *mp, const char *fs, const char *dev);
 static int vfs_unmount_internal(const char *mp);
 static int vfs_unmount_entry(mount_t *mnt);
 
 /*---------------------------------------------------------------------------
  * VFS Initialization and Driver Registration
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Initializes the VFS layer. Must be called once during kernel boot.
  */
 void vfs_init(void) {
     spinlock_init(&vfs_driver_lock);
     driver_list = NULL;
     mount_table_init(); // Initialize the separate mount table manager
     VFS_LOG("Virtual File System initialized");
 }
 
 /**
  * @brief Performs basic validity checks on a driver structure before registration.
  * Ensures required function pointers and names are present.
  * @param driver The driver structure to check.
  * @return FS_SUCCESS or a negative error code.
  */
 static int check_driver_validity(vfs_driver_t *driver) {
     if (!driver) {
         VFS_ERROR("Attempted to check validity of NULL driver");
         return -FS_ERR_INVALID_PARAM;
     }
     if (!driver->fs_name || driver->fs_name[0] == '\0') {
         VFS_ERROR("Driver registration check failed: Missing or empty fs_name");
         return -FS_ERR_INVALID_PARAM;
     }
     // Check essential function pointers required for basic operation
     if (!driver->mount) { VFS_ERROR("Driver '%s' check failed: Missing required 'mount' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->open)  { VFS_ERROR("Driver '%s' check failed: Missing required 'open' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->close) { VFS_ERROR("Driver '%s' check failed: Missing required 'close' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
 
     // Log warnings for missing optional functions (informational)
     if (!driver->read)    VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'read'", driver->fs_name);
     if (!driver->write)   VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'write'", driver->fs_name);
     if (!driver->lseek)   VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'lseek'", driver->fs_name);
     if (!driver->readdir) VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'readdir'", driver->fs_name);
     if (!driver->unlink)  VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'unlink'", driver->fs_name);
     if (!driver->unmount) VFS_DEBUG_LOG("Driver '%s' info: Missing optional 'unmount'", driver->fs_name);
     // Add checks for mkdir, rmdir, stat etc. if they become required
 
     return FS_SUCCESS;
 }
 
 /**
  * @brief Registers a filesystem driver with the VFS.
  * @param driver Pointer to the driver structure to register. Must be persistent.
  * @return FS_SUCCESS on success, negative error code on failure (e.g., duplicate name).
  */
 int vfs_register_driver(vfs_driver_t *driver) {
     int check_result = check_driver_validity(driver);
     if (check_result != FS_SUCCESS) {
         return check_result; // Propagate validation error
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     // Check for duplicate registration by name
     vfs_driver_t *current = driver_list;
     while (current) {
         if (current->fs_name && strcmp(current->fs_name, driver->fs_name) == 0) {
             spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
             VFS_ERROR("Driver '%s' already registered", driver->fs_name);
             return -FS_ERR_FILE_EXISTS; // Use EEXIST equivalent
         }
         current = current->next;
     }
 
     // Add to head of the linked list
     driver->next = driver_list;
     driver_list = driver;
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     VFS_LOG("Registered filesystem driver: %s", driver->fs_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Unregisters a filesystem driver from the VFS.
  * @param driver Pointer to the driver structure to unregister.
  * @return FS_SUCCESS on success, negative error code if not found or invalid.
  */
 int vfs_unregister_driver(vfs_driver_t *driver) {
     if (!driver || !driver->fs_name) {
         VFS_ERROR("Attempted to unregister NULL or invalid driver");
         return -FS_ERR_INVALID_PARAM;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     vfs_driver_t **prev_next_ptr = &driver_list; // Pointer to the previous node's 'next' field
     vfs_driver_t *curr = driver_list;
     bool found = false;
 
     while (curr) {
         if (curr == driver) {
             *prev_next_ptr = curr->next; // Unlink from list
             found = true;
             break;
         }
         prev_next_ptr = &curr->next;
         curr = curr->next;
     }
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     if (found) {
         VFS_LOG("Unregistered driver: %s", driver->fs_name);
         driver->next = NULL; // Clear the unlinked node's next pointer
         return FS_SUCCESS;
     } else {
         VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
         return -FS_ERR_NOT_FOUND; // Use ENOENT equivalent
     }
 }
 
 /**
  * @brief Finds a registered filesystem driver by its name.
  * @param fs_name The name of the filesystem driver (e.g., "FAT", "ext2").
  * @return Pointer to the driver structure, or NULL if not found.
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
         // Ensure current driver has a name before comparing
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
  * @brief Lists all registered filesystem drivers to the kernel log. (Debug utility)
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
         if (count == 0) { VFS_LOG("  (list head not null, but no drivers found - list corrupted?)"); }
         else { VFS_LOG("Total drivers: %d", count); }
     }
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 }
 
 /*---------------------------------------------------------------------------
  * Mount Table Helpers & Path Resolution
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Helper to create and add an entry to the global mount table.
  * Takes ownership of the allocated mount point string.
  * @param mp Absolute mount point path (e.g., "/", "/mnt").
  * @param fs Filesystem name (e.g., "FAT").
  * @param ctx Filesystem-specific context pointer returned by driver->mount.
  * @param drv Pointer to the VFS driver structure.
  * @return FS_SUCCESS or negative error code.
  */
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) {
     if (!mp || !fs || !ctx || !drv) {
         VFS_ERROR("Invalid parameters to add_mount_entry (mp=%p, fs=%p, ctx=%p, drv=%p)", mp, fs, ctx, drv);
         return -FS_ERR_INVALID_PARAM;
     }
     size_t mp_len = strlen(mp);
     if (mp_len == 0 || mp_len >= MAX_PATH_LEN) { // Uses MAX_PATH_LEN from fs_limits.h
         VFS_ERROR("Invalid mount point length: %lu (max: %d)", (unsigned long)mp_len, MAX_PATH_LEN);
         return -FS_ERR_NAMETOOLONG;
     }
     if (mp[0] != '/') {
         VFS_ERROR("Mount point '%s' must be absolute", mp);
         return -FS_ERR_INVALID_PARAM;
     }
 
     // Allocate memory for the mount_t struct and a copy of the mount point path
     char *mp_copy = (char *)kmalloc(mp_len + 1);
     if (!mp_copy) { VFS_ERROR("Failed kmalloc for mount point path copy ('%s')", mp); return -FS_ERR_OUT_OF_MEMORY; }
     strcpy(mp_copy, mp);
 
     mount_t *mnt_to_add = (mount_t *)kmalloc(sizeof(mount_t));
     if (!mnt_to_add) {
         kfree(mp_copy); // Clean up allocated string
         VFS_ERROR("Failed kmalloc for mount_t structure for '%s'", mp);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     // Populate the new mount entry
     mnt_to_add->mount_point = mp_copy; // Store the heap-allocated copy
     mnt_to_add->fs_name = fs;          // Store pointer to persistent driver name
     mnt_to_add->fs_context = ctx;      // Store opaque driver context
     mnt_to_add->next = NULL;
 
     // Add to the global mount table (mount_table_add handles locking)
     int result = mount_table_add(mnt_to_add);
     if (result != FS_SUCCESS) {
         VFS_ERROR("mount_table_add failed for '%s' (err %d)", mp, result);
         kfree(mp_copy);     // Free the string copy on failure
         kfree(mnt_to_add);  // Free the mount_t struct on failure
     } else {
         VFS_LOG("Mount point '%s' (%s) added to table (context: %p)", mp, fs, ctx);
     }
     return result;
 }
 
 /**
  * @brief Finds the most specific (longest matching prefix) mount entry for a given absolute path.
  * @param path The absolute path to resolve (e.g., "/mnt/data/file.txt").
  * @return Pointer to the best matching mount_t entry, or NULL if no suitable mount point (not even root "/") is found.
  */
  static mount_t *find_best_mount_for_path(const char *path) {
     if (!path || path[0] != '/') {
         VFS_DEBUG_LOG("find_best_mount_for_path: Invalid path '%s'", path ? path : "NULL");
         return NULL;
     }
     VFS_DEBUG_LOG("find_best_mount_for_path: Searching for path: '%s'", path);
 
     mount_t *best_match = NULL;
     size_t best_len = 0;
     // Iterate through the mount list (mount_table_get_head provides head, lock handled internally by mount_table_find if needed, but iteration here is potentially racy if list changes)
     // For safety, ideally lock mount table here, iterate, find best, unlock.
     // Assuming read-mostly access or infrequent changes for now.
     mount_t *curr = mount_table_get_head(); // Get list head
 
     while (curr) {
         if (!curr->mount_point || curr->mount_point[0] == '\0') {
             VFS_ERROR("Mount table iteration encountered invalid mount_point in entry %p!", curr);
             curr = curr->next; continue; // Skip invalid entry
         }
 
         size_t current_mount_point_len = strlen(curr->mount_point);
         VFS_DEBUG_LOG("  Checking mount point: '%s' (len %lu)", curr->mount_point, (unsigned long)current_mount_point_len);
 
         // Check if the path starts with the current mount point
         if (strncmp(path, curr->mount_point, current_mount_point_len) == 0) {
             // Check if it's an exact match or a subdirectory match
             bool is_exact_match = (path[current_mount_point_len] == '\0');
             bool is_subdir_match = (path[current_mount_point_len] == '/');
             // Special case: If mount point is "/" and path is not exactly "/", it's a subdir match
             bool is_root_mount = (current_mount_point_len == 1 && curr->mount_point[0] == '/');
             if (is_root_mount && !is_exact_match) {
                 is_subdir_match = true;
             }
 
             if (is_exact_match || is_subdir_match) {
                 // If this match is longer than the previous best match, update
                 if (current_mount_point_len >= best_len) { // Use >= to prefer more specific mounts if lengths are equal (e.g., "/" vs "/mnt" for path "/mnt")
                     VFS_DEBUG_LOG("    -> Found new best match '%s' (len %lu >= %lu)",
                                   curr->mount_point, (unsigned long)current_mount_point_len, (unsigned long)best_len);
                     best_match = curr;
                     best_len = current_mount_point_len;
                 }
             }
         }
         curr = curr->next;
     }
 
     if (best_match) { VFS_DEBUG_LOG("find_best_mount_for_path: Found best match: '%s'", best_match->mount_point); }
     else { VFS_LOG("find_best_mount_for_path: No suitable mount point found for path '%s'.", path); } // Should at least find "/" if root is mounted
     return best_match;
  }
 
 /**
  * @brief Calculates the path relative to a given mount point.
  * @param path The absolute path (e.g., "/mnt/data/file.txt").
  * @param mnt The mount_t entry for the relevant mount point (e.g., "/mnt/data").
  * @return Pointer to the start of the relative path within the original path string (e.g., "/file.txt"),
  * or "/" if the path exactly matches the mount point, or NULL on error.
  */
  static const char *get_relative_path(const char *path, mount_t *mnt) {
     if (!path || !mnt || !mnt->mount_point) {
         VFS_ERROR("get_relative_path: Invalid input (path=%p, mnt=%p)", path, mnt);
         return NULL;
     }
 
     size_t mount_point_len = strlen(mnt->mount_point);
     // Basic check: path must start with mount_point (should be guaranteed by find_best_mount_for_path)
     if (strncmp(path, mnt->mount_point, mount_point_len) != 0) {
          VFS_ERROR("Internal error: Path '%s' does not start with mount point '%s'", path, mnt->mount_point);
          return NULL;
     }
 
     const char *relative_path_start = path + mount_point_len;
 
     // Handle root mount case ("/")
     if (mount_point_len == 1 && mnt->mount_point[0] == '/') {
         // If path is "/", relative is "/". If path is "/foo", relative is "/foo".
         // The pointer relative_path_start already points correctly to either '\0' or 'f'.
         // We need to return "/" if it points to '\0'.
         return (*relative_path_start == '\0') ? "/" : relative_path_start;
     }
 
     // Handle other mount points ("/mnt", "/mnt/data")
     if (*relative_path_start == '\0') {
         // Path exactly matches mount point ("/mnt"), relative is "/"
         return "/";
     } else if (*relative_path_start == '/') {
         // Path is a subdirectory ("/mnt/file"), relative starts at the '/' -> "/file"
         return relative_path_start;
     } else {
         // Should not happen if find_best_mount_for_path works correctly
         VFS_ERROR("Internal error: Invalid prefix match in get_relative_path for '%s' on '%s'", path, mnt->mount_point);
         return NULL;
     }
  }
 
 /*---------------------------------------------------------------------------
  * Mount / Unmount Operations
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Mounts a filesystem (VFS internal implementation). Finds driver, calls driver mount, adds to mount table.
  */
 static int vfs_mount_internal(const char *mp, const char *fs, const char *dev) {
     if (!mp || !fs || !dev) { VFS_ERROR("vfs_mount: Invalid parameters"); return -FS_ERR_INVALID_PARAM; }
     if (mp[0] != '/') { VFS_ERROR("Mount point '%s' must be absolute", mp); return -FS_ERR_INVALID_PARAM; }
     VFS_LOG("VFS internal mount request: mp='%s', fs='%s', dev='%s'", mp, fs, dev);
 
     // Check if mount point is already in use (mount_table_find handles locking)
     if (mount_table_find(mp) != NULL) {
         VFS_ERROR("Mount point '%s' already in use", mp);
         return -FS_ERR_BUSY; // EBUSY
     }
 
     // Find the driver (vfs_get_driver handles locking)
     vfs_driver_t *driver = vfs_get_driver(fs);
     if (!driver) { VFS_ERROR("Filesystem driver '%s' not found", fs); return -FS_ERR_NOT_FOUND; } // ENODEV or similar
     if (!driver->mount) { VFS_ERROR("Driver '%s' exists but has no mount function", fs); return -FS_ERR_NOT_SUPPORTED; } // ENOSYS
 
     // Call the driver's mount function
     VFS_LOG("Calling driver '%s' mount function for device '%s'", fs, dev);
     void *fs_context = driver->mount(dev); // Driver performs device checks and FS initialization
     if (!fs_context) {
         VFS_ERROR("Driver '%s' mount function failed for device '%s'", fs, dev);
         // Driver should log specific error. VFS returns generic mount error.
         return -FS_ERR_MOUNT; // EMOUNT or similar
     }
     VFS_LOG("Driver mount successful, context=%p", fs_context);
 
     // Add the successful mount to the global table
     int result = add_mount_entry(mp, fs, fs_context, driver);
     if (result != FS_SUCCESS) {
         VFS_ERROR("Filesystem mounted but failed to add to mount table! Attempting unmount cleanup.");
         // Attempt to clean up by calling the driver's unmount function
         if (driver->unmount) {
             VFS_LOG("Calling driver unmount cleanup for context %p", fs_context);
             driver->unmount(fs_context); // Best effort cleanup
         } else {
             VFS_ERROR("CRITICAL: Driver '%s' has no unmount function! FS context %p leaked.", fs, fs_context);
         }
         return result; // Propagate error from add_mount_entry
     }
 
     VFS_LOG("Mounted '%s' on '%s' type '%s' successfully", dev, mp, fs);
     return FS_SUCCESS;
  }
 
 /**
  * @brief Unmounts a filesystem (VFS internal implementation). Finds mount entry, checks busy, calls driver unmount, removes from table.
  */
 static int vfs_unmount_internal(const char *mp) {
     if (!mp || mp[0] != '/') { VFS_ERROR("vfs_unmount: Invalid mount point '%s'", mp ? mp : "NULL"); return -FS_ERR_INVALID_PARAM; }
     VFS_LOG("VFS internal unmount request: mp='%s'", mp);
 
     // Find the mount entry (mount_table_find handles locking)
     mount_t *mnt = mount_table_find(mp);
     if (!mnt) { VFS_ERROR("Mount point '%s' not found for unmount", mp); return -FS_ERR_NOT_FOUND; } // ENOENT
 
     // Check if busy (if other filesystems are mounted underneath this one)
     // This check needs to be careful and ideally atomic with the unmount operation.
     // Locking the mount table during this check is recommended.
     // TODO: Add mount_table lock acquire/release around busy check and unmount_entry call.
     bool busy = false;
     size_t mp_len = strlen(mp);
     mount_t *curr = mount_table_get_head(); // Get head (potentially racy without lock)
     while(curr) {
         if (curr != mnt && curr->mount_point && curr->mount_point[0] == '/') {
             size_t curr_len = strlen(curr->mount_point);
             if (curr_len > mp_len && strncmp(curr->mount_point, mp, mp_len) == 0) {
                 bool is_root_mount = (mp_len == 1 && mp[0] == '/');
                 if (is_root_mount || curr->mount_point[mp_len] == '/') {
                     VFS_ERROR("Cannot unmount '%s': Busy (nested mount found: '%s')", mp, curr->mount_point);
                     busy = true; break;
                 }
             }
         }
         curr = curr->next;
     }
     if (busy) { return -FS_ERR_BUSY; } // EBUSY
 
     // Perform the actual unmount using the helper (which calls driver and removes from table)
     return vfs_unmount_entry(mnt);
  }
 
 /**
  * @brief Internal helper to perform unmount logic given a mount_t entry.
  * Calls driver unmount (if exists) and removes entry from mount table.
  * @param mnt Pointer to the mount_t entry to unmount.
  * @return FS_SUCCESS or negative error code.
  */
 static int vfs_unmount_entry(mount_t *mnt) {
     if (!mnt || !mnt->fs_name || !mnt->mount_point || !mnt->fs_context) {
         VFS_ERROR("vfs_unmount_entry: Invalid mount_t provided (%p)", mnt);
         return -FS_ERR_INTERNAL; // Should not happen if called correctly
     }
     VFS_LOG("Performing internal unmount for '%s' (%s)", mnt->mount_point, mnt->fs_name);
 
     // Find the driver (lock acquired/released inside)
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     int driver_result = FS_SUCCESS;
 
     if (!driver) {
         VFS_ERROR("CRITICAL INCONSISTENCY: Driver '%s' not found during unmount for '%s'!", mnt->fs_name, mnt->mount_point);
         driver_result = -FS_ERR_INTERNAL; // Indicate internal error
     } else if (driver->unmount) {
         VFS_LOG("Calling driver '%s' unmount function (context %p)", mnt->fs_name, mnt->fs_context);
         driver_result = driver->unmount(mnt->fs_context); // Call driver's cleanup
         if (driver_result != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' failed to unmount '%s' (context %p, err %d)", mnt->fs_name, mnt->mount_point, mnt->fs_context, driver_result);
             // Continue to remove from mount table even if driver failed? Yes, to reflect VFS state.
         } else {
              VFS_LOG("Driver unmount successful for '%s'", mnt->mount_point);
         }
     } else {
         VFS_LOG("Driver '%s' has no unmount function for '%s'. FS context %p may leak.", mnt->fs_name, mnt->mount_point, mnt->fs_context);
         // No driver error to report, proceed with table removal.
     }
 
     // Remove from mount table (mount_table_remove handles locking and freeing mnt/mp_copy)
     char* mount_point_to_remove = (char*)mnt->mount_point; // Cache before mnt is freed
     VFS_LOG("Removing '%s' from mount table", mount_point_to_remove);
     int remove_result = mount_table_remove(mount_point_to_remove);
     if (remove_result != FS_SUCCESS) {
          VFS_ERROR("mount_table_remove failed for '%s' (err %d) AFTER driver unmount attempt!", mount_point_to_remove, remove_result);
          // Prioritize returning the driver error if it occurred, otherwise return table error
          return (driver_result != FS_SUCCESS) ? driver_result : remove_result;
     }
 
     VFS_LOG("Successfully unmounted and removed '%s' from table.", mount_point_to_remove);
     return driver_result; // Return the result of the driver's unmount attempt (could be SUCCESS or an error)
 }
 
 /**
  * @brief Mounts the root filesystem. Public API called by fs_init.
  */
 int vfs_mount_root(const char *mp, const char *fs_type, const char *dev) {
     VFS_LOG("vfs_mount_root: Request to mount '%s' (%s) on '%s'", dev, fs_type, mp);
     if (strcmp(mp, "/") != 0) {
         VFS_ERROR("vfs_mount_root: Mount point must be '/' (got '%s')", mp);
         return -FS_ERR_INVALID_PARAM;
     }
     return vfs_mount_internal(mp, fs_type, dev);
 }
 
 /**
  * @brief Unmounts the root filesystem. Public API potentially called by fs_shutdown.
  */
 int vfs_unmount_root(void) {
     VFS_LOG("vfs_unmount_root: Request to unmount '/'");
     return vfs_unmount_internal("/");
 }
 
 /**
  * @brief Lists all mounted filesystems via mount_table. (Debug utility)
  */
 void vfs_list_mounts(void) {
     VFS_LOG("--- Mount Table Listing ---");
     mount_table_list(); // Delegates to mount_table implementation
     VFS_LOG("--- End Mount Table ---");
 }
 
 /**
  * @brief Shuts down the VFS layer, attempting to unmount all filesystems.
  */
 int vfs_shutdown(void) {
     VFS_LOG("Shutting down VFS layer...");
     int final_result = FS_SUCCESS;
     int unmount_attempts = 0;
     const int max_attempts = 100; // Safety break
 
     mount_t *current_mount;
     // Iteratively unmount the head of the list. This helps with dependencies
     // (e.g., unmounting "/mnt/data" before "/mnt").
     while ((current_mount = mount_table_get_head()) != NULL && unmount_attempts < max_attempts) {
         unmount_attempts++;
         char mp_copy[MAX_PATH_LEN];
         // Safely copy mount point name before potential free
         if (current_mount->mount_point) {
             strncpy(mp_copy, current_mount->mount_point, MAX_PATH_LEN - 1);
             mp_copy[MAX_PATH_LEN - 1] = '\0';
         } else {
             strcpy(mp_copy, "[INVALID/NULL Mount Point]");
             VFS_ERROR("VFS Shutdown: Encountered mount entry with NULL mount_point!");
         }
 
         VFS_LOG("Attempting shutdown unmount for '%s'...", mp_copy);
         int result = vfs_unmount_entry(current_mount); // Calls driver unmount & removes from table
         if (result != FS_SUCCESS) {
             VFS_ERROR("Failed to unmount '%s' during shutdown (error %d). Possible busy state or driver error.", mp_copy, result);
             if (final_result == FS_SUCCESS) final_result = result; // Store first error
             // If unmount failed, entry might still be in table. Loop relies on max_attempts.
             if (mount_table_find(mp_copy) != NULL) {
                 VFS_ERROR("CRITICAL: Mount point '%s' still exists after unmount attempt failure!", mp_copy);
             }
         }
     }
 
     if (unmount_attempts >= max_attempts) {
         VFS_ERROR("VFS Shutdown: Reached max unmount attempts (%d), potential mount table issues or busy mounts!", max_attempts);
         if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY;
     }
     if (mount_table_get_head() != NULL) {
        VFS_ERROR("VFS Shutdown: Mount points still remain after shutdown attempt!");
         if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY;
         mount_table_list(); // List remaining mounts for debugging
     }
 
     // Clear the driver list (assume drivers don't need explicit cleanup beyond unmount)
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
     // TODO: Free driver structs if dynamically allocated? Assume static for now.
     driver_list = NULL;
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     // TODO: Call mount_table_destroy() if it exists.
 
     if (final_result == FS_SUCCESS) { VFS_LOG("VFS shutdown complete"); }
     else { VFS_ERROR("VFS shutdown encountered errors (first error code: %d)", final_result); }
     return final_result;
 }
 
 /*---------------------------------------------------------------------------
  * File Operations (VFS Public API Implementation)
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Opens or creates a file/directory via the appropriate driver.
  * Resolves path to mount point, calls driver->open, allocates file_t.
  * @param path Absolute path to the file.
  * @param flags Open flags (O_RDONLY, O_CREAT, etc.).
  * @return Pointer to file_t structure on success, NULL on failure.
  */
  file_t *vfs_open(const char *path, int flags) {
     // Serial logging for entry
     serial_write("[vfs_open] Enter. Path='"); serial_write(path ? path : "NULL");
     serial_write("', Flags=0x"); serial_print_hex((uint32_t)flags); serial_write("\n");
 
     if (!path || path[0] != '/') { serial_write("[vfs_open] Error: Invalid path.\n"); VFS_ERROR("vfs_open: Invalid path '%s'", path ? path : "NULL"); return NULL; }
 
     // 1. Find the responsible mount point and driver
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { serial_write("[vfs_open] Error: No mount point found for path.\n"); VFS_ERROR("vfs_open: No mount point found for path '%s'", path); return NULL; }
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { serial_write("[vfs_open] Error: Driver not found for mount point.\n"); VFS_ERROR("vfs_open: Driver '%s' not found for mount '%s'", mnt->fs_name, mnt->mount_point); return NULL; }
 
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { serial_write("[vfs_open] Error: Failed to get relative path.\n"); VFS_ERROR("vfs_open: Failed to calculate relative path for '%s' on '%s'", path, mnt->mount_point); return NULL; }
 
     serial_write("[vfs_open] Using mount='"); serial_write(mnt->mount_point);
     serial_write("', driver='"); serial_write(driver->fs_name);
     serial_write("', rel_path='"); serial_write(relative_path); serial_write("'\n");
 
     if (!driver->open) { serial_write("[vfs_open] Error: Driver does not support open.\n"); VFS_ERROR("Driver '%s' does not support open", driver->fs_name); return NULL; }
 
     // 2. Call the driver's open function
     serial_write("[vfs_open] >>> Calling driver->open: ctx="); serial_print_hex((uintptr_t)mnt->fs_context);
     serial_write(", rel_path='"); serial_write(relative_path);
     serial_write("', flags=0x"); serial_print_hex((uint32_t)flags); serial_write("\n");
 
     vnode_t *node = driver->open(mnt->fs_context, relative_path, flags);
 
     serial_write("[vfs_open] <<< driver->open returned node="); serial_print_hex((uintptr_t)node); serial_write("\n");
 
     if (!node) { serial_write("[vfs_open] Driver open failed.\n"); VFS_DEBUG_LOG("vfs_open: Driver '%s' failed open for rel_path '%s'", driver->fs_name, relative_path); return NULL; }
 
     // 3. Validate vnode returned by driver
     if (node->fs_driver != driver) {
         serial_write("[vfs_open] CRITICAL Error: Driver did not set fs_driver correctly!\n");
         VFS_ERROR("CRITICAL: Driver '%s' open did NOT set vnode->fs_driver!", driver->fs_name);
         // Attempt cleanup, but state is inconsistent
         if (driver->close) {
              file_t temp_file = { .vnode = node, .flags = flags, .offset = 0 };
              node->fs_driver = driver; // Force set for close call
              driver->close(&temp_file);
         }
         kfree(node); // Free the vnode struct itself
         return NULL;
     }
 
     // 4. Allocate VFS file handle
     file_t *file = (file_t *)kmalloc(sizeof(file_t));
     if (!file) {
         serial_write("[vfs_open] Error: kmalloc for file_t failed.\n");
         VFS_ERROR("vfs_open: Failed kmalloc for file_t for path '%s'", path);
         VFS_LOG("vfs_open: Cleaning up vnode %p via driver close", node);
         // Call driver close to release driver-specific resources
         if (driver->close) {
             file_t temp_file = { .vnode = node, .flags = flags, .offset = 0 };
             driver->close(&temp_file); // Driver cleans up node->data
         } else {
             VFS_ERROR("vfs_open: Driver '%s' has no close! Cannot clean up node->data %p", driver->fs_name, node->data);
         }
         kfree(node); // Free the vnode struct
         return NULL;
     }
 
     // 5. Populate file handle
     file->vnode = node;
     file->flags = flags;
     file->offset = 0; // Reset offset on open
 
     serial_write("[vfs_open] Success. file="); serial_print_hex((uintptr_t)file);
     serial_write(", vnode="); serial_print_hex((uintptr_t)node);
     serial_write(", node->data="); serial_print_hex((uintptr_t)node->data); serial_write("\n");
     return file;
  }
 
 /**
  * @brief Closes an open file handle. Calls the underlying driver's close.
  * Frees VFS resources (file_t, vnode_t). Driver frees its context (vnode->data).
  * @param file The file handle to close.
  * @return FS_SUCCESS or negative error code from driver.
  */
 int vfs_close(file_t *file) {
     if (!file) { VFS_ERROR("NULL file handle passed to vfs_close"); return -FS_ERR_INVALID_PARAM; }
     // Check for double close or corruption
     if (!file->vnode) { VFS_ERROR("vfs_close: File handle %p has NULL vnode!", file); kfree(file); return -FS_ERR_BAD_F; }
     if (!file->vnode->fs_driver) { VFS_ERROR("vfs_close: Vnode %p has NULL fs_driver!", file->vnode); kfree(file->vnode); kfree(file); return -FS_ERR_BAD_F; }
 
     vfs_driver_t* driver = file->vnode->fs_driver;
     VFS_DEBUG_LOG("vfs_close: Closing file handle %p (vnode: %p, driver: %s)", file, file->vnode, driver->fs_name ? driver->fs_name : "[N/A]");
 
     int result = FS_SUCCESS;
     if (driver->close) {
         result = driver->close(file); // Driver cleans up vnode->data
         if (result != FS_SUCCESS) { VFS_ERROR("vfs_close: Driver '%s' close failed (err %d)", driver->fs_name, result); }
     } else {
         VFS_WARN("vfs_close: Driver '%s' has no close function. Potential resource leak for vnode->data %p.", driver->fs_name, file->vnode->data);
     }
 
     // VFS layer frees its own structures
     kfree(file->vnode);
     kfree(file);
 
     return result; // Return result from driver close
  }
 
 /**
  * @brief Reads data from an open file via the appropriate driver.
  * @param file The open file handle.
  * @param buf Kernel buffer to read into.
  * @param len Maximum number of bytes to read.
  * @return Number of bytes read, 0 on EOF, negative error code on failure.
  * @note Does NOT currently implement per-file locking. Concurrent reads might race on file->offset.
  */
 int vfs_read(file_t *file, void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
     if (!buf && len > 0) return -FS_ERR_INVALID_PARAM;
     if (len == 0) return 0;
     if (!file->vnode->fs_driver->read) return -FS_ERR_NOT_SUPPORTED;
 
     // TODO: Implement file handle locking here for SMP safety if needed
     // acquire_lock(&file->lock);
 
     VFS_DEBUG_LOG("vfs_read: file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
     int bytes_read = file->vnode->fs_driver->read(file, buf, len);
 
     if (bytes_read > 0) {
         // Check for offset overflow before adding
         // <<< FIXED: Use LONG_MAX from libc/limits.h >>>
         if (file->offset > (LONG_MAX - bytes_read)) {
              VFS_ERROR("vfs_read: File offset overflow for file %p", file);
              // What to do? Clamp offset? Return error? Let's clamp for now.
              file->offset = LONG_MAX;
         } else {
              file->offset += bytes_read; // Update offset *after* successful read
         }
         VFS_DEBUG_LOG("vfs_read: Read %d bytes, new offset=%ld", bytes_read, (long)file->offset);
     } else if (bytes_read == 0) {
         VFS_DEBUG_LOG("vfs_read: Read 0 bytes (EOF)");
     } else {
         VFS_ERROR("vfs_read: Driver read failed (err %d) for file %p", bytes_read, file);
     }
 
     // release_lock(&file->lock);
     return bytes_read;
  }
 
 /**
  * @brief Writes data to an open file via the appropriate driver.
  * @param file The open file handle.
  * @param buf Kernel buffer containing data to write.
  * @param len Number of bytes to write.
  * @return Number of bytes written, negative error code on failure.
  * @note Does NOT currently implement per-file locking. Concurrent writes will race on file->offset and potentially corrupt file content/metadata.
  */
 int vfs_write(file_t *file, const void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
     if (!buf && len > 0) return -FS_ERR_INVALID_PARAM;
     if (len == 0) return 0;
 
     // Check VFS open flags for write permission
     int access_mode = file->flags & O_ACCMODE;
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         VFS_ERROR("vfs_write: File not opened for writing (flags: 0x%lx)", (unsigned long)file->flags);
         return -FS_ERR_PERMISSION_DENIED; // Use EACCES/EPERM equivalent
     }
 
     if (!file->vnode->fs_driver->write) return -FS_ERR_NOT_SUPPORTED;
 
     // TODO: Implement file handle locking here for SMP safety if needed
     // acquire_lock(&file->lock);
 
     VFS_DEBUG_LOG("vfs_write: file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
     int bytes_written = file->vnode->fs_driver->write(file, buf, len);
 
     if (bytes_written > 0) {
         // Check for offset overflow before adding
         // <<< FIXED: Use LONG_MAX from libc/limits.h >>>
         if (file->offset > (LONG_MAX - bytes_written)) {
              VFS_ERROR("vfs_write: File offset overflow for file %p", file);
              file->offset = LONG_MAX; // Clamp offset
         } else {
              file->offset += bytes_written; // Update offset *after* successful write
         }
         VFS_DEBUG_LOG("vfs_write: Wrote %d bytes, new offset=%ld", bytes_written, (long)file->offset);
     } else if (bytes_written == 0) {
         VFS_DEBUG_LOG("vfs_write: Wrote 0 bytes (requested %lu)", (unsigned long)len);
     } else {
         VFS_ERROR("vfs_write: Driver write failed (err %d) for file %p", bytes_written, file);
     }
 
     // release_lock(&file->lock);
     return bytes_written;
  }
 
 /**
  * @brief Changes the file offset via the appropriate driver.
  * @param file The open file handle.
  * @param offset Offset value.
  * @param whence Reference point (SEEK_SET, SEEK_CUR, SEEK_END).
  * @return New absolute offset from the beginning of the file, or negative error code.
  * @note Does NOT currently implement per-file locking. Concurrent lseek/read/write will race on file->offset.
  */
 off_t vfs_lseek(file_t *file, off_t offset, int whence) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return (off_t)-FS_ERR_BAD_F;
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { VFS_ERROR("vfs_lseek: Invalid whence value (%d)", whence); return (off_t)-FS_ERR_INVALID_PARAM; }
     if (!file->vnode->fs_driver->lseek) { return (off_t)-FS_ERR_NOT_SUPPORTED; } // Use ESPIPE?
 
     // TODO: Implement file handle locking here for SMP safety if needed
     // acquire_lock(&file->lock);
 
     VFS_DEBUG_LOG("vfs_lseek: file=%p, vnode=%p, offset=%ld, whence=%d", file, file->vnode, (long)offset, whence);
     off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);
 
     if (new_offset >= 0) {
         file->offset = new_offset; // Update VFS offset on success
         VFS_DEBUG_LOG("vfs_lseek: Seek successful for file=%p, new offset=%ld", file, (long)new_offset);
     } else {
         VFS_ERROR("vfs_lseek: Driver lseek failed for file=%p (err %ld)", file, (long)new_offset);
     }
 
     // release_lock(&file->lock);
     return new_offset; // Return result from driver
 }
 
 /**
  * @brief Reads a directory entry via the appropriate driver.
  * @param dir_file Open file handle representing the directory.
  * @param d_entry_out Pointer to VFS dirent structure to populate.
  * @param entry_index The logical index of the entry to retrieve (0-based).
  * @return FS_SUCCESS (0) on success, -FS_ERR_EOF/-FS_ERR_NOT_FOUND at end, negative error otherwise.
  */
 int vfs_readdir(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index) {
     if (!dir_file || !d_entry_out) return -FS_ERR_INVALID_PARAM;
     if (!dir_file->vnode || !dir_file->vnode->fs_driver) return -FS_ERR_BAD_F;
     // TODO: Add check: Is this actually a directory?
     // if (!(dir_file->flags & O_DIRECTORY)) return -FS_ERR_NOT_A_DIRECTORY;
 
     if (!dir_file->vnode->fs_driver->readdir) return -FS_ERR_NOT_SUPPORTED;
 
     // TODO: Implement directory handle locking if needed for SMP safety
     // acquire_lock(&dir_file->lock);
 
     VFS_DEBUG_LOG("vfs_readdir: dir_file=%p, index=%lu", dir_file, (unsigned long)entry_index);
     int result = dir_file->vnode->fs_driver->readdir(dir_file, d_entry_out, entry_index);
 
     if (result == FS_SUCCESS) { VFS_DEBUG_LOG("vfs_readdir: Success index %lu, name='%s'", (unsigned long)entry_index, d_entry_out->d_name); }
     else if (result == -FS_ERR_EOF || result == -FS_ERR_NOT_FOUND) { VFS_DEBUG_LOG("vfs_readdir: End of directory/Not found at index %lu", (unsigned long)entry_index); }
     else { VFS_ERROR("vfs_readdir: Driver readdir failed (err %d)", result); }
 
     // release_lock(&dir_file->lock);
     return result;
 }
 
 /**
  * @brief Deletes a name (file or potentially empty directory) from the filesystem.
  * @param path Absolute path to the item to delete.
  * @return FS_SUCCESS or negative error code.
  */
 int vfs_unlink(const char *path) {
     if (!path || path[0] != '/') { VFS_ERROR("vfs_unlink: Invalid path '%s'", path ? path : "NULL"); return -FS_ERR_INVALID_PARAM; }
     VFS_DEBUG_LOG("vfs_unlink: path='%s'", path);
 
     // 1. Resolve path to mount point and driver
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { VFS_ERROR("vfs_unlink: No mount point for path '%s'", path); return -FS_ERR_NOT_FOUND; }
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { VFS_ERROR("vfs_unlink: Driver '%s' not found for mount '%s'", mnt->fs_name, mnt->mount_point); return -FS_ERR_INTERNAL; }
 
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("vfs_unlink: Failed to get relative path for '%s'", path); return -FS_ERR_INTERNAL; }
 
     // 2. Check if driver supports unlink
     if (!driver->unlink) return -FS_ERR_NOT_SUPPORTED; // EPERM or ENOSYS
 
     VFS_DEBUG_LOG("vfs_unlink: Using mount '%s', driver '%s', relative path '%s'", mnt->mount_point, driver->fs_name, relative_path);
 
     // 3. Call driver's unlink function
     // TODO: Add locking around operations that modify directory structure if needed for SMP
     int result = driver->unlink(mnt->fs_context, relative_path);
 
     if (result == FS_SUCCESS) { VFS_LOG("vfs_unlink: Driver unlinked '%s' relative to '%s'", relative_path, mnt->mount_point); }
     else { VFS_ERROR("vfs_unlink: Driver failed to unlink '%s' (err %d)", path, result); }
     return result;
 }
 
 
 /*---------------------------------------------------------------------------
  * VFS Status and Utility Functions
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Checks if the VFS is initialized and has a root filesystem mounted.
  */
 bool vfs_is_ready(void) {
     // Check if the root mount point "/" exists (mount_table_find handles locking)
     return (mount_table_find("/") != NULL);
 }
 
 /**
  * @brief Performs a basic self-test of the VFS (e.g., opening root).
  */
  int vfs_self_test(void) {
     VFS_LOG("Running VFS self-test...");
     if (!vfs_is_ready()) { VFS_ERROR("VFS self-test FAILED: VFS not ready (root '/' not mounted)"); return -FS_ERR_NOT_INIT; }
 
     VFS_LOG("VFS self-test: Attempting to open root directory '/'...");
     file_t *root_dir = vfs_open("/", O_RDONLY); // Open read-only
     if (!root_dir) { VFS_ERROR("VFS self-test FAILED: vfs_open failed for root '/'"); return -FS_ERR_IO; }
     VFS_LOG("VFS self-test: Root directory opened successfully (file: %p).", root_dir);
 
     // Optional: Try reading first entry?
     // struct dirent entry;
     // int read_res = vfs_readdir(root_dir, &entry, 0);
     // ... check read_res ...
 
     VFS_LOG("VFS self-test: Attempting to close root directory...");
     int close_result = vfs_close(root_dir);
     if (close_result != FS_SUCCESS) { VFS_ERROR("VFS self-test FAILED: vfs_close failed for root (code: %d)", close_result); return close_result; }
     VFS_LOG("VFS self-test: Root directory closed successfully.");
 
     VFS_LOG("VFS self-test PASSED");
     return FS_SUCCESS;
  }
 
 /**
  * @brief Checks if a path exists by attempting to open it read-only.
  */
  bool vfs_path_exists(const char *path) {
     if (!path) return false;
     VFS_DEBUG_LOG("vfs_path_exists: Checking '%s'", path);
     file_t *file = vfs_open(path, O_RDONLY);
     if (!file) { VFS_DEBUG_LOG("vfs_path_exists: vfs_open failed for '%s', path does not exist or is inaccessible.", path); return false; }
     VFS_DEBUG_LOG("vfs_path_exists: vfs_open succeeded for '%s', path exists.", path);
     vfs_close(file); // Ignore close result
     return true;
  }
 
 /**
  * @brief Dumps VFS debug information (drivers, mounts) to the kernel log.
  */
  void vfs_debug_dump(void) {
     VFS_LOG("========== VFS DEBUG INFORMATION ==========");
     vfs_list_drivers(); // Dump registered drivers
     vfs_list_mounts();  // Dump mounted filesystems
     VFS_LOG("==========================================");
  }
 