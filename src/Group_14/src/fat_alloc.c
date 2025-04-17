/**
 * @file fat_alloc.c
 * @brief Cluster allocation and management implementation for FAT filesystem
 */

 #include "fat_alloc.h"
 #include "fat_core.h"
 #include "fat_utils.h"  // For fat_get_next_cluster, fat_set_cluster_entry
 #include "terminal.h"   // For potential debug logging
 #include "fs_errno.h"
 #include "libc/stdbool.h"
 #include "libc/stdint.h"
 #include "assert.h"     // For KERNEL_ASSERT
 
 /**
  * @brief Finds the first available free cluster in the FAT table.
  * @param fs Pointer to the FAT filesystem structure. Caller must hold lock.
  * @return The cluster number of the first free cluster (>= 2), or 0 if none found.
  */
 static uint32_t find_free_cluster(fat_fs_t *fs)
 {
     // Assertion: Ensure FS and FAT table pointers are valid, handled by caller
     // Assertion: Assumes caller holds fs->lock
 
     // Clusters 0 and 1 are reserved. Start searching from cluster 2.
     uint32_t first_search_cluster = 2;
     // Total number of clusters available for data (0..cluster_count-1 map to 2..cluster_count+1)
     uint32_t last_search_cluster = fs->cluster_count + 1; // Exclusive end
 
     // TODO: Optimization: Add 'fs->first_free_hint' to start search from, update on free/alloc.
     // For now, simple linear scan.
 
     for (uint32_t i = first_search_cluster; i <= last_search_cluster; i++) {
         uint32_t entry_value;
         // Directly reading from the cached FAT table is efficient here.
         // fat_get_next_cluster encapsulates the FAT type logic.
         // We only care if the result indicates it's free (entry value == 0).
         if (fat_get_cluster_entry(fs, i, &entry_value) != FS_SUCCESS) {
              // This indicates an issue reading the FAT itself (e.g., bad index), severe error.
              terminal_printf("[FAT find_free] Error: Failed to read FAT entry for cluster %u\n", i);
              return 0; // Indicate failure
         }
 
         if (entry_value == 0) {
             // Found a free cluster
             return i;
         }
     }
 
     // No free clusters found
     terminal_printf("[FAT find_free] Warning: No free clusters found on device.\n");
     return 0;
 }
 
 
 /**
  * @brief Allocates a new cluster and optionally links it to a previous one.
  */
 uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster)
 {
     KERNEL_ASSERT(fs != NULL); // Non-recoverable if fs is NULL
     // Assertion: Assumes caller holds fs->lock
 
     if (!fs->fat_table) {
         terminal_printf("[FAT alloc] Error: FAT table not loaded.\n");
         return 0; // Cannot allocate without FAT table
     }
 
     // 1. Find a free cluster
     uint32_t free_cluster = find_free_cluster(fs);
     if (free_cluster < 2) { // find_free_cluster returns 0 on error or no space
         return 0; // No space or error finding free cluster
     }
 
     // 2. Mark the newly found cluster as End-Of-Chain (EOC)
     int set_eoc_res = fat_set_cluster_entry(fs, free_cluster, fs->eoc_marker);
     if (set_eoc_res != FS_SUCCESS) {
         terminal_printf("[FAT alloc] Error: Failed to mark cluster %u as EOC (err %d)\n", free_cluster, set_eoc_res);
         // Failed to update FAT, cannot proceed safely
         return 0;
     }
 
     // 3. Link from the previous cluster if one was provided
     if (previous_cluster >= 2) {
         int link_res = fat_set_cluster_entry(fs, previous_cluster, free_cluster);
         if (link_res != FS_SUCCESS) {
             terminal_printf("[FAT alloc] Error: Failed to link cluster %u -> %u (err %d)\n", previous_cluster, free_cluster, link_res);
             // Attempt to revert the EOC mark on the free cluster to avoid leaving it allocated but unlinked
             fat_set_cluster_entry(fs, free_cluster, 0); // Mark back as free (best effort)
             return 0; // Return error
         }
     }
 
     // Success
     // TODO: Update FSINFO free cluster count if implemented
     // TODO: Update fs->first_free_hint if implemented
 
     return free_cluster;
 }
 
 
 /**
  * @brief Frees an entire cluster chain starting from a given cluster.
  */
 int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster)
 {
     KERNEL_ASSERT(fs != NULL); // Non-recoverable if fs is NULL
 
     if (start_cluster < 2) {
         terminal_printf("[FAT free] Error: Cannot free reserved cluster %u\n", start_cluster);
         return -FS_ERR_INVALID_PARAM;
     }
      if (!fs->fat_table) {
         terminal_printf("[FAT free] Error: FAT table not loaded.\n");
         return -FS_ERR_IO; // Cannot free without FAT table
     }
 
     // Assertion: Assumes caller holds fs->lock
 
     uint32_t current_cluster = start_cluster;
     int result = FS_SUCCESS; // Track if any errors occur
 
     while (current_cluster >= 2 && current_cluster < fs->eoc_marker) {
         uint32_t next_cluster = 0; // Default to 0 in case of error
 
         // Get the *next* cluster in the chain *before* freeing the current one
         int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         if (get_next_res != FS_SUCCESS) {
              terminal_printf("[FAT free] Warning: Error reading FAT entry for cluster %u (err %d). Stopping chain free.\n", current_cluster, get_next_res);
              result = get_next_res; // Record the error
              // Decide whether to stop or try freeing the current cluster anyway.
              // Let's attempt to free the current cluster still, but stop the chain traversal.
              next_cluster = fs->eoc_marker; // Force loop exit after this iteration
         }
 
         // Mark the *current* cluster as free (0)
         int set_free_res = fat_set_cluster_entry(fs, current_cluster, 0);
         if (set_free_res != FS_SUCCESS) {
              terminal_printf("[FAT free] Warning: Error writing FAT entry for cluster %u (err %d).\n", current_cluster, set_free_res);
              if (result == FS_SUCCESS) {
                  result = set_free_res; // Record the error if we haven't seen one yet
              }
              // Continue to the next cluster if we could read it, despite the write error here.
         }
 
         // TODO: Update FSINFO free cluster count if implemented
         // TODO: Update fs->first_free_hint if implemented (set hint to 'current_cluster' if it's lower?)
 
         current_cluster = next_cluster; // Move to the next link in the chain
 
          // Safety break for potential infinite loops in corrupt FATs
          if (current_cluster == 0 || current_cluster == 1) {
              terminal_printf("[FAT free] Error: Corrupt FAT chain detected (link to %u).\n", current_cluster);
              if (result == FS_SUCCESS) result = -FS_ERR_CORRUPT;
              break;
          }
     }
 
     return result; // Return success only if no errors occurred
 }