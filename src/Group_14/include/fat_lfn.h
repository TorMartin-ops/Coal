/**
 * @file fat_lfn.h
 * @brief FAT Long File Name (LFN) handling functions.
 *
 * Declares functions for calculating LFN checksums, reconstructing long filenames
 * from LFN directory entries, generating LFN entries from a given name, and
 * generating a corresponding (unique) 8.3 short filename.
 */

 #ifndef FAT_LFN_H
 #define FAT_LFN_H
 
 #include "fat_core.h"   // Core FAT structures (fat_fs_t, fat_dir_entry_t, fat_lfn_entry_t)
 #include <libc/stdint.h> // Standard integer types
 #include <stddef.h>     // For size_t
 
 /* --- LFN Constants --- */
 #define FAT_LFN_ENTRY_LAST_FLAG 0x40 // Mask for sequence number byte indicating the last LFN entry
 
 
 /* --- LFN Handling Functions --- */
 
 /**
  * @brief Calculates the LFN checksum for a given 8.3 short filename.
  *
  * This checksum is stored in each LFN entry and used to associate LFN entries
  * with their corresponding 8.3 entry, especially after file recovery tools.
  *
  * @param name_8_3 Pointer to the 11-byte short filename (no null terminator).
  * @return The calculated 8-bit checksum.
  */
 uint8_t fat_calculate_lfn_checksum(const uint8_t name_8_3[11]);
 
 /**
  * @brief Reconstructs a long filename from an array of LFN entries.
  *
  * Assumes the LFN entries are provided in the order they appear on disk
  * (i.e., last logical part first, with decreasing sequence numbers).
  * Handles simple UTF-16 to ASCII conversion (truncating non-ASCII).
  *
  * @param lfn_entries Pointer to the array of LFN entries.
  * @param lfn_count The number of LFN entries in the array.
  * @param lfn_buf Output buffer to store the reconstructed null-terminated filename.
  * @param lfn_buf_size The size of the output buffer 'lfn_buf'.
  */
 void fat_reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                          char *lfn_buf, size_t lfn_buf_size);
 
 /**
  * @brief Generates the LFN directory entry structures for a given long filename.
  *
  * Creates the necessary LFN entries based on the provided long name and the
  * checksum derived from the corresponding short name. Populates the buffer
  * 'lfn_buf' with these entries in the order they should be written to disk
  * (last logical entry first).
  *
  * @param long_name The null-terminated long filename string.
  * @param short_name_checksum The 8-bit checksum calculated from the 8.3 short name.
  * @param lfn_buf Output buffer where the generated LFN entries will be stored.
  * Must be large enough to hold 'max_lfn_entries'.
  * @param max_lfn_entries The maximum number of LFN entries the buffer can hold.
  * @return The number of LFN entries generated (can be 0 if long_name is 8.3 compatible
  * or if an error occurs, e.g., name too long for buffer). Returns -1 on error.
  */
 int fat_generate_lfn_entries(const char* long_name,
                              uint8_t short_name_checksum,
                              fat_lfn_entry_t* lfn_buf,
                              int max_lfn_entries);
 
 /**
  * @brief Generates a unique 8.3 short filename based on a long filename.
  *
  * Creates a base 8.3 name according to FAT rules (stripping invalid chars,
  * converting to uppercase, separating base and extension).
  * !! IMPORTANT !! This implementation is currently a placeholder and does NOT
  * guarantee uniqueness by checking for collisions within the parent directory
  * and generating ~N suffixes. A full implementation requires directory scanning.
  *
  * @param fs Pointer to the FAT filesystem structure. (Needed for full collision check).
  * @param parent_dir_cluster Cluster number of the parent directory. (Needed for full collision check).
  * @param long_name The null-terminated long filename string.
  * @param short_name_out Output buffer (11 bytes, NO null terminator) for the generated 8.3 name.
  * @return FS_SUCCESS (0) on success (placeholder always returns success).
  * A full implementation would return error codes for failures like
  * no unique name possible or I/O errors during collision checking.
  */
 int fat_generate_unique_short_name(fat_fs_t *fs,
                                    uint32_t parent_dir_cluster,
                                    const char* long_name,
                                    uint8_t short_name_out[11]);
 
 #endif /* FAT_LFN_H */