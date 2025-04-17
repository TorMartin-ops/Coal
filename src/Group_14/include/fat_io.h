/**
 * @file fat_io.h
 * @brief File I/O operations (read, write, seek, close) for FAT filesystem.
 *
 * Declares the VFS file operation functions implemented by the FAT driver,
 * handling data transfer, file position management, and file closure logic.
 * Also declares helpers for reading/writing data at the cluster level via
 * the buffer cache.
 */

 #ifndef FAT_IO_H
 #define FAT_IO_H
 
 #include "fat_core.h"   // Core FAT structures (fat_fs_t, fat_file_context_t)
 #include "fs_errno.h"   // Filesystem error codes
 #include "vfs.h"        // For file_t definition
 #include <libc/stdint.h>
 #include <libc/limits.h> // For LONG_MAX/MIN in lseek if needed, or size_t
 
 /* --- VFS Operations Implemented in fat_io.c --- */
 
 /**
  * @brief Reads data from an opened file.
  *
  * Reads 'len' bytes starting from the file's current offset into 'buf'.
  * Handles traversing the FAT cluster chain and reading data via the buffer cache.
  * Updates the file's offset after reading.
  *
  * @param file Pointer to the VFS file_t structure representing the opened file.
  * @param buf Destination buffer for the read data.
  * @param len Maximum number of bytes to read.
  * @return The number of bytes actually read (can be less than len if EOF is reached),
  * or a negative FS_ERR_* code on failure. Returns 0 on EOF if offset is >= size.
  */
 int fat_read_internal(file_t *file, void *buf, size_t len);
 
 /**
  * @brief Writes data to an opened file.
  *
  * Writes 'len' bytes from 'buf' to the file starting at the file's current offset
  * (or at the end if O_APPEND is set). Handles traversing the FAT cluster chain,
  * allocating new clusters if the file needs to be extended, and writing data
  * via the buffer cache. Updates the file's offset and size, marking the context dirty.
  *
  * @param file Pointer to the VFS file_t structure representing the opened file.
  * @param buf Source buffer containing the data to write.
  * @param len Number of bytes to write.
  * @return The number of bytes actually written (can be less than len if disk space runs out),
  * or a negative FS_ERR_* code on failure.
  */
 int fat_write_internal(file_t *file, const void *buf, size_t len);
 
 /**
  * @brief Changes the current read/write offset of an opened file.
  *
  * Updates the file's offset based on the 'offset' and 'whence' parameters.
  *
  * @param file Pointer to the VFS file_t structure.
  * @param offset The offset value (meaning depends on whence).
  * @param whence The reference point for the seek (SEEK_SET, SEEK_CUR, SEEK_END).
  * @return The resulting file offset from the beginning of the file on success,
  * or (off_t)-1 cast equivalent on failure (e.g., invalid whence, overflow, negative result).
  * Specific error code might be set via fs_set_errno().
  */
 off_t fat_lseek_internal(file_t *file, off_t offset, int whence);
 
 /**
  * @brief Closes an opened file.
  *
  * Performs necessary cleanup, including flushing metadata (size, first cluster)
  * back to the directory entry if the file context was marked dirty. Frees the
  * allocated vnode and file context memory.
  *
  * @param file Pointer to the VFS file_t structure.
  * @return FS_SUCCESS (0) on success, or a negative FS_ERR_* code on failure.
  */
 int fat_close_internal(file_t *file);
 
 
 /* --- Cluster I/O Helpers (Potentially used by other FAT modules) --- */
 
 /**
  * @brief Reads a block of data from a specific cluster using the buffer cache.
  *
  * Handles reading data that might span multiple sectors within the cluster.
  * Assumes the cluster number is valid (>= 2).
  *
  * @param fs Pointer to the fat_fs_t instance.
  * @param cluster The cluster number to read from (must be >= 2).
  * @param offset_in_cluster Byte offset within the cluster to start reading from.
  * @param buf Destination buffer.
  * @param len Number of bytes to read (must fit within cluster boundary from offset).
  * @return Number of bytes read (should equal len on success), or negative FS_ERR_* code on error.
  */
 int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                         void *buf, size_t len);
 
 /**
  * @brief Writes a block of data to a specific cluster using the buffer cache.
  *
  * Handles writing data that might span multiple sectors within the cluster.
  * Marks the relevant buffer cache blocks as dirty. Assumes the cluster number
  * is valid (>= 2).
  *
  * @param fs Pointer to the fat_fs_t instance.
  * @param cluster The cluster number to write to (must be >= 2).
  * @param offset_in_cluster Byte offset within the cluster to start writing to.
  * @param buf Source buffer containing the data to write.
  * @param len Number of bytes to write (must fit within cluster boundary from offset).
  * @return Number of bytes written (should equal len on success), or negative FS_ERR_* code on error.
  */
 int write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                          const void *buf, size_t len);
 
 #endif /* FAT_IO_H */