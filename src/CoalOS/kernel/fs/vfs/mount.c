/**
 * mount.c - Implementation of the high-level filesystem mounting API.
 */

 #include <kernel/fs/vfs/mount.h>
 #include <kernel/fs/vfs/vfs.h>            // For vfs_get_driver
 #include <kernel/fs/vfs/mount_table.h>    // For mount_table_add, find, remove, list
 #include <kernel/memory/kmalloc.h>        // For allocating mount_t and mount_point string
 #include <kernel/drivers/display/terminal.h>       // For logging
 #include <kernel/lib/string.h>         // For strcmp, strlen, strcpy
 #include <kernel/core/types.h>
 #include <kernel/fs/vfs/fs_errno.h>
 
 /**
  * @brief Mounts a filesystem onto a specified mount point.
  */
 fs_error_t mount_filesystem(const char *mount_point, const char *device, const char *fs_name, uint32_t flags) {
     (void)flags; // Mark flags as unused for now
 
     // 1. Validate Input Parameters
     if (!mount_point || !device || !fs_name) {
         terminal_write("[Mount API] Error: Invalid NULL parameter provided.\n");
         return -FS_ERR_INVALID_PARAM;
     }
     if (mount_point[0] != '/') {
          terminal_printf("[Mount API] Error: Mount point '%s' must be an absolute path.\n", mount_point);
          return -FS_ERR_INVALID_PARAM;
     }
     // Basic length checks
     if (strlen(mount_point) == 0 || strlen(device) == 0 || strlen(fs_name) == 0) {
         terminal_write("[Mount API] Error: Empty string parameter provided.\n");
         return -FS_ERR_INVALID_PARAM;
     }
     // TODO: Add more robust path validation if needed (e.g., allowed characters)
 
 
     // 2. Find Filesystem Driver via VFS
     vfs_driver_t *driver = vfs_get_driver(fs_name);
     if (!driver) {
         terminal_printf("[Mount API] Error: Filesystem driver '%s' not registered.\n", fs_name);
         return -FS_ERR_NOT_FOUND; // Or a more specific "FS not supported" error?
     }
     if (!driver->mount) {
          terminal_printf("[Mount API] Error: Driver '%s' does not support mounting.\n", fs_name);
          return -FS_ERR_NOT_SUPPORTED;
     }
 
 
     // 3. Call the Driver's Mount Implementation
     terminal_printf("[Mount API] Calling driver '%s' to mount device '%s'...\n", fs_name, device);
     void *fs_context = driver->mount(device); // Driver handles device interaction
     if (!fs_context) {
         // Driver should have printed a more specific error
         terminal_printf("[Mount API] Driver '%s' failed to mount device '%s'.\n", fs_name, device);
         return -FS_ERR_MOUNT; // Generic mount error
     }
     terminal_printf("[Mount API] Driver mount successful, context=0x%p.\n", fs_context);
 
 
     // 4. Prepare and Add Entry to the Global Mount Table
     //    - Allocate mount_t structure
     //    - Allocate and COPY the mount_point string
     //    - Call mount_table_add
 
     mount_t *new_mount_entry = (mount_t *)kmalloc(sizeof(mount_t));
     if (!new_mount_entry) {
         terminal_write("[Mount API] Error: Failed to allocate memory for mount_t.\n");
         // Attempt to unmount what the driver just mounted
         if (driver->unmount) { driver->unmount(fs_context); }
         return -FS_ERR_OUT_OF_MEMORY;
     }
     memset(new_mount_entry, 0, sizeof(mount_t)); // Clear structure
 
     size_t mp_len = strlen(mount_point);
     char *mp_copy = (char *)kmalloc(mp_len + 1);
     if (!mp_copy) {
         terminal_write("[Mount API] Error: Failed to allocate memory for mount point string copy.\n");
         kfree(new_mount_entry);
         if (driver->unmount) { driver->unmount(fs_context); }
         return -FS_ERR_OUT_OF_MEMORY;
     }
     strcpy(mp_copy, mount_point);
 
     // Populate the new entry
     new_mount_entry->mount_point = mp_copy; // Store the allocated copy
     new_mount_entry->fs_name = driver->fs_name; // Use name from driver struct (should be persistent)
     new_mount_entry->fs_context = fs_context;
     new_mount_entry->next = NULL;
 
     // Add to the global table
     int add_result = mount_table_add(new_mount_entry);
     if (add_result != FS_SUCCESS) {
         terminal_printf("[Mount API] Error: Failed to add mount entry to table (code %d).\n", add_result);
         kfree(mp_copy); // Free the allocated string copy
         kfree(new_mount_entry); // Free the mount_t struct
         if (driver->unmount) { driver->unmount(fs_context); } // Unmount driver context
         return add_result; // Propagate error from mount_table_add
     }
 
     terminal_printf("[Mount API] Successfully mounted '%s' on '%s' type '%s'.\n",
                    device, mount_point, fs_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Unmounts a filesystem previously mounted at the specified mount point.
  */
 fs_error_t unmount_filesystem(const char *mount_point) {
     // 1. Validate Input
     if (!mount_point) {
         terminal_write("[Mount API] Error: NULL mount point for unmount.\n");
         return -FS_ERR_INVALID_PARAM;
     }
     if (mount_point[0] != '/') {
          terminal_printf("[Mount API] Error: Unmount path '%s' must be absolute.\n", mount_point);
          return -FS_ERR_INVALID_PARAM;
     }
 
 
     // 2. Find the Mount Entry in the Global Table
     // Note: mount_table_find locks internally
     mount_t *mnt = mount_table_find(mount_point);
     if (!mnt) {
         terminal_printf("[Mount API] Error: Mount point '%s' not found.\n", mount_point);
         return -FS_ERR_NOT_FOUND;
     }
 
     // 3. Find the Corresponding Driver
     vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
     if (!driver) {
         // This indicates an inconsistent state - mount exists but driver is gone?
         terminal_printf("[Mount API] Error: Driver '%s' for mount point '%s' not found! Cannot unmount cleanly.\n",
                        mnt->fs_name, mount_point);
         // Should we attempt to remove from mount table anyway? Risky.
         return -FS_ERR_INTERNAL; // Internal inconsistency
     }
     if (!driver->unmount) {
         terminal_printf("[Mount API] Error: Driver '%s' does not support unmounting.\n", mnt->fs_name);
         return -FS_ERR_NOT_SUPPORTED;
     }
 
 
     // 4. Call the Driver's Unmount Implementation
     terminal_printf("[Mount API] Calling driver '%s' to unmount context 0x%p for '%s'...\n",
                    mnt->fs_name, mnt->fs_context, mount_point);
     int driver_unmount_result = driver->unmount(mnt->fs_context);
     if (driver_unmount_result != FS_SUCCESS) {
         terminal_printf("[Mount API] Error: Driver unmount failed for '%s' (code %d). Filesystem may still be busy or in error state.\n",
                        mount_point, driver_unmount_result);
         // Even if driver fails, attempt to remove from mount table? Or block?
         // Let's block for now, as state might be inconsistent.
         return driver_unmount_result; // Propagate driver error
     }
     terminal_printf("[Mount API] Driver unmount successful for '%s'.\n", mount_point);
 
 
     // 5. Remove Entry from the Global Mount Table
     // mount_table_remove handles freeing the mount_t struct and mount_point string.
     int remove_result = mount_table_remove(mount_point);
     if (remove_result != FS_SUCCESS) {
         // This is serious if the driver unmounted but we can't remove the table entry.
         terminal_printf("[Mount API] CRITICAL Error: Failed to remove mount table entry for '%s' after successful driver unmount (code %d)!\n",
                        mount_point, remove_result);
         // System might be in an inconsistent state.
         return -FS_ERR_INTERNAL;
     }
 
     terminal_printf("[Mount API] Unmounted '%s' successfully.\n", mount_point);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Lists all currently mounted filesystems to the kernel console.
  */
 void list_mounts(void) {
     // Delegate directly to the mount table implementation
     mount_table_list();
 }