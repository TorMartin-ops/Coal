/**
 * mount_table.h - Header for the global mount table manager.
 *
 * Defines the public interface for managing mounted filesystems, including
 * thread-safe operations for adding, removing, finding, and listing mounts.
 */

 #pragma once // Modern header guard

 #ifndef MOUNT_TABLE_H
 #define MOUNT_TABLE_H
 
 #include <kernel/core/types.h>  // Basic types like bool, size_t
 #include <kernel/fs/vfs/mount.h>  // Crucially includes the definition of mount_t
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 // --- Public Function Declarations ---
 
 /**
  * @brief Initializes the mount table subsystem.
  * Must be called once before any other mount_table functions. Initializes locks.
  */
 void mount_table_init(void);
 
 /**
  * @brief Adds a mount entry to the global mount table (thread-safe).
  * Takes ownership of the mount_t structure and its mount_point string
  * (which must be heap-allocated).
  *
  * @param mnt Pointer to the fully populated mount_t structure to add.
  * @return FS_SUCCESS (0) on success, or a negative fs_errno.h code on failure
  * (e.g., -FS_ERR_INVALID_PARAM, -FS_ERR_FILE_EXISTS).
  */
 int mount_table_add(mount_t *mnt);
 
 /**
  * @brief Removes a mount entry identified by the mount point string (thread-safe).
  * Frees the mount_t structure and its associated mount_point string upon successful removal.
  *
  * @param mount_point The exact mount point string (e.g., "/") to remove.
  * @return FS_SUCCESS (0) on success, -FS_ERR_NOT_FOUND if the mount point doesn't exist,
  * or other negative fs_errno.h code.
  */
 int mount_table_remove(const char *mount_point);
 
 /**
  * @brief Searches for a mount entry by its exact mount point string (thread-safe).
  *
  * @param mount_point The mount point string to search for. If NULL, behavior is
  * implementation-defined (currently returns list head, but this is not guaranteed
  * and unsafe for external iteration without locking).
  * @return Pointer to the found mount_t entry if found, or NULL otherwise.
  * The caller MUST NOT free the returned pointer.
  */
 mount_t *mount_table_find(const char *mount_point);
 
 /**
  * @brief Prints all current mount table entries to the kernel console (thread-safe).
  * Useful for debugging.
  */
 void mount_table_list(void);
 
 /**
  * @brief Gets the head of the mount list for external iteration (lock-free read).
  * NOTE: Iterating the returned list without external locking is NOT thread-safe
  * if the list can be modified concurrently by other threads/CPUs.
  *
  * @return Pointer to the first mount_t entry, or NULL if the list is empty.
  */
 mount_t *mount_table_get_head(void);
 
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // MOUNT_TABLE_H