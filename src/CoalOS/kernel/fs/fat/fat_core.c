/**
 * @file fat_core.c
 * @brief Core FAT filesystem driver registration and basic helpers.
 *
 * Implements the VFS registration/unregistration functions for the FAT driver
 * and provides minimal shared helper functions.
 */

 #include <kernel/fs/fat/fat_core.h>   // Core definitions
 #include <kernel/fs/fat/fat_fs.h>     // For fat_mount_internal, fat_unmount_internal (implementations TBD)
 #include <kernel/fs/fat/fat_dir.h>    // For fat_open_internal, fat_readdir_internal, fat_unlink_internal (impl. TBD)
 #include <kernel/fs/fat/fat_io.h>     // For fat_read_internal, fat_write_internal, fat_lseek_internal, fat_close_internal (impl. exists)
 #include <kernel/fs/vfs/vfs.h>        // For vfs_register_driver, vfs_unregister_driver
 #include <kernel/drivers/display/terminal.h>   // For logging
 #include <kernel/lib/string.h>     // For memset (if needed, though struct init covers it)
 #include <kernel/lib/assert.h>     // For KERNEL_ASSERT
 
 /* --- VFS Driver Function Declarations --- */
 // These functions are implemented in other fat_*.c files but need to be
 // referenced here to populate the vfs_driver_t structure.
 
 // Implemented in fat_fs.c
 extern void *fat_mount_internal(const char *device);
 extern int   fat_unmount_internal(void *fs_context);
 
 // Implemented in fat_dir.c
 extern vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
 extern int      fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index);
 extern int      fat_unlink_internal(void *fs_context, const char *path);
 
 // Implemented in fat_dir_ops.c
 extern int      fat_mkdir_internal(void *fs_context, const char *path, mode_t mode);
 extern int      fat_rmdir_internal(void *fs_context, const char *path);
 
 // Implemented in fat_io.c
 extern int   fat_read_internal(file_t *file, void *buf, size_t len);
 extern int   fat_write_internal(file_t *file, const void *buf, size_t len);
 extern int   fat_close_internal(file_t *file);
 extern off_t fat_lseek_internal(file_t *file, off_t offset, int whence);
 
 /* --- Static VFS Driver Structure --- */
 // Defines the FAT filesystem driver interface for the VFS.
 vfs_driver_t fat_vfs_driver = {
     .fs_name = "FAT",               // Filesystem name
     .mount   = fat_mount_internal,    // Mount function pointer
     .unmount = fat_unmount_internal,  // Unmount function pointer
     .open    = fat_open_internal,     // Open function pointer
     .read    = fat_read_internal,     // Read function pointer
     .write   = fat_write_internal,    // Write function pointer
     .close   = fat_close_internal,    // Close function pointer
     .lseek   = fat_lseek_internal,    // Lseek function pointer
     .readdir = fat_readdir_internal,  // Readdir function pointer
     .unlink  = fat_unlink_internal,   // Unlink function pointer
    .mkdir   = fat_mkdir_internal,    // Mkdir function pointer
    .rmdir   = fat_rmdir_internal,    // Rmdir function pointer
     // Add .stat, etc. here if/when implemented
     .next    = NULL                 // Linked list pointer for VFS internal use
 };
 
 /* --- Public Function Implementations --- */
 
 /**
  * @brief Registers the FAT filesystem driver with the VFS.
  */
 int fat_register_driver(void)
 {
     terminal_write("[FAT Core] Registering FAT filesystem driver with VFS...\n");
     // The structure is statically initialized, just need to register it.
     int result = vfs_register_driver(&fat_vfs_driver);
     if (result == 0) {
         terminal_write("[FAT Core] FAT driver registered successfully.\n");
     } else {
         terminal_printf("[FAT Core] Error: Failed to register FAT driver (VFS error code: %d)\n", result);
     }
     return result;
 }
 
 /**
  * @brief Unregisters the FAT filesystem driver from the VFS.
  */
 void fat_unregister_driver(void)
 {
     terminal_write("[FAT Core] Unregistering FAT filesystem driver from VFS...\n");
     // Pass the same driver structure instance used for registration.
     vfs_unregister_driver(&fat_vfs_driver);
     terminal_write("[FAT Core] FAT driver unregistered.\n");
 }
 
 /**
  * @brief Helper function to extract the full cluster number from a directory entry.
  */
 uint32_t fat_get_entry_cluster(const fat_dir_entry_t *e)
 {
     // Basic sanity check
     if (!e) {
         return 0; // Invalid entry pointer
     }
     // Combine high and low words to form the 32-bit cluster number.
     // The high word is only relevant for FAT32, but this works for all types
     // as it will be 0 for FAT12/16 entries.
     return (((uint32_t)e->first_cluster_high) << 16) | e->first_cluster_low;
 }