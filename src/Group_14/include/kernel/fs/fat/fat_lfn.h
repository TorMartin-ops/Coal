/**
 * @file fat_lfn.h
 * @brief FAT Long File Name (LFN) handling functions.
 */

 #ifndef FAT_LFN_H
 #define FAT_LFN_H

 #include <kernel/fs/fat/fat_core.h>   // Core FAT structures (fat_fs_t, fat_dir_entry_t, fat_lfn_entry_t)
 #include <libc/stdint.h> // Standard integer types
 #include <libc/stddef.h> // For size_t

 /* --- LFN Constants --- */
 #define FAT_LFN_ENTRY_LAST_FLAG 0x40 // Mask for sequence number byte indicating the last LFN entry

 // *** REMOVED REDEFINITION of FAT_MAX_LFN_CHARS (defined in fat_dir.h) ***
 // #define FAT_MAX_LFN_CHARS 255 // Maximum characters in a long filename (practical limit)

 #ifdef __cplusplus
 extern "C" {
 #endif


 /* --- LFN Handling Functions --- */

 /**
  * @brief Calculates the LFN checksum for a given 8.3 short filename.
  */
 uint8_t fat_calculate_lfn_checksum(const uint8_t name_8_3[11]);

 /**
  * @brief Reconstructs a long filename from an array of LFN entries.
  */
 void fat_reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                           char *lfn_buf, size_t lfn_buf_size);

 /**
  * @brief Generates the LFN directory entry structures for a given long filename.
  */
 int fat_generate_lfn_entries(const char* long_name,
                                uint8_t short_name_checksum,
                                fat_lfn_entry_t* lfn_buf,
                                int max_lfn_entries);

 // NOTE: fat_generate_unique_short_name removed from here, now declared as
 // fat_generate_short_name in fat_utils.h

 #ifdef __cplusplus
 }
 #endif

 #endif /* FAT_LFN_H */

