/**
 * @file fat_lfn.c
 * @brief FAT Long File Name (LFN) handling implementation.
 *
 * Provides functions for LFN checksum calculation, reconstruction, entry generation,
 * and unique 8.3 short name generation with collision detection.
 */

 #include "fat_lfn.h"
 #include "fat_core.h"
 #include "fat_dir.h"  // Might need internal dir scanning functions if raw_short_name_exists isn't separate
 #include "fat_utils.h"  // Needs fat_format_filename_raw and potentially fat_raw_short_name_exists prototype
 #include "terminal.h"   // For logging
 #include <string.h>     // For strlen, memcpy, memset
 #include "fs_errno.h"   // Error codes
 #include "assert.h"     // KERNEL_ASSERT
 
 /* --- Forward Declaration for Required Helper --- */
 /* This function is assumed to be implemented elsewhere (e.g., fat_dir.c or fat_utils.c)
  * It needs to scan the directory represented by dir_cluster for an entry
  * whose raw 11-byte name matches the provided short_name_raw.
  * Returns: true if found, false otherwise. Needs to handle FAT12/16 root dir case.
  */
 extern bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]);
 
 
 /* --- Static Helper Prototypes --- */
 static int _itoa_simple(int value, char* buf, int max_len);
 
 
 /**
  * @brief Calculates the LFN checksum for a given 8.3 short filename.
  * (Based on standard algorithm)
  */
 uint8_t fat_calculate_lfn_checksum(const uint8_t name_8_3[11])
 {
     KERNEL_ASSERT(name_8_3 != NULL);
     uint8_t sum = 0;
     for (int i = 0; i < 11; i++) {
         // Rotate right 1 bit, add next byte
         sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name_8_3[i];
     }
     return sum;
 }
 
 /**
  * @brief Reconstructs a long filename from an array of LFN entries.
  * (Based on user's previous implementation)
  */
 void fat_reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                          char *lfn_buf, size_t lfn_buf_size)
 {
     KERNEL_ASSERT(lfn_buf != NULL);
     KERNEL_ASSERT(lfn_buf_size > 0);
     KERNEL_ASSERT(lfn_count >= 0); // Allow 0 count
 
     if (lfn_count == 0) {
          lfn_buf[0] = '\0';
          return;
     }
     KERNEL_ASSERT(lfn_entries != NULL); // Only assert non-NULL if count > 0
 
     lfn_buf[0] = '\0';
     int buf_idx = 0;
 
     // LFN entries are stored on disk in reverse order (last part first).
     // Iterate through the collected entries from highest sequence number to lowest.
     // Assuming lfn_entries[0] holds the entry with the highest sequence number (last on disk).
     for (int i = 0; i < lfn_count; i++) {
         // Optional: Validate sequence number and LAST_FLAG consistency
 
         // Define the parts of the LFN entry containing name characters
         const uint16_t *parts[] = { lfn_entries[i].name1,
                                     lfn_entries[i].name2,
                                     lfn_entries[i].name3 };
         const size_t counts[] = {5, 6, 2}; // Number of chars in each part
 
         bool sequence_terminated = false;
         for (int p = 0; p < 3 && !sequence_terminated; p++) { // Iterate through name1, name2, name3
             for (size_t c = 0; c < counts[p]; c++) { // Iterate through chars in the part
                 uint16_t wide_char = parts[p][c];
 
                 // Check for null terminator (0x0000) or padding (0xFFFF)
                 if (wide_char == 0x0000) {
                     sequence_terminated = true;
                     break; // End of name found
                 }
                 if (wide_char == 0xFFFF) {
                     // Padding character, effectively end of name for this entry part
                     // Note: Microsoft docs say padding should only happen after null terminator,
                     // but some implementations might just pad unused slots. We treat it as end.
                     // sequence_terminated = true; // Treat padding as terminator? Safer to just skip.
                     continue;
                 }
 
                 // Check buffer space (leave room for null terminator)
                 if (buf_idx < (int)lfn_buf_size - 1) {
                     // Simple UTF-16 to ASCII conversion:
                     char ascii_char = (wide_char < 128 && wide_char > 0) ? (char)wide_char : '?';
                     lfn_buf[buf_idx++] = ascii_char;
                 } else {
                     // Buffer full, stop processing
                     terminal_printf("[FAT LFN Reconstruct] Warning: LFN buffer full, name truncated.\n");
                     sequence_terminated = true;
                     break;
                 }
             }
         }
         if(sequence_terminated) break; // Stop processing entries if name ended mid-sequence
     }
     lfn_buf[buf_idx] = '\0'; // Ensure null termination
 }
 
 
 /**
  * @brief Generates the LFN directory entry structures for a given long filename.
  * (Based on user's previous implementation)
  */
 int fat_generate_lfn_entries(const char* long_name,
                              uint8_t short_name_checksum,
                              fat_lfn_entry_t* lfn_buf,
                              int max_lfn_entries)
 {
     KERNEL_ASSERT(long_name != NULL && lfn_buf != NULL);
     KERNEL_ASSERT(max_lfn_entries > 0);
 
     size_t lfn_len = strlen(long_name);
     if (lfn_len == 0 || lfn_len > FAT_MAX_LFN_CHARS) { // Check max LFN length supported
          terminal_printf("[FAT LFN Generate] Error: Invalid long name length (%u).\n", lfn_len);
          return -1;
     }
 
     // Calculate how many LFN entries are needed (13 characters per entry)
     int needed_entries = (int)((lfn_len + 12) / 13); // Ceiling division
 
     if (needed_entries == 0) {
          return 0; // Should not happen if lfn_len > 0
     }
     if (needed_entries > max_lfn_entries) {
         terminal_printf("[FAT LFN Generate] Error: Long name '%s' requires %d LFN entries, buffer only holds %d.\n",
                          long_name, needed_entries, max_lfn_entries);
         return -1; // Indicate error (buffer too small)
     }
 
     // Fill entries in reverse order (matching disk layout)
     for (int seq = 1; seq <= needed_entries; seq++) {
         int entry_index = needed_entries - seq; // Index in our output buffer
         fat_lfn_entry_t *entry = &lfn_buf[entry_index];
 
         // Initialize entry fields
         memset(entry, 0xFF, sizeof(*entry)); // Pad with 0xFF initially
         entry->attr      = FAT_ATTR_LONG_NAME;
         entry->type      = 0; // Must be 0 for LFN
         entry->checksum  = short_name_checksum;
         entry->first_cluster_zero = 0; // Must be 0 for LFN
 
         // Set sequence number (1-based) and LAST flag for the first logical entry (last on disk)
         uint8_t seq_num = (uint8_t)seq;
         if (seq == needed_entries) {
             seq_num |= FAT_LFN_ENTRY_LAST_FLAG;
         }
         entry->seq_num = seq_num;
 
         // Fill name characters (13 per entry)
         int name_start_index = (seq - 1) * 13;
         bool name_terminated = false;
 
         // Helper lambda/function could simplify this, but plain C approach:
         uint16_t *target_part;
         int char_index_in_lfn = 0;
 
         // Part 1: name1 (5 chars)
         target_part = entry->name1;
         for (int i = 0; i < 5; i++, char_index_in_lfn++) {
             int source_idx = name_start_index + char_index_in_lfn;
             if (source_idx < (int)lfn_len) {
                  // Simple ASCII to UTF-16 conversion
                  target_part[i] = (uint16_t)(unsigned char)long_name[source_idx];
             } else {
                  target_part[i] = name_terminated ? 0xFFFF : 0x0000; // Null terminate, then pad
                  name_terminated = true;
             }
         }
         // Part 2: name2 (6 chars)
         target_part = entry->name2;
          for (int i = 0; i < 6; i++, char_index_in_lfn++) {
              int source_idx = name_start_index + char_index_in_lfn;
              if (source_idx < (int)lfn_len) {
                  target_part[i] = (uint16_t)(unsigned char)long_name[source_idx];
              } else {
                  target_part[i] = name_terminated ? 0xFFFF : 0x0000;
                  name_terminated = true;
              }
          }
         // Part 3: name3 (2 chars)
         target_part = entry->name3;
          for (int i = 0; i < 2; i++, char_index_in_lfn++) {
              int source_idx = name_start_index + char_index_in_lfn;
              if (source_idx < (int)lfn_len) {
                  target_part[i] = (uint16_t)(unsigned char)long_name[source_idx];
              } else {
                  target_part[i] = name_terminated ? 0xFFFF : 0x0000;
                  name_terminated = true;
              }
          }
     } // End loop through needed entries
 
     return needed_entries;
 }
 
 
 /**
  * @brief Generates a unique 8.3 short filename based on a long filename.
  *
  * This function attempts to create a unique 8.3 filename within the specified
  * parent directory. It first generates a base name according to 8.3 rules.
  * If that name collides with an existing entry, it attempts to append "~N"
  * suffixes (e.g., "FILE~1.TXT", "MYPROG~2.C") until a unique name is found
  * or a limit is reached.
  *
  * @param fs Pointer to the FAT filesystem context.
  * @param parent_dir_cluster The cluster number of the parent directory (0 for FAT12/16 root).
  * @param long_name The desired long filename.
  * @param short_name_out Buffer of 11 bytes to store the generated unique 8.3 name.
  * @return FS_SUCCESS (0) if a unique name was generated, or a negative FS_ERR_* code on failure.
  */
 int fat_generate_unique_short_name(fat_fs_t *fs,
                                    uint32_t parent_dir_cluster,
                                    const char* long_name,
                                    uint8_t short_name_out[11])
 {
     KERNEL_ASSERT(fs != NULL && long_name != NULL && short_name_out != NULL);
     // Assumes caller holds fs->lock if necessary for fat_raw_short_name_exists
 
     uint8_t base_name[11];
     uint8_t trial_name[11];
     char num_suffix[8]; // Buffer for "~N" string (~ + 6 digits + null)
 
     // 1. Generate the initial base 8.3 name using the helper from fat_utils.c
     // This handles uppercasing, padding, invalid char replacement, extension separation.
     fat_format_filename_raw(long_name, base_name);
 
     // 2. Check if the base name itself is unique
     if (!fat_raw_short_name_exists(fs, parent_dir_cluster, base_name)) {
         memcpy(short_name_out, base_name, 11);
         terminal_printf("[FAT ShortGen] Base name '%.11s' is unique for '%s'.\n", base_name, long_name);
         return FS_SUCCESS;
     }
 
     terminal_printf("[FAT ShortGen] Base name '%.11s' collides for '%s'. Generating ~N suffix...\n", base_name, long_name);
 
     // 3. Collision detected - Generate ~N variations
     // Determine how many characters of the base name we can keep.
     // Max suffix is ~999999 (7 chars), leaving 1 char for the base.
     // Example: ~1 (2 chars) -> keep 6 base chars. ~10 (3 chars) -> keep 5 base chars.
     for (int n = 1; n <= 999999; ++n) {
         // Format the numeric suffix string "~N"
         num_suffix[0] = '~';
         int num_len = _itoa_simple(n, num_suffix + 1, sizeof(num_suffix) - 1);
         if (num_len < 0) {
             terminal_printf("[FAT ShortGen] Error: Failed to convert suffix number %d to string.\n", n);
             return -FS_ERR_INTERNAL; // Error during conversion
         }
         int suffix_len = 1 + num_len; // Length of "~N"
 
         // Calculate how many base characters fit (max 8 total name chars)
         int base_chars_to_keep = 8 - suffix_len;
         if (base_chars_to_keep < 1) {
              // Should not happen with n <= 999999, as suffix_len <= 7
              base_chars_to_keep = 1;
         }
 
         // Construct the trial name
         memset(trial_name, ' ', 11); // Start with spaces
         memcpy(trial_name, base_name, base_chars_to_keep); // Copy truncated base part
         memcpy(trial_name + base_chars_to_keep, num_suffix, suffix_len); // Copy suffix
         memcpy(trial_name + 8, base_name + 8, 3); // Copy original extension
 
         // Check if this trial name exists
         if (!fat_raw_short_name_exists(fs, parent_dir_cluster, trial_name)) {
             // Found a unique name!
             memcpy(short_name_out, trial_name, 11);
             terminal_printf("[FAT ShortGen] Unique name '%.11s' found for '%s'.\n", trial_name, long_name);
             return FS_SUCCESS;
         }
          // Debug log for collision on trial name
          // terminal_printf("[FAT ShortGen] Trial name '%.11s' collides (N=%d).\n", trial_name, n);
 
     } // End for loop (N=1 to 999999)
 
     // If we exit the loop, we couldn't find a unique name after trying all suffixes.
     terminal_printf("[FAT ShortGen] Error: Could not generate unique short name for '%s' after %d attempts.\n", long_name, 999999);
     return -FS_ERR_NAMETOOLONG; // Or -FS_ERR_EXISTS or a specific error
 }
 
 
 /* --- Static Helper Implementations --- */
 
 /**
  * @brief Simple integer to ASCII conversion helper.
  * Converts a non-negative integer to a string in the provided buffer.
  * Does not handle negative numbers.
  * @param value The integer to convert.
  * @param buf Output buffer.
  * @param max_len Maximum size of the buffer (including null terminator).
  * @return Length of the generated string (excluding null), or -1 on error.
  */
 static int _itoa_simple(int value, char* buf, int max_len) {
     if (!buf || max_len <= 0) return -1;
     if (value == 0) {
         if (max_len < 2) return -1;
         buf[0] = '0';
         buf[1] = '\0';
         return 1;
     }
     if (value < 0) return -1; // Not handled
 
     int i = 0;
     int temp_val = value;
 
     // Calculate digits needed (and store in reverse order temporarily)
     while (temp_val > 0 && i < max_len - 1) {
         buf[i++] = (temp_val % 10) + '0';
         temp_val /= 10;
     }
 
     if (temp_val > 0) { // Buffer wasn't long enough
         return -1;
     }
 
     buf[i] = '\0';
     int len = i;
 
     // Reverse the string in place
     int start = 0;
     int end = len - 1;
     while (start < end) {
         char temp = buf[start];
         buf[start] = buf[end];
         buf[end] = temp;
         start++;
         end--;
     }
 
     return len;
 }