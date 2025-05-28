/**
 * @file fat_lfn.c
 * @brief FAT Long File Name (LFN) handling implementation.
 */

 #include <kernel/fs/fat/fat_lfn.h>
 #include <kernel/fs/fat/fat_core.h>   // Core structures, Needed for logging macros potentially
 #include <kernel/fs/fat/fat_fs.h>     // fat_lfn_entry_t, FAT_ATTR_LONG_NAME
 #include <kernel/fs/fat/fat_dir.h>    // *** ADDED: Include for FAT_MAX_LFN_CHARS definition ***
 #include <kernel/drivers/display/terminal.h>   // For logging (terminal_printf), Needed for logging macros
 #include <kernel/lib/string.h>     // For strlen, memcpy, memset
 #include <kernel/lib/assert.h>     // KERNEL_ASSERT
 #include <libc/stdint.h>
 #include <libc/stddef.h>

 // Logging Macros (ensure defined, e.g., via fat_core.h or terminal.h)
 #ifndef FAT_DEBUG_LOG
 #define FAT_DEBUG_LOG(fmt, ...) do {} while(0)
 #endif
 #ifndef FAT_ERROR_LOG
 #define FAT_ERROR_LOG(fmt, ...) terminal_printf("[FAT ERROR] " fmt "\n", ##__VA_ARGS__)
 #endif


 /**
  * @brief Calculates the LFN checksum for a given 8.3 short filename.
  */
 uint8_t fat_calculate_lfn_checksum(const uint8_t name_8_3[11])
 {
     KERNEL_ASSERT(name_8_3 != NULL, "Input 8.3 name cannot be NULL for checksum calculation");
     uint8_t sum = 0;
     for (int i = 0; i < 11; i++) {
         sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name_8_3[i];
     }
     return sum;
 }

 /**
  * @brief Reconstructs a long filename from an array of LFN entries.
  */
 void fat_reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                           char *lfn_buf, size_t lfn_buf_size)
 {
     KERNEL_ASSERT(lfn_buf != NULL, "LFN output buffer cannot be NULL");
     KERNEL_ASSERT(lfn_buf_size > 0, "LFN output buffer size must be > 0");
     KERNEL_ASSERT(lfn_count >= 0, "LFN entry count cannot be negative");

     if (lfn_count == 0) {
         lfn_buf[0] = '\0';
         return;
     }
     KERNEL_ASSERT(lfn_entries != NULL, "LFN entry array cannot be NULL if count > 0");

     lfn_buf[0] = '\0';
     int buf_idx = 0;

     // Iterate through collected entries from highest seq num (index 0) to lowest
     for (int i = 0; i < lfn_count; i++) {
         fat_lfn_entry_t* current_entry = &lfn_entries[i];

         uint16_t name_parts[3][6];
         memcpy(name_parts[0], current_entry->name1, sizeof(current_entry->name1));
         memcpy(name_parts[1], current_entry->name2, sizeof(current_entry->name2));
         memcpy(name_parts[2], current_entry->name3, sizeof(current_entry->name3));

         const size_t counts[] = {5, 6, 2};
         bool sequence_terminated = false;

         for (int p = 0; p < 3 && !sequence_terminated; p++) {
             for (size_t c = 0; c < counts[p]; c++) {
                 uint16_t wide_char = name_parts[p][c];

                 if (wide_char == 0x0000) { sequence_terminated = true; break; }
                 if (wide_char == 0xFFFF) { continue; } // Skip padding

                 if (buf_idx < (int)lfn_buf_size - 1) {
                     char ascii_char = (wide_char < 128 && wide_char > 0) ? (char)wide_char : '?';
                     lfn_buf[buf_idx++] = ascii_char;
                 } else {
                     terminal_printf("[FAT LFN Reconstruct] Warning: LFN buffer full, name truncated.\n");
                     sequence_terminated = true;
                     break;
                 }
             }
         }
         if(sequence_terminated) break;
     }
     lfn_buf[buf_idx] = '\0';
 }


 /**
  * @brief Generates the LFN directory entry structures for a given long filename.
  */
 int fat_generate_lfn_entries(const char* long_name,
                                uint8_t short_name_checksum,
                                fat_lfn_entry_t* lfn_buf,
                                int max_lfn_entries)
 {
     KERNEL_ASSERT(long_name != NULL && lfn_buf != NULL, "Long name and LFN buffer cannot be NULL");
     KERNEL_ASSERT(max_lfn_entries > 0, "Max LFN entries must be positive");

     size_t lfn_len = strlen(long_name);
     // Use FAT_MAX_LFN_CHARS defined in fat_dir.h (now included)
     if (lfn_len == 0 || lfn_len > FAT_MAX_LFN_CHARS) {
         terminal_printf("[FAT LFN Generate] Error: Invalid long name length (%u).\n", (unsigned int)lfn_len);
         return -1; // Use negative error code, e.g., FS_ERR_NAMETOOLONG
     }

     int needed_entries = (int)((lfn_len + 12) / 13);

     if (needed_entries == 0) { return 0; } // Only happens if lfn_len == 0, handled above
     if (needed_entries > max_lfn_entries) {
         terminal_printf("[FAT LFN Generate] Error: Long name '%s' requires %d LFN entries, buffer only holds %d.\n",
                          long_name, needed_entries, max_lfn_entries);
         return -1; // Use negative error code, e.g., FS_ERR_BUFFER_TOO_SMALL
     }

     // Fill entries in reverse order (matching disk layout)
     for (int seq = 1; seq <= needed_entries; seq++) {
         int entry_index = needed_entries - seq;
         fat_lfn_entry_t *entry = &lfn_buf[entry_index];

         memset(entry, 0xFF, sizeof(*entry));
         entry->attr              = FAT_ATTR_LONG_NAME;
         entry->type              = 0;
         entry->checksum          = short_name_checksum;
         entry->first_cluster_zero = 0;

         uint8_t seq_num = (uint8_t)seq;
         if (seq == needed_entries) { seq_num |= FAT_LFN_ENTRY_LAST_FLAG; }
         entry->seq_num = seq_num;

         int name_start_index = (seq - 1) * 13;
         bool name_terminated = false;
         uint16_t current_char;
         int char_index_in_lfn = 0;

         // Part 1: name1 (5 chars)
         for (int i = 0; i < 5; i++, char_index_in_lfn++) {
             int source_idx = name_start_index + char_index_in_lfn;
             if (source_idx < (int)lfn_len) { current_char = (uint16_t)(unsigned char)long_name[source_idx]; }
             else { current_char = name_terminated ? 0xFFFF : 0x0000; name_terminated = true; }
             memcpy(&entry->name1[i], &current_char, sizeof(uint16_t));
         }
         // Part 2: name2 (6 chars)
         for (int i = 0; i < 6; i++, char_index_in_lfn++) {
              int source_idx = name_start_index + char_index_in_lfn;
             if (source_idx < (int)lfn_len) { current_char = (uint16_t)(unsigned char)long_name[source_idx]; }
             else { current_char = name_terminated ? 0xFFFF : 0x0000; name_terminated = true; }
             memcpy(&entry->name2[i], &current_char, sizeof(uint16_t));
         }
         // Part 3: name3 (2 chars)
         for (int i = 0; i < 2; i++, char_index_in_lfn++) {
              int source_idx = name_start_index + char_index_in_lfn;
             if (source_idx < (int)lfn_len) { current_char = (uint16_t)(unsigned char)long_name[source_idx]; }
             else { current_char = name_terminated ? 0xFFFF : 0x0000; name_terminated = true; }
             memcpy(&entry->name3[i], &current_char, sizeof(uint16_t));
         }
     }
     return needed_entries;
 }
