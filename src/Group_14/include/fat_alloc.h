/**
 * @file fat_alloc.h
 * @brief Cluster allocation and management for FAT filesystem
 *
 * Provides functions to find, allocate, and free cluster chains within
 * the FAT filesystem. These functions directly manipulate the in-memory
 * FAT table and rely on external locking of the fat_fs_t structure.
 */

 #ifndef FAT_ALLOC_H
 #define FAT_ALLOC_H
 
 #include "fat_core.h" // For fat_fs_t definition
 #include "fs_errno.h" // For error codes
 
 /**
  * @brief Allocates a new cluster and optionally links it to a previous one.
  *
  * Finds the first available free cluster in the FAT table, marks it as the
  * end-of-chain (EOC), and optionally updates the entry for the
  * 'previous_cluster' to point to the newly allocated one.
  *
  * @param fs Pointer to the FAT filesystem structure. Assumed locked by caller.
  * @param previous_cluster The cluster number to link from (use 0 if allocating
  * the first cluster in a chain).
  * @return The cluster number of the newly allocated cluster (>= 2),
  * or 0 if no free clusters are available or an error occurred.
  */
 uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster);
 
 /**
  * @brief Frees an entire cluster chain starting from a given cluster.
  *
  * Iterates through the FAT table starting from 'start_cluster', marking each
  * cluster in the chain as free (0) until the end-of-chain marker is reached.
  *
  * @param fs Pointer to the FAT filesystem structure. Assumed locked by caller.
  * @param start_cluster The first cluster in the chain to free (must be >= 2).
  * @return FS_SUCCESS (0) on success, or a negative FS_ERR_* code on failure
  * (e.g., invalid start cluster, error reading/writing FAT). Note that
  * it attempts to free as much as possible even if errors occur mid-chain.
  */
 int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster);
 
 #endif /* FAT_ALLOC_H */