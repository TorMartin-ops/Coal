/**
 * @file fat_dir.h
 * @brief Directory operations for FAT filesystem driver.
 *
 * Declares functions responsible for directory traversal, entry lookup,
 * file/directory opening, creation (O_CREAT), truncation (O_TRUNC),
 * deletion (unlink), and reading directory contents (readdir).
 */

 #ifndef FAT_DIR_H
 #define FAT_DIR_H
 
 #include <kernel/fs/fat/fat_core.h>   // Core FAT structures (fat_fs_t, fat_dir_entry_t, etc.)
 #include <kernel/fs/vfs/fs_errno.h>   // Filesystem error codes
 #include <kernel/fs/vfs/vfs.h>        // For vnode_t, file_t, struct dirent definitions
 
 /* --- Constants --- */
 
 // Maximum number of LFN entries per filename (supporting up to 255 chars)
 // Note: Actual max path length might be limited further by buffers.
 #define FAT_MAX_LFN_ENTRIES     20
 
 // Maximum characters in a Long File Name derived from entry count.
 #define FAT_MAX_LFN_CHARS       (FAT_MAX_LFN_ENTRIES * 13)
 
 // Directory Entry flags/markers
 #define FAT_DIR_ENTRY_DELETED   0xE5 // Standard marker for deleted entries
 #define FAT_DIR_ENTRY_UNUSED    0x00 // Marker for the first unused entry (end of directory)
 #define FAT_DIR_ENTRY_KANJI     0x05 // Special start for Shift-JIS filenames (escaped as 0xE5)
 
 /* --- VFS Operations Implemented in fat_dir.c --- */
 
 /**
  * @brief Opens or creates a file/directory node within the FAT filesystem.
  *
  * Handles path resolution, checks permissions, and manages O_CREAT and O_TRUNC flags.
  * Allocates vnode and fat_file_context_t structures.
  *
  * @param fs_context Pointer to the fat_fs_t instance (obtained during mount).
  * @param path The absolute path to the file or directory.
  * @param flags VFS open flags (e.g., O_RDONLY, O_CREAT, O_TRUNC).
  * @return A pointer to the allocated vnode_t on success, NULL on failure (sets fs_errno).
  */
 vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
 
 /**
  * @brief Reads the next directory entry from an opened directory.
  *
  * Retrieves the entry specified by 'entry_index', handling both LFN and 8.3 names.
  * Populates the VFS 'struct dirent'. Manages internal state within the file handle
  * for sequential reads.
  *
  * @param dir_file Pointer to the VFS file_t representing the opened directory.
  * @param d_entry_out Pointer to the VFS struct dirent to populate.
  * @param entry_index The logical index of the directory entry to retrieve (0-based).
  * Must be sequential or 0 to reset iteration.
  * @return FS_SUCCESS (0) if an entry was found and returned.
  * @return -FS_ERR_NOT_FOUND if the end of the directory is reached or index is invalid.
  * @return Other negative FS_ERR_* codes on error (e.g., -FS_ERR_IO).
  */
 int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index);
 
 /**
  * @brief Deletes a file from the FAT filesystem.
  *
  * Finds the file's directory entry, marks it (and associated LFN entries) as deleted,
  * and frees the cluster chain allocated to the file. Does not currently support
  * deleting non-empty directories.
  *
  * @param fs_context Pointer to the fat_fs_t instance.
  * @param path The absolute path to the file to delete.
  * @return FS_SUCCESS (0) on success.
  * @return -FS_ERR_IS_A_DIRECTORY if the path refers to a directory.
  * @return -FS_ERR_PERMISSION_DENIED if the file is read-only.
  * @return -FS_ERR_NOT_FOUND if the path does not exist.
  * @return Other negative FS_ERR_* codes on error.
  */
 int fat_unlink_internal(void *fs_context, const char *path);
 
 
 /* --- Internal Helper Functions (Potentially used by other FAT modules) --- */
 
 /**
  * @brief Looks up a path component within a specific directory cluster.
  *
  * Scans a single directory (defined by start_cluster) for a matching filename
  * component (LFN or 8.3). Does NOT handle multi-component paths ("a/b/c").
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param dir_cluster The starting cluster of the directory to search (0 for FAT12/16 root).
  * @param component The single filename component to search for.
  * @param entry_out Pointer to store the found 8.3 directory entry.
  * @param lfn_out Buffer to store the reconstructed LFN (can be NULL if not needed).
  * @param lfn_max_len Size of the lfn_out buffer.
  * @param entry_offset_in_dir_out Pointer to store the byte offset of the found 8.3 entry within the directory data.
  * @param first_lfn_offset_out Pointer to store the byte offset of the *first* LFN entry associated with the found file (optional, can be NULL).
  * @return FS_SUCCESS (0) if found.
  * @return -FS_ERR_NOT_FOUND if the component is not found.
  * @return Other negative FS_ERR_* codes on error.
  */
 int fat_find_in_dir(fat_fs_t *fs,
                     uint32_t dir_cluster,
                     const char *component,
                     fat_dir_entry_t *entry_out,
                     char *lfn_out, size_t lfn_max_len,
                     uint32_t *entry_offset_in_dir_out,
                     uint32_t *first_lfn_offset_out); // Added LFN offset output
 
 /**
  * @brief Resolves a full absolute path to its final directory entry.
  *
  * Traverses the directory structure from the root based on the path components.
  * Handles "." and ".." components (basic implementation).
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param path The absolute path string (must start with '/').
  * @param entry_out Pointer to store the final 8.3 directory entry.
  * @param lfn_out Buffer to store the final reconstructed LFN (can be NULL).
  * @param lfn_max_len Size of the lfn_out buffer.
  * @param entry_dir_cluster_out Pointer to store the cluster number of the *parent* directory containing the final entry.
  * @param entry_offset_in_dir_out Pointer to store the byte offset of the final 8.3 entry within its parent directory.
  * @return FS_SUCCESS (0) if the path resolves successfully.
  * @return -FS_ERR_NOT_FOUND if any component of the path is not found.
  * @return -FS_ERR_NOT_A_DIRECTORY if an intermediate component is not a directory.
  * @return Other negative FS_ERR_* codes on error.
  */
 int fat_lookup_path(fat_fs_t *fs, const char *path,
                      fat_dir_entry_t *entry_out,
                      char *lfn_out, size_t lfn_max_len,
                      uint32_t *entry_dir_cluster_out, // Cluster of the PARENT dir
                      uint32_t *entry_offset_in_dir_out); // Offset of the entry itself
 
 /**
  * @brief Reads a specific sector from a directory cluster chain.
  *
  * Handles the difference between regular directories (cluster chain) and the
  * fixed-location FAT12/16 root directory. Uses the buffer cache.
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller if modifications occur elsewhere.
  * @param cluster The starting cluster of the directory (use 0 for FAT12/16 root).
  * @param sector_offset_in_chain The logical sector offset within the directory's data stream.
  * @param buffer Pointer to the buffer where the sector data should be stored (must be fs->bytes_per_sector).
  * @return FS_SUCCESS (0) on success.
  * @return -FS_ERR_NOT_FOUND if the offset is beyond the directory size (or root dir bounds).
  * @return Negative FS_ERR_* code on I/O error or invalid cluster chain.
  */
 int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                           uint32_t sector_offset_in_chain,
                           uint8_t* buffer);
 
 /**
  * @brief Updates an existing 8.3 directory entry on disk.
  *
  * Reads the sector containing the entry, modifies it in memory, marks the
  * buffer dirty, and releases it. Uses the buffer cache.
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param dir_cluster Cluster number of the directory containing the entry.
  * @param dir_offset Byte offset of the 8.3 entry within the directory's data.
  * @param new_entry Pointer to the updated fat_dir_entry_t data.
  * @return FS_SUCCESS (0) on success, negative FS_ERR_* code on failure.
  */
 int update_directory_entry(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const fat_dir_entry_t *new_entry);
 
 /**
  * @brief Marks one or more directory entries as deleted.
  *
  * Used by unlink to mark the 8.3 entry and preceding LFN entries.
  * Reads the sector, modifies the first byte(s), marks buffer dirty.
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param dir_cluster Cluster number of the directory containing the entry.
  * @param first_entry_offset Byte offset of the *first* entry to mark (can be an LFN entry).
  * @param num_entries Number of consecutive 32-byte entries to mark.
  * @param marker The deletion marker (usually FAT_DIR_ENTRY_DELETED).
  * @return FS_SUCCESS (0) on success, negative FS_ERR_* code on failure.
  */
 int mark_directory_entries_deleted(fat_fs_t *fs,
                                    uint32_t dir_cluster,
                                    uint32_t first_entry_offset,
                                    size_t num_entries,
                                    uint8_t marker);
 
 /**
  * @brief Writes one or more consecutive directory entries to disk.
  *
  * Used during file creation (O_CREAT) to write LFN entries followed by the 8.3 entry.
  * Handles writes potentially spanning sector boundaries. Uses the buffer cache.
  * Requires sufficient free space to already exist at the target offset.
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param dir_cluster Cluster number of the directory to write into.
  * @param dir_offset Byte offset within the directory's data stream to start writing.
  * @param entries_buf Pointer to the buffer containing the directory entries to write.
  * @param num_entries The number of 32-byte entries in the buffer.
  * @return FS_SUCCESS (0) on success, negative FS_ERR_* code on failure.
  */
 int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries);

                             






int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
uint32_t sector_offset_in_chain,
uint8_t* buffer);
 
 /**
  * @brief Finds a sequence of free slots in a directory.
  *
  * Scans the directory data for a contiguous block of 'needed_slots' marked
  * as deleted (0xE5) or never used (0x00).
  * NOTE: This implementation may be basic and might not handle extending the directory
  * file itself if no contiguous block is found.
  *
  * @param fs Pointer to the fat_fs_t instance. Assumed locked by caller.
  * @param parent_dir_cluster Cluster number of the directory to search.
  * @param needed_slots The number of consecutive 32-byte slots required.
  * @param out_slot_cluster Pointer to store the cluster where the slots were found.
  * @param out_slot_offset Pointer to store the byte offset of the first free slot.
  * @return FS_SUCCESS (0) if found, -FS_ERR_NO_SPACE if not enough space, other errors possible.
  */
 int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                              size_t needed_slots,
                              uint32_t *out_slot_cluster,
                              uint32_t *out_slot_offset);





 #endif /* FAT_DIR_H */