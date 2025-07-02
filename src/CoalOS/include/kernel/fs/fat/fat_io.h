/**
 * @file fat_io.h
 * @brief File I/O operations (read, write, seek, close) for FAT filesystem.
 * @version 1.1 - Added fat_read_internal/fat_write_internal declarations
 *
 * Declares the VFS file operation functions implemented by the FAT driver,
 * handling data transfer, file position management, and file closure logic.
 * Also declares helpers for reading/writing data at the cluster level via
 * the buffer cache.
 */

 #ifndef FAT_IO_H
 #define FAT_IO_H
 
 #include <kernel/fs/fat/fat_core.h>   // Core FAT structures (fat_fs_t, fat_file_context_t)
 #include <kernel/fs/vfs/fs_errno.h>   // Filesystem error codes
 #include <kernel/fs/vfs/vfs.h>        // For file_t definition
 #include <libc/stdint.h>
 #include <libc/limits.h> // For LONG_MAX/MIN in lseek if needed, or size_t
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /* --- VFS Operations Implemented in fat_io.c --- */
 
 /**
  * @brief Reads data from an opened file. Implements VFS read operation.
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
  * @brief Writes data to an opened file. Implements VFS write operation.
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
  * @brief Changes the current read/write offset of an opened file. Implements VFS lseek.
  *
  * Updates the file's offset based on the 'offset' and 'whence' parameters.
  *
  * @param file Pointer to the VFS file_t structure.
  * @param offset The offset value (meaning depends on whence).
  * @param whence The reference point for the seek (SEEK_SET, SEEK_CUR, SEEK_END).
  * @return The resulting file offset from the beginning of the file on success,
  * or (off_t)-1 cast equivalent on failure (e.g., invalid whence, overflow, negative result).
  */
 off_t fat_lseek_internal(file_t *file, off_t offset, int whence);
 
 /**
  * @brief Closes an opened file. Implements VFS close.
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
  * @brief Reads a block of data from a specific cluster or FAT12/16 root dir area.
  *
  * Handles reads spanning sector boundaries within the cluster/area.
  * Assumes the cluster number is valid (>= 2 for data, 0 for FAT12/16 root).
  * Assumes offset and length are validated by the caller against location size.
  *
  * @param fs Pointer to the fat_fs_t instance.
  * @param cluster The cluster number to read from (0 for FAT12/16 root).
  * @param offset_in_location Byte offset within the cluster/root dir area to start reading from.
  * @param buf Destination buffer.
  * @param len Number of bytes to read (must fit within location boundary from offset).
  * @return Number of bytes read (should equal len on success), or negative FS_ERR_* code on error.
  */
 int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_location,
                         void *buf, size_t len);
 
 /**
  * @brief Writes a block of data to a specific data cluster using the buffer cache.
  *
  * Handles writing data that might span multiple sectors within the cluster.
  * Marks the relevant buffer cache blocks as dirty. Assumes the cluster number
  * is valid (>= 2). Cannot be used for FAT12/16 root directory writes.
  * Assumes offset and length are validated by the caller against cluster size.
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



                          
int update_directory_entry_first_cluster_now(fat_fs_t *fs, fat_file_context_t *fctx);

int update_directory_entry_size_now(fat_fs_t *fs, fat_file_context_t *fctx);


 
 #ifdef __cplusplus
 }
 #endif
 
 #endif /* FAT_IO_H */
 