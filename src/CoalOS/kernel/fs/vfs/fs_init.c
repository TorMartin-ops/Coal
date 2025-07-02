/**
 * @file fs_init.c
 * @brief Filesystem Layer Initialization and Management
 *
 * Orchestrates the setup and teardown of the Virtual File System (VFS),
 * including driver registration, device initialization, root filesystem mounting,
 * and shutdown procedures.
 */

 #include <kernel/fs/vfs/fs_init.h>
 #include <kernel/fs/vfs/vfs.h>            // VFS core API
 #include <kernel/fs/fat/fat_core.h>           // FAT filesystem driver (needs prototypes for register/unregister)
 #include <kernel/drivers/storage/disk.h>           // Disk device abstraction
 #include <kernel/drivers/storage/buffer_cache.h>   // Buffer cache registration/API
 #include <kernel/drivers/display/terminal.h>       // Kernel logging/debugging
 #include <kernel/fs/vfs/fs_errno.h>       // Filesystem error codes
 #include <kernel/fs/vfs/fs_config.h>   // Not including as ROOT_* defines are missing
 #include <kernel/core/types.h>          // Standard types (bool, etc.)
 #include <kernel/fs/vfs/sys_file.h>       // For O_RDONLY used in test function
 #include <kernel/memory/kmalloc.h>        // For memory allocation (used in test function)
 #include <kernel/fs/vfs/mount_table.h>    // For list_mounts()
 #include <kernel/drivers/input/keyboard_hw.h> 
 #include <kernel/drivers/display/serial.h>  
 #include <kernel/lib/port_io.h>       // For inb() used in debugging
 
 #include <kernel/lib/string.h>         // For strcmp, etc.
 
 
 /* Global flag to prevent double initialization/shutdown */
 static bool s_fs_initialized = false;
 
 /* Statically allocated structure for the root disk device.
  * In a more dynamic system, this might be allocated or part of a list. */
 static disk_t s_root_disk;
 
 /**
  * @brief Initializes the core filesystem components.
  *
  * Sets up the VFS, registers filesystem drivers (FAT), initializes the
  * root block device, registers it with the buffer cache, and mounts
  * the root filesystem.
  *
  * @return FS_SUCCESS (0) on success, or a negative FS_ERR_* code on failure.
  */
  int fs_init(void)
  {
      if (s_fs_initialized) {
          terminal_write("[FS_INIT] Warning: File system already initialized.\n");
          return FS_SUCCESS;
      }
  
      terminal_write("[FS_INIT] Starting file system initialization...\n");
      int ret = FS_SUCCESS;
  
      // 1. Initialize Buffer Cache (Should happen before disk registration)
      //    Assuming buffer_cache_init() is called elsewhere during kernel boot.
  
      // 2. Initialize VFS Layer
      terminal_write("[FS_INIT] Initializing VFS layer...\n");
      vfs_init(); // Initialize mount table, file descriptor table etc.
  
      // 3. Register Filesystem Drivers
      terminal_write("[FS_INIT] Registering FAT filesystem driver...\n");
      ret = fat_register_driver(); // Calls function declared in fat.h, implemented in fat_core.c
      if (ret != FS_SUCCESS) {
          terminal_printf("[FS_INIT] Error: FAT driver registration failed (code %d).\n", ret);
          vfs_shutdown(); // Attempt cleanup
          return ret;
      }
      terminal_write("[FS_INIT] FAT driver registered successfully.\n");
  
      // Add registration for other potential FS drivers here...
  
      // 4. Initialize and Register the Root Disk Device
      const char *root_device_name = ROOT_DEVICE_NAME; // Using local define
      const char *root_fs_type = ROOT_FS_TYPE;         // Using local define
  
      if (!root_device_name || !root_fs_type) {
           terminal_write("[FS_INIT] Error: Root device name or FS type configuration is invalid (NULL).\n");
           fat_unregister_driver();
           vfs_shutdown();
           return FS_ERR_INVALID_PARAM;
      }
  
      terminal_printf("[FS_INIT Debug] KBC Status before disk_init: 0x%x\n", inb(KBC_STATUS_PORT));
      // *** FIRST (and only) disk_init call for the root disk ***
      ret = disk_init(&s_root_disk, root_device_name);
      terminal_printf("[FS_INIT Debug] KBC Status after disk_init: 0x%x\n", inb(KBC_STATUS_PORT));
  
      if (ret != FS_SUCCESS) {
          terminal_printf("[FS_INIT] Error: Failed to initialize root disk device '%s' (code %d).\n",
                          root_device_name, ret);
          fat_unregister_driver();
          vfs_shutdown();
          return ret; // Propagate disk error
      }
      // *** The redundant second disk_init call has been removed from here ***
  
      terminal_printf("[FS_INIT] Registering root disk '%s' with buffer cache...\n", root_device_name);
      ret = buffer_register_disk(&s_root_disk);
      if (ret != FS_SUCCESS) {
           terminal_printf("[FS_INIT] Error: Failed to register root disk '%s' with buffer cache (code %d).\n",
                           root_device_name, ret);
           fat_unregister_driver();
           vfs_shutdown();
           return ret; // Propagate buffer cache registration error
      }
      terminal_printf("[FS_INIT] Root disk '%s' registered successfully.\n", root_device_name);
  
  
      // 5. Mount the Root Filesystem via VFS
      terminal_printf("[FS_INIT] Attempting to mount root FS (%s) on device '%s' at '/'\n",
                      root_fs_type,
                      root_device_name);
  
      ret = vfs_mount_root("/", root_fs_type, root_device_name);
      if (ret != FS_SUCCESS) {
          terminal_printf("[FS_INIT] Error: Root filesystem mount failed for device '%s' (code %d).\n",
                          root_device_name, ret);
          terminal_write("[FS_INIT] Attempting cleanup after mount failure...\n");
          // Consider unregistering disk from buffer cache if an unregister function exists
          // buffer_unregister_disk(&s_root_disk); // If available
          fat_unregister_driver();
          vfs_shutdown();
          return ret; // Propagate mount error
      }
  
      s_fs_initialized = true;
      terminal_write("[FS_INIT] File system initialization complete.\n");
      terminal_write("[FS_INIT] Current mount points:\n");
      list_mounts(); // Display the active mounts
  
      return FS_SUCCESS;
  }
 
 /**
  * @brief Checks if the filesystem layer is currently initialized.
  * @return True if initialized, false otherwise.
  */
 bool fs_is_initialized(void)
 {
     return s_fs_initialized;
 }
 
 /**
  * @brief Shuts down the filesystem layer gracefully.
  *
  * Unmounts filesystems, unregisters drivers, and shuts down the VFS.
  *
  * @return FS_SUCCESS (0) on success, or a negative FS_ERR_* code on failure
  * (currently always returns FS_SUCCESS, but logs warnings).
  */
 int fs_shutdown(void)
 {
     if (!s_fs_initialized) {
         terminal_write("[FS_SHUTDOWN] Warning: File system not initialized, nothing to shut down.\n");
         return FS_SUCCESS;
     }
 
     terminal_write("[FS_SHUTDOWN] Shutting down file system...\n");
     int final_ret = FS_SUCCESS; // Track if any step fails
 
     // 1. Unmount Root Filesystem (and implicitly any others via VFS shutdown)
     terminal_write("[FS_SHUTDOWN] Unmounting root filesystem...\n");
     int unmount_result = vfs_unmount_root(); // Should handle unmounting "/"
     if (unmount_result != FS_SUCCESS) {
         terminal_printf("[FS_SHUTDOWN] Warning: Root file system unmount failed (code %d). Force continuing shutdown.\n",
                         unmount_result);
         final_ret = unmount_result; // Report the error but continue
     }
 
     // 2. Unregister Filesystem Drivers
     terminal_write("[FS_SHUTDOWN] Unregistering FAT driver...\n");
     fat_unregister_driver(); // Calls function declared in fat.h, implemented in fat_core.c
     // Unregister other drivers here...
 
     // 3. Unregister Disks from Buffer Cache (Optional but good practice)
     // if (buffer_unregister_disk) { // Check if function exists
     //     terminal_printf("[FS_SHUTDOWN] Unregistering disk '%s' from buffer cache...\n", s_root_disk.blk_dev.device_name);
     //     buffer_unregister_disk(&s_root_disk);
     // }
 
     // 4. Shutdown VFS Layer (cleans up mount points, file descriptors)
     terminal_write("[FS_SHUTDOWN] Shutting down VFS layer...\n");
     int vfs_result = vfs_shutdown();
     if (vfs_result != FS_SUCCESS) {
         terminal_printf("[FS_SHUTDOWN] Warning: VFS shutdown failed (code %d).\n", vfs_result);
         if (final_ret == FS_SUCCESS) final_ret = vfs_result; // Report first error encountered
     }
 
     // 5. Sync Buffer Cache (Flush remaining dirty buffers)
     terminal_write("[FS_SHUTDOWN] Syncing buffer cache...\n");
     buffer_cache_sync(); // Ensure all data is written
 
     // Consider low-level disk cleanup if necessary (e.g., power down commands)
     // disk_shutdown(&s_root_disk); // If available
 
 
     s_fs_initialized = false;
     terminal_write("[FS_SHUTDOWN] File system shutdown complete.\n");
     return final_ret; // Return success or the first error encountered
 }
 
 
 /**
  * @brief Simple test function for verifying basic file read access.
  *
  * Attempts to open, read a small chunk, and close a file using VFS calls.
  * Logs results to the terminal.
  *
  * @param path The absolute path to the file to test.
  * @return FS_SUCCESS (0) on success, negative error code on failure.
  */
 int fs_test_file_access(const char *path)
 {
     if (!s_fs_initialized) {
         terminal_write("[FS_TEST] Error: File system not initialized.\n");
         return FS_ERR_NOT_INIT;
     }
     if (!path) {
         terminal_write("[FS_TEST] Error: NULL path provided.\n");
         return FS_ERR_INVALID_PARAM;
     }
 
     terminal_printf("[FS_TEST] Testing file access: '%s'\n", path);
 
     file_t *file = NULL;
     char *buffer = NULL;
     int ret = FS_SUCCESS;
 
     // Try to open the file for reading
     file = vfs_open(path, O_RDONLY);
     if (!file) {
         terminal_printf("[FS_TEST] Error: vfs_open failed for file '%s'.\n", path);
         return FS_ERR_UNKNOWN; // Assuming failure means not found or other error
     }
 
     // Allocate a buffer for reading
     buffer = kmalloc(128); // Allocate a reasonable size buffer
     if (!buffer) {
          terminal_printf("[FS_TEST] Error: Failed to allocate read buffer for '%s'.\n", path); // Corrected typo
          ret = FS_ERR_OUT_OF_MEMORY;
          goto cleanup;
     }
 
     // Try to read some data
     ssize_t bytes_read = vfs_read(file, buffer, 128 - 1); // Read up to buffer size - 1
     if (bytes_read < 0) {
         terminal_printf("[FS_TEST] Error: Failed to read from file '%s' (code %d)\n",
                         path, (int)bytes_read);
         ret = (int)bytes_read; // vfs_read returns negative FS_ERR_* code
         goto cleanup;
     }
 
     // Null-terminate and display the data
     buffer[bytes_read] = '\0';
     terminal_printf("[FS_TEST] Successfully read %d bytes from '%s':\n", (int)bytes_read, path);
     terminal_write(">>>\n");
     terminal_write(buffer); // Assumes buffer contains printable text
     terminal_write("\n<<<\n");
 
     ret = FS_SUCCESS; // Mark as success before closing
 
 cleanup:
     // Close the file (must happen even on read error)
     if (file) {
         int close_result = vfs_close(file);
         if (close_result != FS_SUCCESS) {
             terminal_printf("[FS_TEST] Warning: Failed to close file '%s' (code %d)\n",
                             path, close_result);
             // Report close error only if read succeeded
             if (ret == FS_SUCCESS) ret = close_result;
         }
     }
 
     // Free the buffer
     if (buffer) {
         kfree(buffer);
     }
 
     if (ret == FS_SUCCESS) {
         terminal_printf("[FS_TEST] File access test successful for '%s'\n", path);
     } else {
         terminal_printf("[FS_TEST] File access test failed for '%s' (Final code: %d)\n", path, ret);
     }
 
     return ret;
 }