/**
 * vfs.c - Virtual File System Implementation
 *
 * Uses the global mount list managed by mount_table.c
 */

 #include "vfs.h"
 #include "kmalloc.h"
 #include "terminal.h"
 #include "string.h"
 #include "types.h"
 #include "sys_file.h"
 #include "fs_errno.h"
 #include "mount.h"          // Include for mount_t definition
 #include "mount_table.h"    // Include for global mount table functions
 
 /* Define SEEK macros if not already defined elsewhere */
 #ifndef SEEK_SET
 #define SEEK_SET 0
 #endif
 #ifndef SEEK_CUR
 #define SEEK_CUR 1
 #endif
 #ifndef SEEK_END
 #define SEEK_END 2
 #endif
 
 /* Debug macro - define VFS_DEBUG to enable verbose logging */
 #define VFS_DEBUG 1 // Keep debugging enabled for now
 
 #ifdef VFS_DEBUG
 #define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
 #define VFS_DEBUG_LOG(fmt, ...) terminal_printf("[VFS DEBUG] " fmt "\n", ##__VA_ARGS__)
 #else
 #define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
 #define VFS_DEBUG_LOG(fmt, ...) do {} while(0)
 #endif
 
 #define VFS_ERROR(fmt, ...) terminal_printf("[VFS ERROR] " fmt "\n", ##__VA_ARGS__)
 
 /* Global driver list head - Linked list of registered filesystem drivers */
 static vfs_driver_t *driver_list = NULL;
 
 /* Forward declarations for internal helper functions */
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv);
 static mount_t *find_best_mount_for_path(const char *path); // Changed return type to mount_t*
 static const char *get_relative_path(const char *path, mount_t *mnt); // Changed param type to mount_t*
 static int check_driver_validity(vfs_driver_t *driver);
 
 // Helper to get the head of the global mount list (assumes find(NULL) works or modify mount_table.c)
 // Alternatively, modify mount_table.c to expose a get_head function.
 static mount_t *get_mount_list_head() {
     // This is a conceptual placeholder. You might need to adjust mount_table.c
     // to provide a direct way to get the list head for iteration.
     // If mount_table_find(NULL) doesn't work, iterate differently or add a getter.
     return mount_table_find(NULL); // Assuming this works as a trick for now
 }
 
 
 /*---------------------------------------------------------------------------
  * VFS Initialization and Driver Registration
  *---------------------------------------------------------------------------*/
 void vfs_init(void) {
     // No local mount_table to clear
     driver_list = NULL;
     VFS_LOG("Virtual File System initialized");
 }
 
 static int check_driver_validity(vfs_driver_t *driver) {
     // (Implementation unchanged)
     if (!driver) { VFS_ERROR("Attempted to register NULL driver"); return -FS_ERR_INVALID_PARAM; }
     if (!driver->fs_name) { VFS_ERROR("Driver has NULL fs_name"); return -FS_ERR_INVALID_PARAM; }
     if (!driver->mount) { VFS_ERROR("Driver '%s' has NULL mount function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     if (!driver->open) { VFS_ERROR("Driver '%s' has NULL open function", driver->fs_name); return -FS_ERR_INVALID_PARAM; }
     return FS_SUCCESS;
 }
 
 int vfs_register_driver(vfs_driver_t *driver) {
     // (Implementation unchanged)
     int check_result = check_driver_validity(driver);
     if (check_result != FS_SUCCESS) { return check_result; }
     vfs_driver_t *current = driver_list;
     while (current) {
         if (strcmp(current->fs_name, driver->fs_name) == 0) { VFS_ERROR("Driver '%s' already registered", driver->fs_name); return -FS_ERR_FILE_EXISTS; }
         current = current->next;
     }
     driver->next = driver_list;
     driver_list = driver;
     VFS_LOG("Registered driver: %s", driver->fs_name);
     return FS_SUCCESS;
 }
 
 int vfs_unregister_driver(vfs_driver_t *driver) {
     // (Implementation unchanged)
      if (!driver) { VFS_ERROR("Attempted to unregister NULL driver"); return -FS_ERR_INVALID_PARAM; }
      vfs_driver_t **prev = &driver_list;
      vfs_driver_t *curr = driver_list;
      while (curr) {
          if (curr == driver) {
              *prev = curr->next; // Remove from list
              VFS_LOG("Unregistered driver: %s", driver->fs_name);
              return FS_SUCCESS;
          }
          prev = &curr->next;
          curr = curr->next;
      }
      VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
      return -FS_ERR_NOT_FOUND;
 }
 
 vfs_driver_t *vfs_get_driver(const char *fs_name) {
     // (Implementation unchanged)
      if (!fs_name) { VFS_ERROR("NULL fs_name passed to vfs_get_driver"); return NULL; }
      vfs_driver_t *curr = driver_list;
      while (curr) {
          if (strcmp(curr->fs_name, fs_name) == 0) { return curr; }
          curr = curr->next;
      }
      VFS_ERROR("Driver '%s' not found", fs_name);
      return NULL;
 }
 
 void vfs_list_drivers(void) {
     // (Implementation unchanged)
     VFS_LOG("Registered filesystem drivers:");
     if (!driver_list) { VFS_LOG("  (none)"); return; }
     vfs_driver_t *curr = driver_list; int count = 0;
     while (curr) { VFS_LOG("  %d: %s", ++count, curr->fs_name); curr = curr->next; }
     VFS_LOG("Total drivers: %d", count);
 }
 
 /*---------------------------------------------------------------------------
  * Mount Table Helpers (Revised)
  *---------------------------------------------------------------------------*/
 
 /**
  * @brief Adds a new mount entry by calling the external mount_table_add.
  */
 static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) {
     if (!mp || !fs || !ctx || !drv) {
         VFS_ERROR("Invalid parameters to add_mount_entry");
         return -FS_ERR_INVALID_PARAM;
     }
 
     // Create a persistent copy of the mount point path string
     // IMPORTANT: mount_table_remove will need to free this later!
     size_t mp_len = strlen(mp);
     char *mp_copy = (char *)kmalloc(mp_len + 1);
     if (!mp_copy) {
         VFS_ERROR("Failed to allocate memory for mount point path copy");
         return -FS_ERR_OUT_OF_MEMORY;
     }
     strcpy(mp_copy, mp);
 
     // Allocate the mount_t struct expected by mount_table_add
     mount_t *mnt_to_add = (mount_t *)kmalloc(sizeof(mount_t));
     if (!mnt_to_add) {
         kfree(mp_copy); // Free the allocated copy
         VFS_ERROR("Failed to allocate memory for mount_t structure");
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     // Populate the mount_t structure
     mnt_to_add->mount_point = mp_copy; // Store the persistent copy
     mnt_to_add->fs_name = fs;         // Assumes fs (driver name) is persistent
     mnt_to_add->fs_context = ctx;
     // NOTE: mount_t from mount.h doesn't store the driver pointer or mount_point_len
     // We rely on looking up the driver via fs_name later if needed.
     mnt_to_add->next = NULL; // mount_table_add handles the 'next' pointer
 
     // Call the external function to add to the global list
     int result = mount_table_add(mnt_to_add);
     if (result != 0) {
         kfree(mp_copy); // Free the copy if add failed
         kfree(mnt_to_add);
         VFS_ERROR("mount_table_add failed with code %d", result);
     } else {
         VFS_LOG("Added mount (via mount_table_add): '%s' -> %s (context: 0x%p)", mp, fs, ctx);
     }
     return result;
 }
 
 
 /**
  * @brief Finds the most specific mount entry corresponding to a given path using the global list.
  * Implements the longest-prefix match algorithm by iterating the global list.
  * @param path The absolute path to resolve.
  * @return Pointer to the best matching mount_t, or NULL if no suitable mount point found.
  */
 static mount_t *find_best_mount_for_path(const char *path) {
     if (!path || path[0] != '/') {
         VFS_ERROR("Invalid or non-absolute path passed to find_best_mount_for_path: '%s'", path ? path : "NULL");
         return NULL;
     }
 
     mount_t *best_match = NULL;
     size_t best_len = 0;
 
     // Get the head of the global list (requires modification to mount_table.c or uses a trick)
     mount_t *curr = get_mount_list_head(); // Assumes this returns the head
 
     while (curr) {
         // Pre-calculate or get length if mount_t stores it
         size_t current_mount_point_len = strlen(curr->mount_point);
 
         // Check if path starts with the mount point
         if (strncmp(path, curr->mount_point, current_mount_point_len) == 0) {
             // Check for valid matches: exact, subdirectory, or root '/'
             bool is_exact_match = (path[current_mount_point_len] == '\0');
             bool is_subdir_match = (path[current_mount_point_len] == '/');
             bool is_root_mount = (current_mount_point_len == 1 && curr->mount_point[0] == '/');
 
             if (is_exact_match || is_subdir_match || is_root_mount) {
                 // This is a valid match. Is it the best (longest) one so far?
                 if (current_mount_point_len > best_len) {
                     best_match = curr;
                     best_len = current_mount_point_len;
                 }
                 // Optional refinement: if lengths are equal, prefer non-root mount? (Can be complex)
             }
         }
         curr = curr->next; // Move to the next node in the global list
     }
 
     if (best_match) {
         VFS_DEBUG_LOG("Path '%s' matched to mount point '%s'", path, best_match->mount_point);
     } else {
         VFS_ERROR("No mount point found for path '%s'", path);
     }
 
     return best_match;
 }
 
 
 /**
  * @brief Calculates the path relative to a mount point.
  */
 static const char *get_relative_path(const char *path, mount_t *mnt) { // Use mount_t
     if (!path || !mnt) {
         return NULL;
     }
 
     size_t mount_point_len = strlen(mnt->mount_point); // Calculate length
     const char *relative_path = path + mount_point_len;
 
     // Handle special cases
     if (*relative_path == '\0') { // Path is identical to mount point
         // Special case for root mount point "/"
         if (mount_point_len == 1 && mnt->mount_point[0] == '/') {
              return "/"; // Relative path is root itself
         } else {
              // For other mount points exact match -> relative path is "" or "/"?
              // Let's return "/" for consistency with drivers expecting a path.
              return "/";
         }
     } else if (mount_point_len == 1 && mnt->mount_point[0] == '/') { // Root mount
         // Relative path IS the original path if root is mounted
         return path;
     } else if (*relative_path == '/') { // Path is like /mnt/point/sub/dir
         // The relative path starts AFTER the mount point, including the leading '/'
         return relative_path;
     } else {
          // Should not happen if mount point matched correctly with trailing / or exact
          VFS_ERROR("Unexpected relative path calculation for '%s' on mount '%s'", path, mnt->mount_point);
          return NULL;
     }
 }
 
 /*---------------------------------------------------------------------------
  * Mount / Unmount Operations (Revised)
  *---------------------------------------------------------------------------*/
 int vfs_mount_root(const char *mp, const char *fs, const char *dev) {
     if (!mp || strcmp(mp, "/") != 0) { VFS_ERROR("Root mount point must be '/'"); return -FS_ERR_INVALID_PARAM; }
 
     // Check if root is already mounted using the global list finder
     if (mount_table_find("/") != NULL) {
         VFS_LOG("Root filesystem already mounted, ignoring duplicate mount request.");
         return FS_SUCCESS; // Already mounted is not an error here
     }
 
     vfs_driver_t *driver = vfs_get_driver(fs);
     if (!driver) { VFS_ERROR("Filesystem driver '%s' not found", fs); return -FS_ERR_NOT_FOUND; }
 
     VFS_LOG("Attempting to mount device '%s' as %s at root", dev, fs);
     void *fs_context = driver->mount(dev); // Call driver mount
     if (!fs_context) { VFS_ERROR("Driver '%s' failed to mount device '%s'", fs, dev); return -FS_ERR_MOUNT; }
 
     // Add the entry to the global list via the helper
     int result = add_mount_entry(mp, fs, fs_context, driver);
     if (result != FS_SUCCESS) {
         if (driver->unmount) { driver->unmount(fs_context); } // Cleanup on add failure
         return result;
     }
 
     VFS_LOG("Root filesystem mounted successfully: '%s' on device '%s'", fs, dev);
     return FS_SUCCESS;
 }
 
 int vfs_unmount_root(void) {
     // Find the root mount entry using the global list finder
     mount_t *root_mount = mount_table_find("/");
     if (!root_mount) {
         VFS_ERROR("Root filesystem not found or not mounted at '/'");
         return -FS_ERR_NOT_FOUND;
     }
 
     // Check if it's the only mount point (iterate global list)
     mount_t *curr = get_mount_list_head(); // Assumes this works
     int mount_count = 0;
     while (curr) { mount_count++; curr = curr->next; }
 
     if (mount_count > 1) {
         VFS_ERROR("Cannot unmount root while other filesystems are mounted (count: %d)", mount_count);
         return -FS_ERR_BUSY;
     }
 
     // Get the driver associated with this mount
     vfs_driver_t *driver = vfs_get_driver(root_mount->fs_name);
     int result = FS_SUCCESS;
     if (driver && driver->unmount) {
         result = driver->unmount(root_mount->fs_context);
         if (result != FS_SUCCESS) {
             VFS_ERROR("Driver '%s' failed to unmount root (code: %d)", root_mount->fs_name, result);
             // Continue with removal from list anyway
         }
     } else if (!driver) {
          VFS_ERROR("Driver '%s' not found during unmount!", root_mount->fs_name);
          result = -FS_ERR_NOT_FOUND; // Driver missing is an error
     }
 
     // Remove from global mount table using the external function
     // mount_table_remove should handle freeing the mount_t struct and the copied path string
     int remove_result = mount_table_remove("/");
     if (remove_result != 0) {
          VFS_ERROR("mount_table_remove failed for root '/' (code %d)", remove_result);
          // If driver unmount succeeded but remove failed, state is inconsistent
          if (result == FS_SUCCESS) result = remove_result; // Propagate remove error if unmount was okay
     } else {
         VFS_LOG("Root filesystem unmounted successfully");
     }
 
     return result;
 }
 
 
 // vfs_list_mounts: Use the external function directly
 void vfs_list_mounts(void) {
     mount_table_list();
 }
 
 int vfs_shutdown(void) {
     VFS_LOG("Shutting down VFS layer...");
     int result = vfs_unmount_root(); // Try to unmount root (which uses global list)
     driver_list = NULL; // Clear driver list
     if (result == FS_SUCCESS) { VFS_LOG("VFS shutdown complete"); }
     else { VFS_ERROR("VFS shutdown encountered errors (code: %d)", result); }
     return result;
 }
 
 /*---------------------------------------------------------------------------
  * File Operations (Revised)
  *---------------------------------------------------------------------------*/
 file_t *vfs_open(const char *path, int flags) {
     if (!path) { VFS_ERROR("NULL path provided"); return NULL; }
     VFS_DEBUG_LOG("Opening path '%s' with flags 0x%x", path, flags);
 
     // Find the best mount point using the global list helper
     mount_t *mnt = find_best_mount_for_path(path);
     if (!mnt) { return NULL; } // Error already printed by helper
 
     // Get the driver for this mount point
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) { VFS_ERROR("Driver '%s' not found for mount point '%s'", mnt->fs_name, mnt->mount_point); return NULL; }
 
     // Get the path relative to the mount point
     const char *relative_path = get_relative_path(path, mnt);
     if (!relative_path) { VFS_ERROR("Failed to calculate relative path for '%s'", path); return NULL; }
 
     VFS_DEBUG_LOG("Using mount '%s', driver '%s', relative path '%s'",
                   mnt->mount_point, driver->fs_name, relative_path);
 
     // Validate and call driver's open function
     if (!driver->open) { VFS_ERROR("Driver '%s' has no open function", driver->fs_name); return NULL; }
     vnode_t *node = driver->open(mnt->fs_context, relative_path, flags);
     if (!node) { VFS_ERROR("Driver '%s' failed to open path '%s'", driver->fs_name, relative_path); return NULL; }
 
     // Allocate and initialize file handle (vnode now links back to driver)
     file_t *file = (file_t *)kmalloc(sizeof(file_t));
     if (!file) {
         VFS_ERROR("Failed kmalloc for file handle");
         if (driver->close) { // Attempt cleanup
             file_t temp_file = { .vnode = node, .flags = flags, .offset = 0 };
             driver->close(&temp_file);
         }
         return NULL;
     }
     file->vnode = node;
     file->flags = flags;
     file->offset = 0;
 
     VFS_DEBUG_LOG("Successfully opened '%s' (file: 0x%p, vnode: 0x%p)", path, file, node);
     return file;
 }
 
 // vfs_close, vfs_read, vfs_write, vfs_lseek remain largely unchanged
 // as they operate on the file_t handle which contains the vnode,
 // and the vnode contains the fs_driver pointer.
 
 int vfs_close(file_t *file) {
      if (!file) { VFS_ERROR("NULL file handle passed to vfs_close"); return -FS_ERR_INVALID_PARAM; }
      if (!file->vnode) { VFS_ERROR("File handle has NULL vnode"); kfree(file); return -FS_ERR_INVALID_PARAM; }
      if (!file->vnode->fs_driver) { VFS_ERROR("File vnode has NULL fs_driver"); kfree(file); return -FS_ERR_INVALID_PARAM; }
      if (!file->vnode->fs_driver->close) { VFS_ERROR("Driver has no close function"); kfree(file); return -FS_ERR_NOT_SUPPORTED; }
      VFS_DEBUG_LOG("Closing file handle 0x%p (vnode: 0x%p)", file, file->vnode);
      int result = file->vnode->fs_driver->close(file); // Driver is responsible for freeing vnode->data if needed
      kfree(file->vnode); // Free the vnode itself
      kfree(file);       // Free the file handle
      if (result != FS_SUCCESS) { VFS_ERROR("Driver returned error %d from close", result); }
      return result;
 }
 
 int vfs_read(file_t *file, void *buf, size_t len) {
      if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->read) { return -FS_ERR_INVALID_PARAM; }
      if (!buf && len > 0) { return -FS_ERR_INVALID_PARAM; }
      if (len == 0) return 0;
      VFS_DEBUG_LOG("Reading from file 0x%p, offset=%lld, len=%u", file, file->offset, len);
      int bytes_read = file->vnode->fs_driver->read(file, buf, len);
      if (bytes_read > 0) { file->offset += bytes_read; VFS_DEBUG_LOG("Read %d bytes, new offset=%lld", bytes_read, file->offset); }
      else if (bytes_read < 0) { VFS_ERROR("Driver read error %d", bytes_read); }
      else { VFS_DEBUG_LOG("Read 0 bytes (EOF)"); }
      return bytes_read;
 }
 
 int vfs_write(file_t *file, const void *buf, size_t len) {
      if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->write) { return -FS_ERR_INVALID_PARAM; }
      if (!buf && len > 0) { return -FS_ERR_INVALID_PARAM; }
      if (len == 0) return 0;
      if (!(file->flags & (O_WRONLY | O_RDWR))) { return -FS_ERR_PERMISSION_DENIED; }
      VFS_DEBUG_LOG("Writing to file 0x%p, offset=%lld, len=%u", file, file->offset, len);
      int bytes_written = file->vnode->fs_driver->write(file, buf, len);
      if (bytes_written > 0) { file->offset += bytes_written; VFS_DEBUG_LOG("Wrote %d bytes, new offset=%lld", bytes_written, file->offset); }
      else if (bytes_written < 0) { VFS_ERROR("Driver write error %d", bytes_written); }
      else { VFS_DEBUG_LOG("Wrote 0 bytes"); }
      return bytes_written;
 }
 
 off_t vfs_lseek(file_t *file, off_t offset, int whence) {
      if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->lseek) { return (off_t)-FS_ERR_INVALID_PARAM; }
      if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { return (off_t)-FS_ERR_INVALID_PARAM; }
      VFS_DEBUG_LOG("Seeking in file 0x%p, offset=%lld, whence=%d", file, offset, whence);
      off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);
      if (new_offset >= 0) { file->offset = new_offset; VFS_DEBUG_LOG("Seek successful, new offset=%lld", new_offset); }
      else { VFS_ERROR("Driver lseek error %lld", new_offset); }
      return new_offset;
 }
 
 // vfs_is_ready, vfs_self_test, vfs_path_exists, vfs_debug_dump need updates
 // to use the global mount list functions as well.
 
 bool vfs_is_ready(void) {
     if (!driver_list) { return false; }
     // Check global mount list via external function/head pointer
     mount_t *head = get_mount_list_head(); // Assuming this works
     if (!head) { return false; }
     // Check specifically for root mount in global list
     mount_t *curr = head;
     while (curr) {
         if (strcmp(curr->mount_point, "/") == 0) { return true; }
         curr = curr->next;
     }
     return false; // Root not found in global list
  }
 
  int vfs_self_test(void) {
      VFS_LOG("Running VFS self-test...");
      if (!vfs_is_ready()) { VFS_ERROR("VFS not ready for testing"); return -FS_ERR_NOT_INIT; }
      file_t *root_dir = vfs_open("/", O_RDONLY); // Uses updated open logic
      if (!root_dir) { VFS_ERROR("Failed to open root directory"); return -FS_ERR_IO; }
      int close_result = vfs_close(root_dir); // Uses updated close logic
      if (close_result != FS_SUCCESS) { VFS_ERROR("Failed to close root directory (code: %d)", close_result); return close_result; }
      VFS_LOG("VFS self-test passed");
      return FS_SUCCESS;
  }
 
  bool vfs_path_exists(const char *path) {
      if (!path) return false;
      file_t *file = vfs_open(path, O_RDONLY); // Uses updated open logic
      if (!file) return false;
      vfs_close(file); // Uses updated close logic
      return true;
  }
 
  void vfs_debug_dump(void) {
     VFS_LOG("========== VFS DEBUG INFORMATION ==========");
     vfs_list_drivers(); // List drivers (local list)
     mount_table_list(); // List mounts (global list via external func)
 
     // Optionally: Re-implement tree display by iterating the global list
     VFS_LOG("Mount tree (from global list):");
     mount_t *head = get_mount_list_head(); // Get global head
     if (!head) {
         VFS_LOG("  (empty)");
     } else {
         mount_t *root = NULL;
         mount_t *curr = head;
         while(curr) { if (strcmp(curr->mount_point, "/") == 0) { root = curr; break; } curr = curr->next; }
 
         if (!root) { VFS_LOG("  (no root mount)"); }
         else {
              VFS_LOG("  / [%s]", root->fs_name);
              curr = head;
              while (curr) {
                  if (curr != root) {
                       // (Indentation logic as before, using curr->mount_point)
                       size_t depth = 0; size_t mp_len = strlen(curr->mount_point);
                       for (size_t i = 0; i < mp_len; i++) { if (curr->mount_point[i] == '/') depth++; }
                       char indent[64] = "  ";
                       for (size_t i = 0; i < depth; i++) { strcat(indent, "  "); } // Basic indentation
                       VFS_LOG("%s%s [%s]", indent, curr->mount_point, curr->fs_name);
                  }
                  curr = curr->next;
              }
         }
     }
     VFS_LOG("==========================================");
  }
  
  int readdir(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index) {
    // ... your code (if any) ...
    return -FS_ERR_NOT_SUPPORTED; // Add this line
}

int unlink(void *fs_context, const char *path) {
    // ... your code (if any) ...
    return -FS_ERR_NOT_SUPPORTED; // Add this line
}