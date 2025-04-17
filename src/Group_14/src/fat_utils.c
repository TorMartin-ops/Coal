/**
 * @file fat_utils.c
 * @brief Utility functions for FAT filesystem driver.
 */

 #include "fat_utils.h"  // Header for this file's declarations
 #include "fat_core.h"   // For fat_fs_t definition
 #include <string.h>     // For strlen, strcmp, memset, memcpy
 #include "libc/ctype.h" // For toupper
 #include "fs_errno.h"   // Filesystem error codes
 #include "terminal.h"   // Logging
 #include "assert.h"     // KERNEL_ASSERT
 
 /**
  * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
  */
 uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster)
 {
     KERNEL_ASSERT(fs != NULL, "NULL fs context in fat_cluster_to_lba");
     if (cluster < 2) return 0; // Clusters 0 and 1 are reserved/invalid for data
 
     return fs->first_data_sector + ((cluster - 2) * fs->sectors_per_cluster);
 }
 
 /**
  * @brief Retrieves the next cluster in the chain from the FAT table.
  */
 int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster)
 {
     KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL && next_cluster != NULL, "Invalid arguments to fat_get_next_cluster");
 
     // Calculate maximum valid cluster index based on FAT size
     // Note: fat_size_sectors * bytes_per_sector gives total FAT size in bytes
     size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
     size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
     if (entry_size == 0) return -FS_ERR_INVALID_FORMAT; // Should not happen if mounted correctly
     uint32_t max_cluster_index = fat_size_bytes / entry_size;
 
     if (current_cluster >= max_cluster_index) {
          terminal_printf("[FAT Get Next] Error: Current cluster %u out of bounds (max %u).\n", current_cluster, max_cluster_index);
          return -FS_ERR_INVALID_PARAM;
     }
 
     // Access the appropriate FAT table based on filesystem type
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *FAT32 = (uint32_t*)fs->fat_table;
         *next_cluster = FAT32[current_cluster] & 0x0FFFFFFF; // Mask reserved bits
     }
     else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *FAT16 = (uint16_t*)fs->fat_table;
         *next_cluster = FAT16[current_cluster];
     }
     else {
         // FAT12 not fully implemented in this example
         return -FS_ERR_NOT_SUPPORTED;
     }
 
     return FS_SUCCESS; // Success
 }
 
 /**
  * @brief Updates the FAT entry for a given cluster with a new value.
  */
 int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value)
 {
     KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL, "Invalid arguments to fat_set_cluster_entry");
 
     size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
     size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
      if (entry_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t max_cluster_index = fat_size_bytes / entry_size;
 
     if (cluster >= max_cluster_index) {
          terminal_printf("[FAT Set Entry] Error: Cluster index %u out of bounds (max %u).\n", cluster, max_cluster_index);
          return -FS_ERR_INVALID_PARAM;
     }
 
     // Update the appropriate FAT table based on filesystem type
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *FAT32 = (uint32_t*)fs->fat_table;
         // Preserve the top 4 bits (reserved)
         FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
     }
     else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *FAT16 = (uint16_t*)fs->fat_table;
         FAT16[cluster] = (uint16_t)value;
     }
     else {
         // FAT12 not fully implemented in this example
         return -FS_ERR_NOT_SUPPORTED;
     }
 
     // Mark the FAT table as dirty so it gets flushed later
     // TODO: Implement a dirty flag or mechanism if needed (e.g., fs->fat_dirty = true;)
     // For now, rely on caller to sync if necessary, or sync aggressively.
 
     return FS_SUCCESS; // Success
 }
 
 /**
  * @brief Converts a standard filename to FAT 8.3 format.
  */
 void format_filename(const char *input, char output_8_3[11])
 {
     size_t i, j;
     memset(output_8_3, ' ', 11);
 
     if (!input || input[0] == '\0') {
         memcpy(output_8_3, "NO_NAME    ", 11);
         return;
     }
 
     while (*input && (*input == '.' || *input == ' ')) {
         input++;
     }
     if (!*input) {
         memcpy(output_8_3, "NO_NAME    ", 11);
         return;
     }
 
     for (i = 0, j = 0; input[i] && input[i] != '.' && j < 8; i++) {
         char c = input[i];
         // Skip invalid characters for FAT 8.3 names
         if (strchr("\"*+,/:;<=>?[\\]|", c) || c < ' ') {
              continue;
         }
         output_8_3[j++] = toupper(c);
     }
 
     while (input[i] && input[i] != '.') {
         i++;
     }
 
     if (input[i] == '.') {
         i++;
         for (j = 8; input[i] && j < 11; i++) {
             char c = input[i];
              if (strchr("\"*+,./:;<=>?[\\]|", c) || c < ' ') { // Note: '.' is also invalid in extension
                  continue;
             }
             output_8_3[j++] = toupper(c);
         }
     }
     // No null termination for raw 8.3 names
 }
 
 
 // --- Implementations added in previous steps ---
 
 /**
  * @brief Retrieves the value of a specific cluster entry from the FAT table.
  */
 int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value)
 {
     KERNEL_ASSERT(fs != NULL && entry_value != NULL, "NULL fs or entry_value pointer");
     KERNEL_ASSERT(fs->fat_table != NULL, "FAT table not loaded");
 
     size_t fat_offset;
     // Recalculate max_cluster_index based on FAT size in bytes / entry size
     size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
     size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
     if (entry_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t max_cluster_index = fat_size_bytes / entry_size;
 
     if (cluster >= max_cluster_index) {
          terminal_printf("[FAT Get Entry] Error: Cluster index %u out of bounds (max %u).\n", cluster, max_cluster_index);
          return -FS_ERR_INVALID_PARAM;
     }
 
     switch (fs->type) {
         case FAT_TYPE_FAT16:
             fat_offset = cluster * 2;
              // Use calculated fat_size_bytes for bounds check
              if (fat_offset + 1 >= fat_size_bytes) {
                  terminal_printf("[FAT Get Entry FAT16] Error: Calculated offset %u out of bounds (%u).\n", (unsigned int)fat_offset, (unsigned int)fat_size_bytes);
                  return -FS_ERR_INTERNAL;
              }
             *entry_value = (uint32_t)(*(uint16_t*)((uint8_t*)fs->fat_table + fat_offset));
             break;
         case FAT_TYPE_FAT32:
             fat_offset = cluster * 4;
             // Use calculated fat_size_bytes for bounds check
             if (fat_offset + 3 >= fat_size_bytes) {
                  terminal_printf("[FAT Get Entry FAT32] Error: Calculated offset %u out of bounds (%u).\n", (unsigned int)fat_offset, (unsigned int)fat_size_bytes);
                  return -FS_ERR_INTERNAL;
              }
             *entry_value = (*(uint32_t*)((uint8_t*)fs->fat_table + fat_offset)) & 0x0FFFFFFF; // Mask high 4 bits
             break;
         case FAT_TYPE_FAT12:
             terminal_printf("[FAT Get Entry] Error: FAT12 get_cluster_entry not fully implemented.\n");
             return -FS_ERR_NOT_SUPPORTED;
         default:
             return -FS_ERR_INVALID_FORMAT;
     }
 
     return FS_SUCCESS;
 }
 
 /**
  * @brief Compares a filename component string against a reconstructed LFN string.
  */
 int fat_compare_lfn(const char* component, const char* reconstructed_lfn) {
     // Check for NULL pointers
     if (!component || !reconstructed_lfn) {
         return -1; // Indicate difference or error
     }
 
     size_t len_comp = strlen(component);
     size_t len_lfn = strlen(reconstructed_lfn);
 
     if (len_comp != len_lfn) {
         return (int)len_comp - (int)len_lfn; // Different lengths
     }
 
     // Case-insensitive comparison
     for (size_t i = 0; i < len_comp; ++i) {
         char c1 = toupper((unsigned char)component[i]);
         char c2 = toupper((unsigned char)reconstructed_lfn[i]);
         if (c1 != c2) {
             return c1 - c2;
         }
     }
     return 0; // Match
 }
 
 /**
  * @brief Compares a filename component string against a raw 8.3 FAT filename.
  */
 int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]) {
     // Check for NULL pointers
     if (!component || !name_8_3) {
         return -1; // Indicate difference or error
     }
 
     char formatted_component[12];
     char formatted_83[12];
     size_t comp_idx = 0;
     size_t fat_idx = 0;
     size_t out_idx = 0;
 
     // Format component base name (uppercase, up to 8 chars)
     while (component[comp_idx] && component[comp_idx] != '.' && out_idx < 8) {
         formatted_component[out_idx++] = toupper((unsigned char)component[comp_idx++]);
     }
     // Pad component base with spaces if shorter than 8
     while (out_idx < 8) formatted_component[out_idx++] = ' ';
     formatted_component[8] = '\0'; // Terminate base for comparison
 
     // Format 8.3 base name from directory entry (uppercase, stop at first space)
     out_idx = 0;
     fat_idx = 0;
     while (fat_idx < 8 && name_8_3[fat_idx] != ' ') {
         // Handle KANJI escape character (0x05) -> 'E'
         formatted_83[out_idx++] = (name_8_3[fat_idx] == 0x05) ? 'E' : toupper(name_8_3[fat_idx]);
         fat_idx++;
     }
     formatted_83[out_idx] = '\0'; // Terminate base for comparison
 
     // Compare base names
     if (strcmp(formatted_component, formatted_83) != 0) {
         return 1; // Base names don't match
     }
 
     // Format component extension (uppercase, up to 3 chars)
     if (component[comp_idx] == '.') comp_idx++; // Skip dot if present
     out_idx = 0;
     while (component[comp_idx] && out_idx < 3) {
         formatted_component[out_idx++] = toupper((unsigned char)component[comp_idx++]);
     }
     // Pad component extension with spaces if shorter than 3
     while (out_idx < 3) formatted_component[out_idx++] = ' ';
     formatted_component[3] = '\0'; // Terminate extension for comparison
 
     // Format 8.3 extension from directory entry (uppercase, stop at first space)
     fat_idx = 8; // Start index of extension in raw 8.3 name
     out_idx = 0;
     while (fat_idx < 11 && name_8_3[fat_idx] != ' ') {
          // KANJI check not typically needed for extension, but safe to include
          formatted_83[out_idx++] = (name_8_3[fat_idx] == 0x05) ? 'E' : toupper(name_8_3[fat_idx]);
         fat_idx++;
     }
     formatted_83[out_idx] = '\0'; // Terminate extension for comparison
 
     // Compare extensions
     return strcmp(formatted_component, formatted_83);
 }