/**
 * @file fat_fs.h
 * @brief Filesystem-level operations for FAT driver (mount, unmount).
 *
 * Declares the functions responsible for initializing (mounting) a FAT filesystem
 * instance on a block device and tearing it down (unmounting).
 */

 #ifndef FAT_FS_H
 #define FAT_FS_H
 
 #include <kernel/fs/fat/fat_core.h> // Core FAT structures (fat_fs_t, etc.)
 #include <kernel/fs/vfs/fs_errno.h> // Filesystem error codes
 #include <kernel/fs/vfs/vfs.h>      // VFS definitions (though not directly used here)
 
 /* --- VFS Operations Implemented in fat_fs.c --- */
 
 /**
  * @brief Mounts a FAT filesystem on a specified block device.
  *
  * Reads the boot sector, validates it, determines FAT type and geometry,
  * allocates the fat_fs_t context, loads the FAT table into memory,
  * and prepares the filesystem instance for use by the VFS.
  *
  * @param device_name The name of the registered block device (e.g., "hda").
  * @return A pointer to the allocated and initialized fat_fs_t structure
  * on success (castable to void* for VFS). Returns NULL on failure.
  */
 void *fat_mount_internal(const char *device_name);
 
 /**
  * @brief Unmounts a FAT filesystem instance.
  *
  * Flushes any dirty FAT table sectors back to disk via the buffer cache,
  * frees the in-memory FAT table, frees the fat_fs_t context structure,
  * and performs any other necessary cleanup.
  *
  * @param fs_context A pointer to the fat_fs_t structure (cast from void*)
  * previously returned by fat_mount_internal.
  * @return FS_SUCCESS (0) on successful unmount.
  * @return Negative FS_ERR_* code on failure (e.g., invalid context, flush error).
  */
 int fat_unmount_internal(void *fs_context);
 
 #endif /* FAT_FS_H */