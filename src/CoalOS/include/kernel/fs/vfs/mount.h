/**
 * mount.h - Filesystem Mounting API
 *
 * Defines the structure for representing mount points and the high-level
 * kernel API functions for mounting and unmounting filesystems.
 */

 #pragma once

 #ifndef MOUNT_H
 #define MOUNT_H
 
 #include <kernel/core/types.h>    // Basic types (size_t, bool, etc.)
 #include <kernel/fs/vfs/fs_errno.h> // Filesystem error codes (fs_error_t)
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 // --- Structures ---
 
 /**
  * @brief Represents an active mount point in the system.
  * Stored in the global mount table (managed by mount_table.c).
  */
 typedef struct mount {
     /** Dynamically allocated string holding the absolute mount point path (e.g., "/", "/mnt/data"). */
     const char *mount_point;
     /** Name of the filesystem driver used (e.g., "FAT32", "ext2"). Assumed persistent. */
     const char *fs_name;
     /** Opaque pointer to filesystem-specific data returned by the driver's mount() function. */
     void *fs_context;
     /** Pointer to the next mount_t entry in the global linked list. */
     struct mount *next;
     /** Device name string (e.g., "hda", "hdb") */
     const char *device;
     /** Unique device ID for this mount */
     uint32_t device_id;
     /** Filesystem type (e.g., "FAT16", "FAT32") - for quick reference */
     const char *fs_type;
 } mount_t;
 
 
 // --- Public API Functions ---
 
 /**
  * @brief Mounts a filesystem onto a specified mount point.
  * This function interacts with the VFS to find the driver, call its mount
  * implementation, and register the mount point in the global table.
  *
  * @param mount_point The absolute path where the filesystem should be mounted.
  * @param device A string identifying the block device (e.g., "hda", "hdb").
  * @param fs_name The name of the filesystem driver to use (e.g., "FAT32").
  * @param flags Mount flags (e.g., MS_READONLY - TBD). Currently unused.
  * @return FS_SUCCESS (0) on success, or a negative fs_errno code on failure.
  */
 fs_error_t mount_filesystem(const char *mount_point, const char *device, const char *fs_name, uint32_t flags);
 
 /**
  * @brief Unmounts a filesystem previously mounted at the specified mount point.
  * Interacts with the VFS to call the driver's unmount implementation and
  * remove the mount point from the global table.
  *
  * @param mount_point The exact absolute path of the mount point to unmount.
  * @return FS_SUCCESS (0) on success, or a negative fs_errno code on failure
  * (e.g., -FS_ERR_NOT_FOUND, -FS_ERR_BUSY).
  */
 fs_error_t unmount_filesystem(const char *mount_point);
 
 /**
  * @brief Lists all currently mounted filesystems to the kernel console.
  * This is primarily a debugging utility. It retrieves the list from the
  * underlying mount table manager.
  */
 void list_mounts(void); // Keep name consistent with previous usage
 
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // MOUNT_H