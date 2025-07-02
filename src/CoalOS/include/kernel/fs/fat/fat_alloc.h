// --- Add to fat_alloc.h ---

#ifndef FAT_ALLOC_H
#define FAT_ALLOC_H

#include <kernel/fs/fat/fat_fs.h>     // For fat_fs_t, fat_dir_entry_t
#include <kernel/core/types.h>      // For uint32_t etc.
#include <kernel/fs/vfs/fs_errno.h>   // For FS_SUCCESS etc.

// Allocates a new cluster and optionally links it from a previous one.
uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster);

// Frees an entire cluster chain starting from a given cluster.
int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster);

// Creates a new file entry (including LFN and 8.3) in a parent directory.
// Assumes parent directory exists and path resolves correctly up to the last component.
// Does NOT allocate data clusters for the file itself (creates a 0-byte file).
int fat_create_file(fat_fs_t *fs,
                    const char *path,          // Full path to the new file
                    uint8_t attributes,       // Attributes for the new file (e.g., FAT_ATTR_ARCHIVE)
                    fat_dir_entry_t *entry_out, // Output: Populated 8.3 entry for the new file
                    uint32_t *dir_cluster_out, // Output: Cluster where the new entry was written
                    uint32_t *dir_offset_out); // Output: Byte offset of the 8.3 entry within its dir

// Truncates a file to zero length. Frees associated cluster chain.
// Updates the directory entry on disk.
int fat_truncate_file(fat_fs_t *fs,
                      fat_dir_entry_t *entry, // The 8.3 entry struct (will be modified in memory)
                      uint32_t entry_dir_cluster, // Cluster containing this entry
                      uint32_t entry_offset_in_dir); // Byte offset of this 8.3 entry

#endif // FAT_ALLOC_H