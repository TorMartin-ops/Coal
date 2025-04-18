/**
 * vfs.c - Virtual File System Implementation (Corrected)
 *
 * Provides a unified interface over different filesystem drivers.
 * Uses the global mount list managed by mount_table.c
 *
 * Corrections based on compile log and provided headers:
 * - Replaced all `vnode_driver_t` with `vfs_driver_t` to match vfs.h.
 * - Corrected spinlock definition/usage. Using explicit definition and relying on spinlock_init.
 * - Replaced `mount_table_get_list_head` with `mount_table_get_head`.
 * - Removed fallback for MAX_PATH_LEN, assuming definition in fs_limits.h.
 * - Reverted FS_ERR_NO_ENTRY usage to FS_ERR_NOT_FOUND.
 * - Fixed printf format specifier for file->flags (uint32_t -> %lx). // FIXED
 * - Removed use of undeclared O_DIRECTORY in self-test. // FIXED
 */

 #include "vfs.h"           // Declares vfs_driver_t, file_t, vnode_t etc.
 #include "kmalloc.h"
 #include "terminal.h"      // Kernel logging/printing
 #include "string.h"
 #include "types.h"
 #include "sys_file.h"      // SEEK_*, O_* flags (incl. O_RDONLY etc.)
 #include "fs_errno.h"      // FS_ERR_* codes
 #include "fs_limits.h"     // Should define MAX_PATH_LEN
 #include "mount.h"         // mount_t definition and mount API functions
 #include "mount_table.h"   // Global mount table functions (mount_table_add/find/remove/get_head)
 #include "spinlock.h"      // Spinlock definitions and functions (spinlock_t, spinlock_init, etc.)
 #include <libc/limits.h>   // LONG_MAX, LONG_MIN etc. (Assumed available)
 #include <libc/stddef.h>   // NULL, size_t (Assumed available)
 #include <libc/stdbool.h>  // bool (Assumed available)
 #include <libc/stdarg.h>   // varargs for printf (Assumed available)
 #include <assert.h>        // KERNEL_ASSERT
 
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
 
 // Fallback for MAX_PATH_LEN removed - it MUST be defined in fs_limits.h now.
 
 
 /* Debug macro - define VFS_DEBUG to enable verbose logging */
 // #define VFS_DEBUG 1 // Uncomment for debugging
 
 #ifdef VFS_DEBUG
 #define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
 #define VFS_DEBUG_LOG(fmt, ...) terminal_printf("[VFS DEBUG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 #else
 #define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
 #define VFS_DEBUG_LOG(fmt, ...) do {} while(0)
 #endif
 
 #define VFS_ERROR(fmt, ...) terminal_printf("[VFS ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 
 /* --- Global State --- */
 
 // Linked list of registered filesystem drivers
 static vfs_driver_t *driver_list = NULL; // Use type defined in vfs.h
 
 // Spinlock to protect access to the driver_list
 // Explicit definition as SPINLOCK_INITIALIZER is not provided by the spinlock header.
 // Initialization happens in vfs_init() via spinlock_init().
 #ifdef DEFINE_SPINLOCK // If macro exists, use it (though unlikely given the context)
     DEFINE_SPINLOCK(vfs_driver_lock);
 #else // Otherwise define explicitly
     spinlock_t vfs_driver_lock; // FIXED: Removed "= SPINLOCK_INITIALIZER"
 #endif
 
 
 /* --- Forward Declarations --- */
 
 static int check_driver_validity(vfs_driver_t *driver); // Use correct type
 static mount_t *find_best_mount_for_path(const char *path);
 static const char *get_relative_path(const char *path, mount_t *mnt);
 static int vfs_unmount_internal(mount_t *mnt);
 
 /*---------------------------------------------------------------------------
  * VFS Initialization and Driver Registration
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Initializes the VFS layer. Must be called before any VFS operations.
  */
 void vfs_init(void) {
     spinlock_init(&vfs_driver_lock); // Initialize the spinlock
     driver_list = NULL;
     mount_table_init(); // Ensure mount table is also initialized
     VFS_LOG("Virtual File System initialized");
 }
 
 /**
  * @brief Performs basic checks on a driver structure before registration.
  */
 static int check_driver_validity(vfs_driver_t *driver) { // Use correct type
     if (!driver) return -FS_ERR_INVALID_PARAM;
     if (!driver->fs_name || driver->fs_name[0] == '\0') {
         VFS_ERROR("Driver registration check failed: Missing or empty fs_name");
         return -FS_ERR_INVALID_PARAM;
     }
     // Core functions required
     if (!driver->mount) { VFS_ERROR("Driver '%s' check failed: Missing 'mount' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->open)  { VFS_ERROR("Driver '%s' check failed: Missing 'open' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->close) { VFS_ERROR("Driver '%s' check failed: Missing 'close' function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
 
     // Log missing optional functions
     if (!driver->read)    VFS_LOG("Driver '%s' info: Missing optional 'read'", driver->fs_name);
     if (!driver->write)   VFS_LOG("Driver '%s' info: Missing optional 'write'", driver->fs_name);
     if (!driver->lseek)   VFS_LOG("Driver '%s' info: Missing optional 'lseek'", driver->fs_name);
     if (!driver->readdir) VFS_LOG("Driver '%s' info: Missing optional 'readdir'", driver->fs_name);
     if (!driver->unlink)  VFS_LOG("Driver '%s' info: Missing optional 'unlink'", driver->fs_name);
     if (!driver->unmount) VFS_LOG("Driver '%s' info: Missing optional 'unmount'", driver->fs_name);
 
     return FS_SUCCESS;
 }
 
 /**
  * @brief Registers a filesystem driver with the VFS.
  * @param driver Pointer to the driver structure (vfs_driver_t) to register.
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int vfs_register_driver(vfs_driver_t *driver) { // Use correct type
     int check_result = check_driver_validity(driver);
     if (check_result != FS_SUCCESS) {
         return check_result;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
 
     // Check for duplicates by name
     vfs_driver_t *current = driver_list;
     while (current) {
         if (current->fs_name && strcmp(current->fs_name, driver->fs_name) == 0) {
             spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
             VFS_ERROR("Driver '%s' already registered", driver->fs_name);
             return -FS_ERR_FILE_EXISTS;
         }
         current = current->next;
     }
 
     // Add to head of list
     driver->next = driver_list;
     driver_list = driver;
 
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     VFS_LOG("Registered filesystem driver: %s", driver->fs_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Unregisters a filesystem driver.
  * @param driver Pointer to the driver structure (vfs_driver_t) to unregister.
  * @return FS_SUCCESS on success, negative error code if not found or invalid.
  */
 int vfs_unregister_driver(vfs_driver_t *driver) { // Use correct type
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
          driver->next = NULL;
          return FS_SUCCESS;
      } else {
          VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
          return -FS_ERR_NOT_FOUND;
      }
 }
 
 /**
  * @brief Finds a registered driver by name.
  * @param fs_name The name of the filesystem driver.
  * @return Pointer to the driver structure (vfs_driver_t), or NULL if not found.
  */
 vfs_driver_t *vfs_get_driver(const char *fs_name) { // Use correct type
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
         vfs_driver_t *curr = driver_list; // Use correct type
         int count = 0;
         while (curr) {
             VFS_LOG("  %d: %s", ++count, curr->fs_name ? curr->fs_name : "[INVALID NAME]");
             curr = curr->next;
         }
         if (count == 0) {
              VFS_LOG("  (list head not null, but no drivers found - list corrupted?)");
         } else {
              VFS_LOG("Total drivers: %d", count);
         }
     }
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 }
 
 /*---------------------------------------------------------------------------
  * Mount Table Helpers
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Helper to create and add an entry to the global mount table.
  */
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) { // Use correct type
     if (!mp || !fs || !ctx || !drv) {
         VFS_ERROR("Invalid parameters to add_mount_entry (mp=%p, fs=%p, ctx=%p, drv=%p)", mp, fs, ctx, drv);
         return -FS_ERR_INVALID_PARAM;
     }
     size_t mp_len = strlen(mp);
     if (mp_len == 0 || mp_len >= MAX_PATH_LEN) { // Uses MAX_PATH_LEN from fs_limits.h
         VFS_ERROR("Invalid mount point length: %lu (max: %d)", (long unsigned int)mp_len, MAX_PATH_LEN);
         return -FS_ERR_NAMETOOLONG;
     }
     if (mp[0] != '/') {
         VFS_ERROR("Mount point '%s' must be absolute", mp);
         return -FS_ERR_INVALID_PARAM;
     }
 
     char *mp_copy = (char *)kmalloc(mp_len + 1);
     if (!mp_copy) {
         VFS_ERROR("Failed to allocate memory for mount point path copy ('%s')", mp);
         return -FS_ERR_OUT_OF_MEMORY;
     }
     strcpy(mp_copy, mp);
 
     mount_t *mnt_to_add = (mount_t *)kmalloc(sizeof(mount_t));
     if (!mnt_to_add) {
         kfree(mp_copy);
         VFS_ERROR("Failed to allocate memory for mount_t structure for '%s'", mp);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     mnt_to_add->mount_point = mp_copy;
     mnt_to_add->fs_name = fs;
     mnt_to_add->fs_context = ctx;
     mnt_to_add->next = NULL;
 
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
  * @brief Finds the most specific mount entry for a given path (longest prefix match).
  */
  static mount_t *find_best_mount_for_path(const char *path) {
      if (!path || path[0] != '/') {
          VFS_DEBUG_LOG("find_best_mount_for_path: Invalid path '%s'", path ? path : "NULL");
          return NULL;
      }
      VFS_DEBUG_LOG("find_best_mount_for_path: Searching for path: '%s'", path);
 
      mount_t *best_match = NULL;
      size_t best_len = 0;
      // Use the corrected function name from mount_table.h
      mount_t *curr = mount_table_get_head();
 
      while (curr) {
          if (!curr->mount_point || curr->mount_point[0] == '\0') {
              VFS_ERROR("Mount table iteration encountered invalid mount_point in entry %p!", curr);
              curr = curr->next;
              continue;
          }
 
          size_t current_mount_point_len = strlen(curr->mount_point);
          VFS_DEBUG_LOG("  Checking mount point: '%s' (len %lu)", curr->mount_point, (long unsigned int)current_mount_point_len);
 
          if (strncmp(path, curr->mount_point, current_mount_point_len) == 0) {
              bool is_exact_match = (path[current_mount_point_len] == '\0');
              bool is_subdir_match = (path[current_mount_point_len] == '/');
              bool is_root_mount = (current_mount_point_len == 1 && curr->mount_point[0] == '/');
 
              // If it's the root mount "/", any path matches as a subdirectory (unless exact)
              if (is_root_mount && !is_exact_match && path[1] != '\0') {
                  is_subdir_match = true;
              }
 
              if (is_exact_match || is_subdir_match) {
                  if (current_mount_point_len >= best_len) {
                      VFS_DEBUG_LOG("    -> Found new best match '%s' (len %lu >= %lu)",
                                    curr->mount_point, (long unsigned int)current_mount_point_len, (long unsigned int)best_len);
                      best_match = curr;
                      best_len = current_mount_point_len;
                  }
              }
          }
          curr = curr->next;
      }
 
      if (best_match) {
          VFS_DEBUG_LOG("find_best_mount_for_path: Found best match: '%s'", best_match->mount_point);
      } else {
          VFS_LOG("find_best_mount_for_path: No suitable mount point found for path '%s'.", path);
      }
      return best_match;
  }
 
 
 /**
  * @brief Calculates the path relative to a mount point.
  */
  static const char *get_relative_path(const char *path, mount_t *mnt) {
      if (!path || !mnt || !mnt->mount_point) {
          VFS_ERROR("get_relative_path: Invalid input (path=%p, mnt=%p)", path, mnt);
          return NULL;
      }
 
      size_t mount_point_len = strlen(mnt->mount_point);
      const char *relative_path_start = path + mount_point_len;
 
      // Handle root mount case ("/")
      if (mount_point_len == 1 && mnt->mount_point[0] == '/') {
          // If path is exactly "/", relative is "/". If path is "/foo", relative is "/foo".
          return (*relative_path_start == '\0') ? "/" : relative_path_start;
      }
 
      // Handle other mount points ("/mnt", "/mnt/data")
      if (*relative_path_start == '\0') {
          // Path exactly matches mount point ("/mnt"), relative is "/"
          return "/";
      } else if (*relative_path_start == '/') {
          // Path is a subdirectory ("/mnt/file"), relative is "/file"
          return relative_path_start;
      } else {
          // This case should ideally not happen if find_best_mount_for_path works correctly
          VFS_ERROR("Internal error: Invalid prefix match in get_relative_path for '%s' on '%s'", path, mnt->mount_point);
          return NULL;
      }
  }
 
 /*---------------------------------------------------------------------------
  * Mount / Unmount Operations (VFS Internal Implementation)
  * Note: vfs.h declares vfs_mount_root/vfs_unmount_root, these seem like internal helpers.
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Mounts a filesystem (VFS internal implementation).
  * Likely called by vfs_mount_root.
  */
 int vfs_mount(const char *mp, const char *fs, const char *dev) {
      if (!mp || !fs || !dev) { VFS_ERROR("vfs_mount: Invalid parameters"); return -FS_ERR_INVALID_PARAM; }
      if (mp[0] != '/') { VFS_ERROR("Mount point '%s' must be absolute", mp); return -FS_ERR_INVALID_PARAM; }
      VFS_LOG("VFS internal mount request: mp='%s', fs='%s', dev='%s'", mp, fs, dev);
 
      if (mount_table_find(mp) != NULL) {
          VFS_ERROR("Mount point '%s' already in use", mp);
          return -FS_ERR_BUSY;
      }
 
      vfs_driver_t *driver = vfs_get_driver(fs); // Use correct type
      if (!driver) { VFS_ERROR("Filesystem driver '%s' not found", fs); return -FS_ERR_NOT_FOUND; }
      if (!driver->mount) { VFS_ERROR("Driver '%s' exists but has no mount function", fs); return -FS_ERR_NOT_SUPPORTED; }
 
      VFS_LOG("Calling driver '%s' mount function for device '%s'", fs, dev);
      void *fs_context = driver->mount(dev);
      if (!fs_context) {
          VFS_ERROR("Driver '%s' mount function failed for device '%s'", fs, dev);
          return -FS_ERR_MOUNT;
      }
      VFS_LOG("Driver mount successful, context=%p", fs_context);
 
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
  * Likely called by vfs_unmount_root or vfs_shutdown.
  */
 int vfs_unmount(const char *mp) {
     if (!mp || mp[0] != '/') { VFS_ERROR("vfs_unmount: Invalid mount point '%s'", mp ? mp : "NULL"); return -FS_ERR_INVALID_PARAM; }
     VFS_LOG("VFS internal unmount request: mp='%s'", mp);
 
     mount_t *mnt = mount_table_find(mp);
     if (!mnt) {
         VFS_ERROR("Mount point '%s' not found for unmount", mp);
         return -FS_ERR_NOT_FOUND;
     }
 
     // Check if busy (if other filesystems are mounted underneath this one)
     bool busy = false;
     size_t mp_len = strlen(mp);
     mount_t *curr = mount_table_get_head(); // Use correct function
     while(curr) {
         if (curr != mnt && curr->mount_point && curr->mount_point[0] == '/') {
             size_t curr_len = strlen(curr->mount_point);
             // Check if curr->mount_point starts with mp and is longer
             if (curr_len > mp_len && strncmp(curr->mount_point, mp, mp_len) == 0) {
                 // Need to ensure it's a subdirectory, e.g. /mnt/data vs /mnt
                 // If mp is "/", any other mount makes it busy.
                 // If mp is "/mnt", then "/mnt/data" makes it busy, but "/mntABC" does not.
                 bool is_root_mount = (mp_len == 1 && mp[0] == '/');
                 if (is_root_mount || curr->mount_point[mp_len] == '/') {
                     VFS_ERROR("Cannot unmount '%s': Busy (nested mount found: '%s')", mp, curr->mount_point);
                     busy = true;
                     break;
                 }
             }
         }
         curr = curr->next;
     }
 
     if (busy) {
         return -FS_ERR_BUSY;
     }
 
     // Perform the actual unmount using the internal helper
     return vfs_unmount_internal(mnt);
  }
 
 
 /**
  * @brief Internal helper to perform unmount logic given a mount_t entry.
  */
 static int vfs_unmount_internal(mount_t *mnt) {
     if (!mnt || !mnt->fs_name || !mnt->mount_point || !mnt->fs_context) {
         VFS_ERROR("vfs_unmount_internal: Invalid mount_t provided (%p)", mnt);
         return -FS_ERR_INTERNAL;
     }
     VFS_LOG("Performing internal unmount for '%s' (%s)", mnt->mount_point, mnt->fs_name);
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name); // Use correct type
     int driver_result = FS_SUCCESS;
 
     if (!driver) {
         VFS_ERROR("CRITICAL INCONSISTENCY: Driver '%s' not found during unmount for '%s'!", mnt->fs_name, mnt->mount_point);
         // Can't call driver unmount, but still try to remove from mount table
         driver_result = -FS_ERR_INTERNAL;
     } else if (driver->unmount) {
         VFS_LOG("Calling driver '%s' unmount function (context %p)", mnt->fs_name, mnt->fs_context);
         driver_result = driver->unmount(mnt->fs_context);
         if (driver_result != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' failed to unmount '%s' (context %p, err %d)", mnt->fs_name, mnt->mount_point, mnt->fs_context, driver_result);
             // Don't proceed with removing from mount table if driver failed?
             // Or remove anyway to signal inconsistency? Let's try removing anyway.
         } else {
              VFS_LOG("Driver unmount successful for '%s'", mnt->mount_point);
         }
     } else {
         // Driver exists but no unmount function. Log potential leak.
         VFS_LOG("Driver '%s' has no unmount function for '%s'. FS context %p may leak.", mnt->fs_name, mnt->mount_point, mnt->fs_context);
     }
 
     // Always attempt to remove from mount table after trying driver unmount
     char* mount_point_to_remove = (char*)mnt->mount_point; // Cache before mnt might be invalid
     VFS_LOG("Removing '%s' from mount table", mount_point_to_remove);
     int remove_result = mount_table_remove(mount_point_to_remove);
     if (remove_result != FS_SUCCESS) {
          VFS_ERROR("mount_table_remove failed for '%s' (err %d) AFTER driver unmount attempt!", mount_point_to_remove, remove_result);
          // Prioritize returning the driver error if it occurred, otherwise return table error
          return (driver_result != FS_SUCCESS) ? driver_result : remove_result;
     }
 
     VFS_LOG("Successfully unmounted and removed '%s' from table.", mount_point_to_remove);
     return driver_result; // Return the result of the driver's unmount attempt
 }
 
 
 /**
  * @brief Lists all mounted filesystems via mount_table.
  */
 void vfs_list_mounts(void) {
     VFS_LOG("--- Mount Table Listing ---");
     mount_table_list();
     VFS_LOG("--- End Mount Table ---");
 }
 
 /**
  * @brief Shuts down the VFS layer, attempting to unmount all filesystems.
  */
 int vfs_shutdown(void) {
     VFS_LOG("Shutting down VFS layer...");
     int final_result = FS_SUCCESS;
     int unmount_attempts = 0;
     const int max_attempts = 100; // Safety break for infinite loops
 
     mount_t *current_mount;
     // Repeatedly get the head and try to unmount it. This handles dependencies better.
     // Use corrected function call from mount_table.h
     while ((current_mount = mount_table_get_head()) != NULL && unmount_attempts < max_attempts) {
         unmount_attempts++;
         char mp_copy[MAX_PATH_LEN];
         // Ensure mount_point is valid before copying
         if (current_mount->mount_point) {
             strncpy(mp_copy, current_mount->mount_point, MAX_PATH_LEN - 1);
             mp_copy[MAX_PATH_LEN - 1] = '\0';
         } else {
             strcpy(mp_copy, "[INVALID/NULL Mount Point]");
             // Should not happen if mount table is consistent
             VFS_ERROR("VFS Shutdown: Encountered mount entry with NULL mount_point!");
             // Attempting to remove based on the entry pointer itself might be needed
             // For now, just log and hope the loop terminates. This indicates a deeper issue.
             // We might skip this entry or try a specific removal strategy if this occurs.
             // Let's try unmount_internal which handles NULL checks inside.
         }
 
 
         VFS_LOG("Attempting shutdown unmount for '%s'...", mp_copy);
         int result = vfs_unmount_internal(current_mount); // Use helper which removes from table
         if (result != FS_SUCCESS) {
             VFS_ERROR("Failed to unmount '%s' during shutdown (error %d). Possible busy state or driver error.", mp_copy, result);
             // Store the first error encountered
             if (final_result == FS_SUCCESS) {
                 final_result = result;
             }
             // If unmount failed, the entry might still be in the table.
             // The loop should eventually hit max_attempts if it cannot be unmounted.
              if (mount_table_find(mp_copy) != NULL) {
                  VFS_ERROR("CRITICAL: Mount point '%s' still exists after unmount attempt failure!", mp_copy);
                  // Break or continue? Continuing might lead to infinite loop if always busy.
                  // Let's rely on max_attempts for now.
              }
         }
     }
 
      if (unmount_attempts >= max_attempts) {
          VFS_ERROR("VFS Shutdown: Reached max unmount attempts (%d), potential mount table issues or busy mounts!", max_attempts);
          if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY; // Indicate problem
      }
      if (mount_table_get_head() != NULL) {
         VFS_ERROR("VFS Shutdown: Mount points still remain after shutdown attempt!");
          if (final_result == FS_SUCCESS) final_result = -FS_ERR_BUSY; // Indicate problem
          mount_table_list(); // List remaining mounts for debugging
      }
 
 
     // Clear the driver list (assume drivers don't need explicit cleanup beyond unmount)
     uintptr_t irq_flags = spinlock_acquire_irqsave(&vfs_driver_lock);
     // TODO: Should we free driver structs if dynamically allocated? Assume not for now.
     driver_list = NULL;
     spinlock_release_irqrestore(&vfs_driver_lock, irq_flags);
 
     // TODO: Potentially call mount_table_destroy() if it exists to clean up table resources.
 
     if (final_result == FS_SUCCESS) {
         VFS_LOG("VFS shutdown complete");
     } else {
         VFS_ERROR("VFS shutdown encountered errors (first error code: %d)", final_result);
     }
     return final_result;
 }
 
 /*---------------------------------------------------------------------------
  * File Operations (VFS Public API Implementation)
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Opens or creates a file/directory via the appropriate driver.
  */
 file_t *vfs_open(const char *path, int flags) {
     if (!path || path[0] != '/') { VFS_ERROR("vfs_open: Invalid path '%s'", path ? path : "NULL"); return NULL; }
     VFS_DEBUG_LOG("vfs_open: path='%s', flags=0x%x", path, flags);
 
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { VFS_ERROR("vfs_open: No mount point found for path '%s'", path); return NULL; }
 
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name); // Use correct type
     if (!driver) { VFS_ERROR("vfs_open: Driver '%s' not found for mount point '%s'", mnt->fs_name, mnt->mount_point); return NULL; }
 
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("vfs_open: Failed to calculate relative path for '%s' on '%s'", path, mnt->mount_point); return NULL; }
 
     VFS_DEBUG_LOG("vfs_open: Using mount '%s', driver '%s', relative path '%s'",
                   mnt->mount_point, driver->fs_name, relative_path);
 
     if (!driver->open) { VFS_ERROR("Driver '%s' does not support open operation", driver->fs_name); return NULL; }
 
     // Call the driver's open function
     vnode_t *node = driver->open(mnt->fs_context, relative_path, flags);
     if (!node) {
         VFS_DEBUG_LOG("vfs_open: Driver '%s' failed to open relative path '%s'", driver->fs_name, relative_path);
         // Driver open failed, return NULL. Driver is responsible for internal cleanup.
         return NULL;
     }
 
     // Sanity check: Driver MUST set the fs_driver pointer in the returned vnode
     // KERNEL_ASSERT might be too harsh, maybe log error and clean up?
     if (node->fs_driver != driver) {
         VFS_ERROR("CRITICAL: Driver '%s' open implementation did NOT set vnode->fs_driver correctly!", driver->fs_name);
         // We don't know which driver SHOULD handle cleanup now. This is bad.
         // Attempt cleanup with the *expected* driver, but this is risky.
         if (driver->close) {
              // Create a temporary file struct just for closing the rogue vnode
              file_t temp_file = { .vnode = node, .flags = flags, .offset = 0 };
              node->fs_driver = driver; // Force set it for the close call
              driver->close(&temp_file);
         }
         kfree(node); // Free the vnode struct itself (assuming VFS allocated it or driver expects VFS to free)
                      // OR: Assume driver->open allocated vnode and driver->close frees it.
                      // Let's assume driver->close handles freeing node->data, and VFS frees node itself.
         return NULL; // Abort
     }
 
     // Allocate the VFS file structure
     file_t *file = (file_t *)kmalloc(sizeof(file_t));
     if (!file) {
         VFS_ERROR("vfs_open: Failed kmalloc for file_t for path '%s'", path);
         VFS_LOG("vfs_open: Cleaning up vnode %p via driver close due to file_t allocation failure", node);
         // We successfully opened the vnode via the driver, but failed to allocate the VFS file_t.
         // We need to call the driver's close function to release driver resources.
         if (driver->close) {
             // Use a temporary file_t on the stack to pass context to driver->close
             file_t temp_file = { .vnode = node, .flags = flags, .offset = 0 };
             driver->close(&temp_file); // Driver should clean up node->data
         } else {
             VFS_ERROR("vfs_open: Driver '%s' has no close function! Cannot clean up vnode %p / node->data %p after file_t allocation failure!",
                       driver->fs_name, node, node->data);
             // Resource leak (node->data) likely if no close function.
         }
         // Assume VFS layer is responsible for freeing the vnode struct itself,
         // while driver->close is responsible for freeing node->data.
         kfree(node);
         return NULL;
     }
 
     // Populate the VFS file structure
     file->vnode = node;
     file->flags = flags;
     file->offset = 0; // Standard practice: open sets offset to 0
 
     VFS_DEBUG_LOG("vfs_open: Success '%s' (file: %p, vnode: %p, ctx: %p)", path, file, node, node->data);
     return file;
 }
 
 /**
  * @brief Closes an open file handle. Calls the underlying driver's close.
  */
 int vfs_close(file_t *file) {
      if (!file) { VFS_ERROR("NULL file handle passed to vfs_close"); return -FS_ERR_INVALID_PARAM; }
      if (!file->vnode) {
          // This might happen if called twice on the same file handle
          VFS_ERROR("vfs_close: File handle %p has NULL vnode! Potential double close or corruption.", file);
          kfree(file); // Free the handle itself, as vnode is already gone
          return -FS_ERR_BAD_F;
      }
      if (!file->vnode->fs_driver) {
          VFS_ERROR("vfs_close: File handle %p vnode %p has NULL fs_driver! Vnode possibly corrupted.", file, file->vnode);
          kfree(file->vnode); // Free vnode struct
          kfree(file);       // Free handle
          return -FS_ERR_BAD_F; // Indicate bad file descriptor
      }
      if (!file->vnode->fs_driver->close) {
          // If there's no close function, we might just free memory, but resources inside driver might leak.
          VFS_ERROR("vfs_close: Driver '%s' has no close function. Potential resource leak for vnode->data %p.",
                    file->vnode->fs_driver->fs_name, file->vnode->data);
          kfree(file->vnode); // Free vnode struct
          kfree(file);       // Free handle
          // Return success? Or an error? Let's return success as VFS did its part.
          return FS_SUCCESS;
      }
 
 
      vfs_driver_t* driver = file->vnode->fs_driver; // Use correct type
      VFS_DEBUG_LOG("vfs_close: Closing file handle %p (vnode: %p, driver: %s)",
                    file, file->vnode, driver->fs_name ? driver->fs_name : "[N/A]");
 
      // Call the driver's close function. It is responsible for cleaning up
      // filesystem-specific resources associated with the open file (e.g., file->vnode->data).
      int result = driver->close(file);
 
      if (result != FS_SUCCESS) {
         VFS_ERROR("vfs_close: Driver '%s' close failed for file %p (err %d)", driver->fs_name, file, result);
         // Even if driver close failed, we should free the VFS resources.
      }
 
      // VFS layer frees the vnode_t structure and the file_t structure.
      // Driver's close function was responsible for freeing file->vnode->data.
      kfree(file->vnode);
      kfree(file);
 
      return result; // Return the result from the driver's close operation
  }
 
 /**
  * @brief Reads data from an open file via the appropriate driver.
  */
 int vfs_read(file_t *file, void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
     if (!buf && len > 0) return -FS_ERR_INVALID_PARAM; // Reading 0 bytes is okay, reading into NULL is not.
     if (len == 0) return 0; // Read 0 bytes successfully.
     if (!file->vnode->fs_driver->read) return -FS_ERR_NOT_SUPPORTED; // Driver doesn't implement read
 
     VFS_DEBUG_LOG("vfs_read: file=%p, offset=%ld, len=%lu", file, file->offset, (long unsigned int)len);
     int bytes_read = file->vnode->fs_driver->read(file, buf, len);
 
     // If read was successful (returned >= 0), update the file offset.
     if (bytes_read > 0) {
         // Check for potential overflow? off_t is long.
         // If file->offset + bytes_read > UIAOS_LONG_MAX -> Error? Clamp?
         // For now, assume it fits.
         file->offset += bytes_read;
         VFS_DEBUG_LOG("vfs_read: Read %d bytes, new offset=%ld", bytes_read, file->offset);
     } else if (bytes_read == 0) {
         // End Of File (EOF) or read completed successfully reading zero bytes (e.g., reading at EOF).
         VFS_DEBUG_LOG("vfs_read: Read 0 bytes (EOF or request satisfied)");
     } else {
         // Negative return value indicates an error.
         VFS_ERROR("vfs_read: Driver read failed (err %d) for file %p", bytes_read, file);
     }
     return bytes_read; // Return bytes read or error code from driver
 }
 
 /**
  * @brief Writes data to an open file via the appropriate driver.
  */
 int vfs_write(file_t *file, const void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return -FS_ERR_BAD_F;
     if (!buf && len > 0) return -FS_ERR_INVALID_PARAM; // Writing 0 bytes is okay, writing from NULL is not.
     if (len == 0) return 0; // Wrote 0 bytes successfully.
 
     // Check if file was opened with write permissions
     // Assuming O_WRONLY and O_RDWR are defined in sys_file.h
     if (!(file->flags & (O_WRONLY | O_RDWR))) {
          // FIXED: Changed %x to %lx for uint32_t flags
          VFS_ERROR("vfs_write: File not opened with write access (flags: 0x%lx)", (long unsigned int)file->flags);
          return -FS_ERR_PERMISSION_DENIED; // Or -FS_ERR_BAD_F? EBADF is often used here.
     }
 
     if (!file->vnode->fs_driver->write) return -FS_ERR_NOT_SUPPORTED; // Driver doesn't implement write
 
     VFS_DEBUG_LOG("vfs_write: file=%p, offset=%ld, len=%lu", file, file->offset, (long unsigned int)len);
     int bytes_written = file->vnode->fs_driver->write(file, buf, len);
 
     // If write was successful (returned >= 0), update the file offset.
     if (bytes_written > 0) {
         // Potential overflow check similar to read?
         file->offset += bytes_written;
         VFS_DEBUG_LOG("vfs_write: Wrote %d bytes, new offset=%ld", bytes_written, file->offset);
     } else if (bytes_written == 0) {
         // Could mean success writing zero bytes, or e.g., disk full if len > 0.
         // Driver should return error code in case of failure. Assume 0 means success.
         VFS_DEBUG_LOG("vfs_write: Wrote 0 bytes (requested %lu)", (long unsigned int)len);
     } else {
         // Negative return value indicates an error.
         VFS_ERROR("vfs_write: Driver write failed (err %d) for file %p", bytes_written, file);
     }
     return bytes_written; // Return bytes written or error code from driver
 }
 
 /**
  * @brief Changes the file offset via the appropriate driver.
  */
 off_t vfs_lseek(file_t *file, off_t offset, int whence) {
     if (!file || !file->vnode || !file->vnode->fs_driver) return (off_t)-FS_ERR_BAD_F;
     // Check if whence is valid
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
          VFS_ERROR("vfs_lseek: Invalid whence value (%d)", whence);
          return (off_t)-FS_ERR_INVALID_PARAM;
     }
     if (!file->vnode->fs_driver->lseek) {
         // If driver doesn't support lseek, maybe emulate simple cases?
         // POSIX allows lseek on non-seekable devices to return ESPIPE.
         // For now, just return not supported.
         return (off_t)-FS_ERR_NOT_SUPPORTED;
     }
 
     VFS_DEBUG_LOG("vfs_lseek: file=%p, vnode=%p, offset=%ld, whence=%d",
                   file, file->vnode, offset, whence);
 
     // Call the driver's lseek function
     off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);
 
     // Driver returns the new absolute offset on success, or negative error code.
     if (new_offset >= 0) {
         // Update the VFS file handle's offset to match the driver's result.
         file->offset = new_offset;
         VFS_DEBUG_LOG("vfs_lseek: Seek successful for file=%p, new offset=%ld", file, new_offset);
     } else {
         // Driver returned an error. Don't update VFS offset.
         VFS_ERROR("vfs_lseek: Driver lseek failed for file=%p (err %ld)", file, new_offset);
     }
     // Return the new offset or error code from the driver.
     return new_offset;
 }
 
 /**
  * @brief Reads a directory entry via the appropriate driver.
  * Note: This differs from POSIX readdir. It fetches entry by index.
  */
 int vfs_readdir(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index) {
     if (!dir_file || !d_entry_out) return -FS_ERR_INVALID_PARAM;
     if (!dir_file->vnode || !dir_file->vnode->fs_driver) return -FS_ERR_BAD_F;
     // TODO: Add check: Is this actually a directory? (Requires stat or type flag)
     // Could check file->flags if O_DIRECTORY was required/set during open.
     // Example: if (!(dir_file->flags & O_DIRECTORY)) return -FS_ERR_NOT_DIR;
 
     if (!dir_file->vnode->fs_driver->readdir) return -FS_ERR_NOT_SUPPORTED;
 
     VFS_DEBUG_LOG("vfs_readdir: dir_file=%p, index=%lu", dir_file, (long unsigned int)entry_index);
     int result = dir_file->vnode->fs_driver->readdir(dir_file, d_entry_out, entry_index);
 
     // Driver should return FS_SUCCESS (0) on success,
     // -FS_ERR_EOF or -FS_ERR_NOT_FOUND when index is out of bounds,
     // or other negative error code on failure.
     if (result == FS_SUCCESS) {
         // TODO: Ensure driver filled d_entry_out->d_name and possibly d_type
         VFS_DEBUG_LOG("vfs_readdir: Success for index %lu, name='%s'", (long unsigned int)entry_index, d_entry_out->d_name);
     } else if (result == -FS_ERR_EOF) { // Standard EOF check (End of Directory)
         VFS_DEBUG_LOG("vfs_readdir: End of directory reached at index %lu", (long unsigned int)entry_index);
     } else if (result == -FS_ERR_NOT_FOUND) { // Check for NOT_FOUND (e.g., index invalid)
         VFS_DEBUG_LOG("vfs_readdir: Entry not found at index %lu", (long unsigned int)entry_index);
     } else {
         // Other driver error
          VFS_ERROR("vfs_readdir: Driver readdir failed for dir_file %p, index %lu (err %d)", dir_file, (long unsigned int)entry_index, result);
     }
     return result; // Return result from driver
 }
 
 /**
  * @brief Deletes a name from the filesystem via the appropriate driver.
  * Can delete files or empty directories (driver determines).
  */
 int vfs_unlink(const char *path) {
     if (!path || path[0] != '/') { VFS_ERROR("vfs_unlink: Invalid path '%s'", path ? path : "NULL"); return -FS_ERR_INVALID_PARAM; }
     VFS_DEBUG_LOG("vfs_unlink: path='%s'", path);
 
     // Find the mount point governing this path
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { VFS_ERROR("vfs_unlink: No mount point found for path '%s'", path); return -FS_ERR_NOT_FOUND; /* Or perhaps -FS_ERR_INVALID_PARAM? */ }
 
     // Get the driver for this mount point
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name); // Use correct type
     if (!driver) { VFS_ERROR("vfs_unlink: Driver '%s' not found for mount '%s'", mnt->fs_name, mnt->mount_point); return -FS_ERR_INTERNAL; }
 
     // Calculate the path relative to the mount point
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("vfs_unlink: Failed to calculate relative path for '%s'", path); return -FS_ERR_INTERNAL; }
 
     // Check if the driver supports the unlink operation
     if (!driver->unlink) return -FS_ERR_NOT_SUPPORTED;
 
     VFS_DEBUG_LOG("vfs_unlink: Using mount '%s', driver '%s', relative path '%s'",
                   mnt->mount_point, driver->fs_name, relative_path);
 
     // Call the driver's unlink function
     int result = driver->unlink(mnt->fs_context, relative_path);
 
     if (result == FS_SUCCESS) {
         VFS_LOG("vfs_unlink: Driver successfully unlinked '%s' relative to '%s'", relative_path, mnt->mount_point);
     } else {
         VFS_ERROR("vfs_unlink: Driver failed to unlink '%s' (err %d)", path, result);
     }
     return result; // Return result from driver
 }
 
 
 /*---------------------------------------------------------------------------
  * VFS Status and Utility Functions
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Checks if the VFS is initialized and has a root filesystem mounted.
  */
 bool vfs_is_ready(void) {
     // Check if the root mount point "/" exists in the table
     mount_t *root_mount = mount_table_find("/");
     return (root_mount != NULL);
  }
 
 /**
  * @brief Performs a basic self-test of the VFS (e.g., opening root).
  */
  int vfs_self_test(void) {
      VFS_LOG("Running VFS self-test...");
      if (!vfs_is_ready()) {
          VFS_ERROR("VFS self-test FAILED: VFS not ready (root '/' not mounted)");
          return -FS_ERR_NOT_INIT; // Or appropriate error
      }
 
      VFS_LOG("VFS self-test: Attempting to open root directory '/'...");
      // Open root directory read-only
      // FIXED: Removed O_DIRECTORY as it's not defined in sys_file.h
      file_t *root_dir = vfs_open("/", O_RDONLY);
      if (!root_dir) {
          VFS_ERROR("VFS self-test FAILED: vfs_open failed for root directory '/'");
          return -FS_ERR_IO; // Or error returned by vfs_open
      }
      VFS_LOG("VFS self-test: Root directory opened successfully (file: %p).", root_dir);
 
      // Optional: Try reading first entry?
      // struct dirent entry;
      // int read_res = vfs_readdir(root_dir, &entry, 0);
      // ... check read_res ...
 
      VFS_LOG("VFS self-test: Attempting to close root directory...");
      int close_result = vfs_close(root_dir);
      if (close_result != FS_SUCCESS) {
          VFS_ERROR("VFS self-test FAILED: vfs_close failed for root directory (code: %d)", close_result);
          return close_result;
      }
      VFS_LOG("VFS self-test: Root directory closed successfully.");
 
      VFS_LOG("VFS self-test PASSED");
      return FS_SUCCESS;
  }
 
 /**
  * @brief Checks if a path exists by attempting to open it read-only.
  * Note: This doesn't distinguish between files and directories unless O_DIRECTORY is used/checked.
  */
  bool vfs_path_exists(const char *path) {
      if (!path) return false;
      VFS_DEBUG_LOG("vfs_path_exists: Checking '%s'", path);
      // Attempt to open the path read-only.
      file_t *file = vfs_open(path, O_RDONLY);
      if (!file) {
          // vfs_open returning NULL typically means not found or permission error.
          VFS_DEBUG_LOG("vfs_path_exists: vfs_open failed for '%s', path does not exist or is inaccessible.", path);
          return false;
      }
      // If open succeeded, the path exists. Close the handle.
      VFS_DEBUG_LOG("vfs_path_exists: vfs_open succeeded for '%s', path exists.", path);
      vfs_close(file); // Ignore close result for existence check
      return true;
  }
 
 /**
  * @brief Dumps VFS debug information (drivers, mounts) to the kernel log.
  */
  void vfs_debug_dump(void) {
     VFS_LOG("========== VFS DEBUG INFORMATION ==========");
     vfs_list_drivers(); // Dump registered drivers
     vfs_list_mounts();  // Dump mounted filesystems
     // Add more debug info if needed (e.g., open file table, vnode cache stats)
     VFS_LOG("==========================================");
  }