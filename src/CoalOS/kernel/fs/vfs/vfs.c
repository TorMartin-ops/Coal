/**
 * @file vfs.c
 * @brief Virtual File System Implementation
 * @version 1.3
 *
 * Provides a unified interface over different filesystem drivers.
 * Manages filesystem driver registration and mount points via the mount_table.
 * Implements core VFS operations (open, close, read, write, lseek, etc.)
 * by dispatching calls to the appropriate underlying filesystem driver.
 *
 *
 * Key Aspects & Considerations:
 * - Mount Point Resolution: Uses longest prefix matching.
 * - Driver Management: Simple linked list for registered drivers.
 * - Locking: Uses spinlocks for global driver list, mount table, and now
 * per-file structures (file_t) to protect offset/state during I/O.
 * - Error Handling: Primarily propagates errors from underlying drivers or
 * returns standard FS_ERR_* / POSIX errno codes.
 * - Missing Features: Permissions, ownership, directory creation/deletion,
 * symbolic links, stat, advanced caching, etc.
 */

 #include <kernel/fs/vfs/vfs.h>           // Declares vfs_driver_t, file_t, vnode_t etc. (MUST define file_t.lock)
 #include <kernel/memory/kmalloc.h>       // Kernel memory allocation
 #include <kernel/drivers/display/terminal.h>      // Kernel logging/printing
 #include <kernel/lib/string.h>        // Kernel string functions (strcmp, strlen, strncpy, etc.)
 #include <kernel/core/types.h>         // Core types (ssize_t, off_t, etc.)
 #include <kernel/fs/vfs/sys_file.h>      // SEEK_*, O_* flags
 #include <kernel/fs/vfs/fs_errno.h>      // FS_ERR_* / POSIX errno codes
 #include <kernel/fs/vfs/fs_limits.h>     // MAX_PATH_LEN definition
 #include <kernel/fs/vfs/mount.h>         // mount_t definition
 #include <kernel/fs/vfs/mount_table.h>   // Global mount table functions
 #include <kernel/sync/spinlock.h>      // Spinlock definitions and functions
 #include <libc/limits.h>   // LONG_MAX, LONG_MIN etc. (Assumed available)
 #include <libc/stddef.h>   // NULL, size_t (Assumed available)
 #include <libc/stdbool.h>  // bool (Assumed available)
 #include <libc/stdarg.h>   // varargs for printf (Assumed available)
 #include <kernel/lib/assert.h>        // KERNEL_ASSERT
 #include <kernel/drivers/display/serial.h>        // Serial logging for critical paths

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
 #define OFF_T_MAX LONG_MAX
 #endif
 #ifndef OFF_T_MIN
 #define OFF_T_MIN LONG_MIN
 #endif


 /* Debug macro - define VFS_DEBUG_LEVEL >= 1 to enable verbose logging */
 #ifndef VFS_DEBUG_LEVEL
 #define VFS_DEBUG_LEVEL 1 // Keep some logging on by default
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
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv);
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
     // Check other *required* file ops
     if (!driver->read)  { VFS_ERROR("Driver '%s' check failed: Missing required 'read'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->write) { VFS_ERROR("Driver '%s' check failed: Missing required 'write'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->lseek) { VFS_ERROR("Driver '%s' check failed: Missing required 'lseek'", driver->fs_name); return -FS_ERR_INVALID_PARAM; }

     // Log warnings for missing optional functions (not fatal for registration)
     if (!driver->readdir) VFS_WARN("Driver '%s' info: Missing optional 'readdir'", driver->fs_name);
     if (!driver->unlink)  VFS_WARN("Driver '%s' info: Missing optional 'unlink'", driver->fs_name);
     if (!driver->unmount) VFS_WARN("Driver '%s' info: Missing optional 'unmount'", driver->fs_name);
     // Add checks for mkdir, rmdir, stat etc. if added to vfs_driver_t

     return FS_SUCCESS;
 }

 /**
  * @brief Registers a filesystem driver with the VFS.
  */
 int vfs_register_driver(vfs_driver_t *driver) {
     int check_result = check_driver_validity(driver);
     if (check_result != FS_SUCCESS) {
         return check_result;
     }

     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);

     // Check for duplicate registration
     vfs_driver_t *current = driver_list;
     while (current) {
         if (current->fs_name && strcmp(current->fs_name, driver->fs_name) == 0) {
             spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
             VFS_ERROR("Driver '%s' already registered", driver->fs_name);
             return -FS_ERR_FILE_EXISTS;
         }
         current = current->next;
     }

     // Add to head
     driver->next = driver_list;
     driver_list = driver;

     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);

     VFS_LOG("Registered filesystem driver: %s", driver->fs_name);
     return FS_SUCCESS;
 }

 /**
  * @brief Unregisters a filesystem driver from the VFS.
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
             *prev_next_ptr = curr->next;
             found = true;
             break;
         }
         prev_next_ptr = &curr->next;
         curr = curr->next;
     }

     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);

     if (found) {
         VFS_LOG("Unregistered driver: %s", driver->fs_name);
         driver->next = NULL;
         return FS_SUCCESS;
     } else {
         VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
         return -FS_ERR_NOT_FOUND;
     }
 }

 /**
  * @brief Finds a registered filesystem driver by its name.
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
  */
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) {
     KERNEL_ASSERT(mp && fs && ctx && drv, "add_mount_entry: Invalid NULL parameter");
     size_t mp_len = strlen(mp);
     if (mp_len == 0 || mp_len >= MAX_PATH_LEN) {
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
         kfree(mp_copy);
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
         kfree(mp_copy);
         kfree(mnt_to_add);
     } else {
         VFS_LOG("Mount point '%s' (%s) added to table (context: %p)", mp, fs, ctx);
     }
     return result;
 }

 /**
  * @brief Finds the most specific (longest matching prefix) mount entry for a given absolute path.
  */
  static mount_t *find_best_mount_for_path(const char *path) {
     KERNEL_ASSERT(path && path[0] == '/', "find_best_mount_for_path: Invalid path");
     VFS_DEBUG_LOG("find_best_mount_for_path: Searching for path: '%s'", path);

     mount_t *best_match = NULL;
     size_t best_len = 0;
     // Read access to mount table (locking handled by mount_table_get_head/iteration if needed)
     // Assuming mount table reads are safe or infrequent for now.
     mount_t *curr = mount_table_get_head();

     while (curr) {
         KERNEL_ASSERT(curr->mount_point && curr->mount_point[0] == '/', "Invalid mount entry in list");
         size_t current_mount_point_len = strlen(curr->mount_point);
         VFS_DEBUG_LOG("  Checking mount point: '%s' (len %lu)", curr->mount_point, (unsigned long)current_mount_point_len);

         if (strncmp(path, curr->mount_point, current_mount_point_len) == 0) {
             bool is_exact_match = (path[current_mount_point_len] == '\0');
             bool is_subdir_match = (path[current_mount_point_len] == '/');
             bool is_root_mount = (current_mount_point_len == 1); // '/' is the only 1-char valid mount point

             if (is_exact_match || is_subdir_match || (is_root_mount && !is_exact_match)) {
                 if (current_mount_point_len >= best_len) {
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
     else { VFS_LOG("find_best_mount_for_path: No suitable mount point found for path '%s'.", path); }
     return best_match;
  }

 /**
  * @brief Calculates the path relative to a given mount point.
  */
  static const char *get_relative_path(const char *path, mount_t *mnt) {
     KERNEL_ASSERT(path && mnt && mnt->mount_point, "get_relative_path: Invalid input");
     size_t mount_point_len = strlen(mnt->mount_point);
     KERNEL_ASSERT(strncmp(path, mnt->mount_point, mount_point_len) == 0, "Path does not start with mount point");

     const char *relative_path_start = path + mount_point_len;

     // Handle root mount ("/") correctly
     if (mount_point_len == 1 && mnt->mount_point[0] == '/') {
         return (*relative_path_start == '\0') ? "/" : relative_path_start;
     }
     // Handle other mounts
     if (*relative_path_start == '\0') return "/"; // Exact match
     if (*relative_path_start == '/') return relative_path_start; // Subdirectory

     VFS_ERROR("Internal error: Invalid prefix match in get_relative_path for '%s' on '%s'", path, mnt->mount_point);
     return NULL; // Should not happen
  }

 /*---------------------------------------------------------------------------
  * Mount / Unmount Operations
  *---------------------------------------------------------------------------*/

 /**
  * @brief Mounts a filesystem (VFS internal implementation).
  */
 static int vfs_mount_internal(const char *mp, const char *fs, const char *dev) {
     KERNEL_ASSERT(mp && fs && dev, "vfs_mount_internal: Invalid NULL parameter");
     KERNEL_ASSERT(mp[0] == '/', "Mount point must be absolute");
     VFS_LOG("VFS internal mount request: mp='%s', fs='%s', dev='%s'", mp, fs, dev);

     // Check if mount point exists (mount_table_find handles locking)
     if (mount_table_find(mp) != NULL) {
         VFS_ERROR("Mount point '%s' already in use", mp);
         return -FS_ERR_BUSY;
     }

     // Find driver (vfs_get_driver handles locking)
     vfs_driver_t *driver = vfs_get_driver(fs);
     if (!driver) { VFS_ERROR("Filesystem driver '%s' not found", fs); return -FS_ERR_NOT_FOUND; }
     if (!driver->mount) { VFS_ERROR("Driver '%s' exists but has no mount function", fs); return -FS_ERR_NOT_SUPPORTED; }

     // Call driver mount
     VFS_LOG("Calling driver '%s' mount function for device '%s'", fs, dev);
     void *fs_context = driver->mount(dev);
     if (!fs_context) {
         VFS_ERROR("Driver '%s' mount function failed for device '%s'", fs, dev);
         return -FS_ERR_MOUNT;
     }
     VFS_LOG("Driver mount successful, context=%p", fs_context);

     // Add to mount table
     int result = add_mount_entry(mp, fs, fs_context, driver);
     if (result != FS_SUCCESS) {
         VFS_ERROR("Filesystem mounted but failed to add to mount table! Attempting unmount cleanup.");
         if (driver->unmount) {
             VFS_LOG("Calling driver unmount cleanup for context %p", fs_context);
             driver->unmount(fs_context);
         } else {
             VFS_ERROR("CRITICAL: Driver '%s' has no unmount function! FS context %p leaked.", fs, fs_context);
         }
         return result;
     }

     VFS_LOG("Mounted '%s' on '%s' type '%s' successfully", dev, mp, fs);
     return FS_SUCCESS;
  }

 /**
  * @brief Unmounts a filesystem (VFS internal implementation).
  */
 static int vfs_unmount_internal(const char *mp) {
     KERNEL_ASSERT(mp && mp[0] == '/', "vfs_unmount: Invalid mount point");
     VFS_LOG("VFS internal unmount request: mp='%s'", mp);

     // Find mount entry (mount_table_find handles locking)
     mount_t *mnt = mount_table_find(mp);
     if (!mnt) { VFS_ERROR("Mount point '%s' not found for unmount", mp); return -FS_ERR_NOT_FOUND; }

     // Check if busy (requires mount_table lock or atomic check)
     // --- Simplified Check (Racy without external lock) ---
     bool busy = false;
     size_t mp_len = strlen(mp);
     mount_t *curr = mount_table_get_head(); // Use getter
     while(curr) {
         if (curr != mnt && curr->mount_point && curr->mount_point[0] == '/') {
             size_t curr_len = strlen(curr->mount_point);
             if (curr_len > mp_len && strncmp(curr->mount_point, mp, mp_len) == 0) {
                 bool is_root_mount = (mp_len == 1); // '/' is the only 1-char mount point
                 if (is_root_mount || curr->mount_point[mp_len] == '/') {
                     VFS_ERROR("Cannot unmount '%s': Busy (nested mount found: '%s')", mp, curr->mount_point);
                     busy = true; break;
                 }
             }
         }
         curr = curr->next;
     }
     if (busy) { return -FS_ERR_BUSY; }
     // --- End Simplified Check ---

     // Perform the unmount (calls driver, removes from table)
     return vfs_unmount_entry(mnt);
  }

 /**
  * @brief Internal helper to perform unmount logic given a mount_t entry.
  */
 static int vfs_unmount_entry(mount_t *mnt) {
     KERNEL_ASSERT(mnt && mnt->fs_name && mnt->mount_point && mnt->fs_context, "vfs_unmount_entry: Invalid mount_t");
     VFS_LOG("Performing internal unmount for '%s' (%s)", mnt->mount_point, mnt->fs_name);

     // Find the driver (lock acquired/released inside)
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     int driver_result = FS_SUCCESS;

     if (!driver) {
         VFS_ERROR("CRITICAL INCONSISTENCY: Driver '%s' not found during unmount for '%s'!", mnt->fs_name, mnt->mount_point);
         driver_result = -FS_ERR_INTERNAL;
     } else if (driver->unmount) {
         VFS_LOG("Calling driver '%s' unmount function (context %p)", mnt->fs_name, mnt->fs_context);
         driver_result = driver->unmount(mnt->fs_context);
         if (driver_result != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' failed to unmount '%s' (context %p, err %d)", mnt->fs_name, mnt->mount_point, mnt->fs_context, driver_result);
         } else {
              VFS_LOG("Driver unmount successful for '%s'", mnt->mount_point);
         }
     } else {
         VFS_LOG("Driver '%s' has no unmount function for '%s'. FS context %p may leak.", mnt->fs_name, mnt->mount_point, mnt->fs_context);
     }

     // Remove from mount table (mount_table_remove handles locking and freeing mnt/mp_copy)
     char* mount_point_to_remove = (char*)mnt->mount_point; // Cache before mnt might be freed
     VFS_LOG("Removing '%s' from mount table", mount_point_to_remove);
     int remove_result = mount_table_remove(mount_point_to_remove);
     if (remove_result != FS_SUCCESS) {
          VFS_ERROR("mount_table_remove failed for '%s' (err %d) AFTER driver unmount attempt!", mount_point_to_remove, remove_result);
          return (driver_result != FS_SUCCESS) ? driver_result : remove_result;
     }

     VFS_LOG("Successfully unmounted and removed '%s' from table.", mount_point_to_remove);
     return driver_result; // Return the driver's result (or SUCCESS if driver had no unmount)
 }

 /**
  * @brief Mounts the root filesystem.
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
  * @brief Unmounts the root filesystem.
  */
 int vfs_unmount_root(void) {
     VFS_LOG("vfs_unmount_root: Request to unmount '/'");
     return vfs_unmount_internal("/");
 }

 /**
  * @brief Lists all mounted filesystems.
  */
 void vfs_list_mounts(void) {
     VFS_LOG("--- Mount Table Listing ---");
     mount_table_list();
     VFS_LOG("--- End Mount Table ---");
 }

 /**
  * @brief Shuts down the VFS layer.
  */
 int vfs_shutdown(void) {
     VFS_LOG("Shutting down VFS layer...");
     int final_result = FS_SUCCESS;
     int unmount_attempts = 0;
     const int max_attempts = 100;
     mount_t *current_mount;

     while ((current_mount = mount_table_get_head()) != NULL && unmount_attempts < max_attempts) {
         unmount_attempts++;
         char mp_copy[MAX_PATH_LEN];
         if (current_mount->mount_point) {
             strncpy(mp_copy, current_mount->mount_point, MAX_PATH_LEN - 1);
             mp_copy[MAX_PATH_LEN - 1] = '\0';
         } else {
             strcpy(mp_copy, "[INVALID/NULL Mount Point]");
             VFS_ERROR("VFS Shutdown: Encountered mount entry with NULL mount_point!");
         }

         VFS_LOG("Attempting shutdown unmount for '%s'...", mp_copy);
         int result = vfs_unmount_entry(current_mount);
         if (result != FS_SUCCESS) {
             VFS_ERROR("Failed to unmount '%s' during shutdown (error %d).", mp_copy, result);
             if (final_result == FS_SUCCESS) final_result = result;
             if (mount_table_find(mp_copy) != NULL) { // Check if it's still there
                 VFS_ERROR("CRITICAL: Mount point '%s' still exists after unmount attempt failure!", mp_copy);
             }
         }
     }

     if (unmount_attempts >= max_attempts) {
         VFS_ERROR("VFS Shutdown: Reached max unmount attempts (%d)!", max_attempts);
         if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY;
     }
     if (mount_table_get_head() != NULL) {
        VFS_ERROR("VFS Shutdown: Mount points still remain!");
         if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY;
         mount_table_list();
     }

     // Clear the driver list
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
     driver_list = NULL;
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);

     if (final_result == FS_SUCCESS) { VFS_LOG("VFS shutdown complete"); }
     else { VFS_ERROR("VFS shutdown encountered errors (first error code: %d)", final_result); }
     return final_result;
 }

 /*---------------------------------------------------------------------------
  * File Operations (VFS Public API Implementation with Locking)
  *---------------------------------------------------------------------------*/

 file_t *vfs_open(const char *path, int flags) {
     serial_write("[vfs_open] Enter. Path='"); serial_write(path ? path : "NULL");
     serial_write("', Flags=0x"); serial_print_hex((uint32_t)flags); serial_write("\n");

     // Enhanced path validation
     if (!path) {
         serial_write("[vfs_open] ERROR: NULL path parameter\n");
         return NULL;
     }
     
     // Check for empty path or invalid first character
     if (path[0] == '\0') {
         serial_write("[vfs_open] ERROR: Empty path\n");
         return NULL;
     }
     
     if (path[0] != '/') {
         serial_write("[vfs_open] ERROR: Path must start with '/'\n");
         return NULL;
     }
     
     // Validate path length to prevent buffer overflows  
     size_t path_len = strlen(path);
     if (path_len >= 4096) { // Reasonable path length limit
         serial_printf("[vfs_open] ERROR: Path too long: %zu chars\n", path_len);
         return NULL;
     }

     // 1. Find mount point and driver
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { /* ... error logging ... */ return NULL; }
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { /* ... error logging ... */ return NULL; }
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { /* ... error logging ... */ return NULL; }

     serial_write("[vfs_open] Using mount='"); serial_write(mnt->mount_point);
     serial_write("', driver='"); serial_write(driver->fs_name);
     serial_write("', rel_path='"); serial_write(relative_path); serial_write("'\n");

     if (!driver->open) { /* ... error logging ... */ return NULL; }

     // 2. Call driver's open
     serial_write("[vfs_open] >>> Calling driver->open...\n");
     vnode_t *node = driver->open(mnt->fs_context, relative_path, flags);
     serial_write("[vfs_open] <<< driver->open returned node="); serial_print_hex((uintptr_t)node); serial_write("\n");
     if (!node) { /* ... error logging ... */ return NULL; }

     // 3. Validate vnode
     if (node->fs_driver != driver) {
         /* ... error logging and cleanup ... */
         return NULL;
     }

     // 4. Allocate file handle
     file_t *file = (file_t *)kmalloc(sizeof(file_t));
     if (!file) {
        /* ... error logging and cleanup ... */
        return NULL;
     }

     // 5. Populate file handle
     file->vnode = node;
     file->flags = flags;
     file->offset = 0;
     spinlock_init(&file->lock); // <<< INITIALIZE LOCK >>>

     serial_write("[vfs_open] Success. file="); serial_print_hex((uintptr_t)file); /* ... */ serial_write("\n");
     return file;
 }

 int vfs_close(file_t *file) {
     if (!file) { VFS_ERROR("NULL file handle passed to vfs_close"); return -FS_ERR_INVALID_PARAM; }
     if (!file->vnode) { VFS_ERROR("vfs_close: File handle %p has NULL vnode!", file); kfree(file); return -FS_ERR_BAD_F; }
     if (!file->vnode->fs_driver) { VFS_ERROR("vfs_close: Vnode %p has NULL fs_driver!", file->vnode); kfree(file->vnode); kfree(file); return -FS_ERR_BAD_F; }

     vfs_driver_t* driver = file->vnode->fs_driver;
     VFS_DEBUG_LOG("vfs_close: Closing file handle %p (vnode: %p, driver: %s)", file, file->vnode, driver->fs_name ? driver->fs_name : "[N/A]");

     int result = FS_SUCCESS;
     // --- Acquire Lock (Optional but safer if driver close touches shared vnode state) ---
     // spinlock_acquire_irqsave(&file->lock); // Consider if driver->close needs protection

     if (driver->close) {
         result = driver->close(file); // Driver cleans up vnode->data
         if (result != FS_SUCCESS) { VFS_ERROR("vfs_close: Driver '%s' close failed (err %d)", driver->fs_name, result); }
     } else {
         VFS_WARN("vfs_close: Driver '%s' has no close function. Potential resource leak for vnode->data %p.", driver->fs_name, file->vnode->data);
     }

     // --- Release Lock (if acquired above) ---
     // spinlock_release_irqrestore(&file->lock, irq_flags);

     // VFS layer frees its own structures
     kfree(file->vnode);
     kfree(file); // Free the file struct itself

     return result; // Return result from driver close
 }

 int vfs_read(file_t *file, void *buf, size_t len) {
    // Input validation (as before)
    if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
    if (!buf && len > 0) return -FS_ERR_INVALID_PARAM;
    if (len == 0) return 0;
    if (!file->vnode->fs_driver->read) return -FS_ERR_NOT_SUPPORTED;

    // === Acquire Lock ===
    uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock);

    VFS_DEBUG_LOG("vfs_read: START file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
    int bytes_read = file->vnode->fs_driver->read(file, buf, len); // Driver uses current file->offset

    if (bytes_read > 0) {
        // Check for offset overflow before adding
        if (file->offset > (OFF_T_MAX - bytes_read)) {
             VFS_ERROR("vfs_read: File offset overflow for file %p", file);
             file->offset = OFF_T_MAX; // Clamp offset
        } else {
             file->offset += bytes_read; // Update offset *after* successful read
        }
        VFS_DEBUG_LOG("vfs_read: OK file=%p, read %d bytes, new offset=%ld", file, bytes_read, (long)file->offset);
    } else if (bytes_read == 0) {
        VFS_DEBUG_LOG("vfs_read: EOF file=%p, offset=%ld", file, (long)file->offset);
    } else {
        VFS_ERROR("vfs_read: FAIL file=%p, driver error %d", file, bytes_read);
    }

    // === Release Lock ===
    spinlock_release_irqrestore(&file->lock, irq_flags);
    return bytes_read;
 }

 int vfs_write(file_t *file, const void *buf, size_t len) {
    // Input validation (as before)
    if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
    if (!buf && len > 0) return -FS_ERR_INVALID_PARAM;
    if (len == 0) return 0;
    int access_mode = file->flags & O_ACCMODE;
    if (access_mode != O_WRONLY && access_mode != O_RDWR) {
        VFS_ERROR("vfs_write: File not opened for writing (flags: 0x%lx)", (unsigned long)file->flags);
        return -FS_ERR_PERMISSION_DENIED;
    }
    if (!file->vnode->fs_driver->write) return -FS_ERR_NOT_SUPPORTED;

    // === Acquire Lock ===
    uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock);

    VFS_DEBUG_LOG("vfs_write: START file=%p, offset=%ld, len=%lu", file, (long)file->offset, (unsigned long)len);
    int bytes_written = file->vnode->fs_driver->write(file, buf, len); // Driver uses current file->offset

    if (bytes_written > 0) {
        // Check for offset overflow before adding
        if (file->offset > (OFF_T_MAX - bytes_written)) {
             VFS_ERROR("vfs_write: File offset overflow for file %p", file);
             file->offset = OFF_T_MAX; // Clamp offset
        } else {
             file->offset += bytes_written; // Update offset *after* successful write
        }
        VFS_DEBUG_LOG("vfs_write: OK file=%p, wrote %d bytes, new offset=%ld", file, bytes_written, (long)file->offset);
    } else if (bytes_written == 0) {
        VFS_DEBUG_LOG("vfs_write: Wrote 0 bytes file=%p (requested %lu)", file, (unsigned long)len);
    } else {
        VFS_ERROR("vfs_write: FAIL file=%p, driver error %d", file, bytes_written);
    }

    // === Release Lock ===
    spinlock_release_irqrestore(&file->lock, irq_flags);
    return bytes_written;
 }

 off_t vfs_lseek(file_t *file, off_t offset, int whence) {
    // Input validation (as before)
    if (!file || !file->vnode || !file->vnode->fs_driver) return (off_t)-FS_ERR_BAD_F;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { VFS_ERROR("vfs_lseek: Invalid whence value (%d)", whence); return (off_t)-FS_ERR_INVALID_PARAM; }
    if (!file->vnode->fs_driver->lseek) { return (off_t)-FS_ERR_NOT_SUPPORTED; }

    // === Acquire Lock ===
    uintptr_t irq_flags = spinlock_acquire_irqsave(&file->lock);

    VFS_DEBUG_LOG("vfs_lseek: START file=%p, current=%ld, req offset=%ld, whence=%d",
                  file, (long)file->offset, (long)offset, whence);

    // Driver call uses current file->offset if needed
    off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);

    if (new_offset >= 0) {
        file->offset = new_offset; // Update VFS offset on success
        VFS_DEBUG_LOG("vfs_lseek: OK file=%p, new offset=%ld", file, (long)new_offset);
    } else {
        VFS_ERROR("vfs_lseek: FAIL file=%p, driver error %ld", file, (long)new_offset);
    }

    // === Release Lock ===
    spinlock_release_irqrestore(&file->lock, irq_flags);
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
 /**
  * @brief Create a directory
  * @param path Path to the directory to create
  * @param mode Directory permissions
  * @return 0 on success, negative error code on failure
  */
 int vfs_mkdir(const char *path, mode_t mode) {
     VFS_DEBUG_LOG("vfs_mkdir: Enter. Path='%s', mode=0%o", path ? path : "NULL", mode);
     
     // Validate path
     if (!path || path[0] != '/') {
         VFS_ERROR("vfs_mkdir: Invalid path");
         return -FS_ERR_INVALID_PARAM;
     }
     
     // Find mount point and driver
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) {
         VFS_ERROR("vfs_mkdir: No mount point found for path '%s'", path);
         return -FS_ERR_NOT_FOUND;
     }
     
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) {
         VFS_ERROR("vfs_mkdir: No driver found for filesystem '%s'", mnt->fs_name);
         return -FS_ERR_NOT_FOUND;
     }
     
     // Check if driver supports mkdir
     if (!driver->mkdir) {
         VFS_ERROR("vfs_mkdir: Driver '%s' does not support mkdir operation", driver->fs_name);
         return -FS_ERR_NOT_SUPPORTED;
     }
     
     // Get relative path
     const char *rel_path = get_relative_path(path, mnt);
     
     // Call driver's mkdir function
     int result = driver->mkdir(mnt->fs_context, rel_path, mode);
     
     if (result == 0) {
         VFS_DEBUG_LOG("vfs_mkdir: Successfully created directory '%s'", path);
     } else {
         VFS_ERROR("vfs_mkdir: Failed to create directory '%s', error: %d", path, result);
     }
     
     return result;
 }

 /**
  * @brief Remove a directory
  * @param path Path to the directory to remove
  * @return 0 on success, negative error code on failure
  */
 int vfs_rmdir(const char *path) {
     VFS_DEBUG_LOG("vfs_rmdir: Enter. Path='%s'", path ? path : "NULL");
     
     // Validate path
     if (!path || path[0] != '/') {
         VFS_ERROR("vfs_rmdir: Invalid path");
         return -FS_ERR_INVALID_PARAM;
     }
     
     // Find mount point and driver
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) {
         VFS_ERROR("vfs_rmdir: No mount point found for path '%s'", path);
         return -FS_ERR_NOT_FOUND;
     }
     
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) {
         VFS_ERROR("vfs_rmdir: No driver found for filesystem '%s'", mnt->fs_name);
         return -FS_ERR_NOT_FOUND;
     }
     
     // Check if driver supports rmdir
     if (!driver->rmdir) {
         VFS_ERROR("vfs_rmdir: Driver '%s' does not support rmdir operation", driver->fs_name);
         return -FS_ERR_NOT_SUPPORTED;
     }
     
     // Get relative path
     const char *rel_path = get_relative_path(path, mnt);
     
     // Call driver's rmdir function
     int result = driver->rmdir(mnt->fs_context, rel_path);
     
     if (result == 0) {
         VFS_DEBUG_LOG("vfs_rmdir: Successfully removed directory '%s'", path);
     } else {
         VFS_ERROR("vfs_rmdir: Failed to remove directory '%s', error: %d", path, result);
     }
     
     return result;
 }

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
 