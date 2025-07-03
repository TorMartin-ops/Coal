/**
 * @file page_cache.h
 * @brief Page Cache for File System - Caches file data at page level
 * @author Coal OS Development Team
 * @version 1.0
 * 
 * @details Provides a page-level cache for file data to improve file system
 * performance. Works above the buffer cache layer to cache file contents
 * at PAGE_SIZE granularity.
 */

#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <kernel/core/types.h>
#include <kernel/sync/spinlock.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

// Page cache configuration
#define PAGE_CACHE_HASH_SIZE    256     // Number of hash buckets
#define PAGE_CACHE_MAX_PAGES    1024    // Maximum pages in cache
#define PAGE_CACHE_MIN_FREE     64      // Minimum free pages to maintain

// Page cache flags
#define PAGE_FLAG_VALID         0x01    // Page contains valid data
#define PAGE_FLAG_DIRTY         0x02    // Page has been modified
#define PAGE_FLAG_LOCKED        0x04    // Page is locked for I/O
#define PAGE_FLAG_UPTODATE      0x08    // Page is up to date with disk
#define PAGE_FLAG_ERROR         0x10    // I/O error occurred on this page

// Forward declaration
typedef struct page_cache_entry page_cache_entry_t;

/**
 * @brief Page cache entry structure
 */
struct page_cache_entry {
    // File identification
    uint32_t device_id;         // Device containing the file
    uint32_t inode_number;      // Inode number of the file
    uint32_t page_index;        // Page index within the file (offset / PAGE_SIZE)
    
    // Page data
    void *data;                 // Page data (PAGE_SIZE bytes)
    uint32_t flags;             // Page flags
    
    // Reference counting
    uint32_t ref_count;         // Number of active references
    uint32_t map_count;         // Number of memory mappings
    
    // Linked lists
    page_cache_entry_t *hash_next;  // Next in hash chain
    page_cache_entry_t *hash_prev;  // Previous in hash chain
    page_cache_entry_t *lru_next;   // Next in LRU list
    page_cache_entry_t *lru_prev;   // Previous in LRU list
    
    // Synchronization
    spinlock_t lock;            // Per-page lock
    
    // File system specific data
    void *fs_private;           // File system private data
};

/**
 * @brief Page cache statistics
 */
typedef struct {
    uint32_t total_pages;       // Total pages in cache
    uint32_t dirty_pages;       // Number of dirty pages
    uint32_t locked_pages;      // Number of locked pages
    uint64_t cache_hits;        // Total cache hits
    uint64_t cache_misses;      // Total cache misses
    uint64_t page_faults;       // Page faults handled
    uint64_t write_backs;       // Pages written back
    uint64_t evictions;         // Pages evicted
} page_cache_stats_t;

/**
 * @brief Initialize the page cache system
 */
void page_cache_init(void);

/**
 * @brief Get a page from the cache or allocate a new one
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param page_index Page index within the file
 * @return Page cache entry or NULL on error
 */
page_cache_entry_t* page_cache_get(uint32_t device_id, uint32_t inode_number, 
                                   uint32_t page_index);

/**
 * @brief Find a page in the cache without allocating
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param page_index Page index within the file
 * @return Page cache entry or NULL if not found
 */
page_cache_entry_t* page_cache_find(uint32_t device_id, uint32_t inode_number,
                                    uint32_t page_index);

/**
 * @brief Release a reference to a page
 * @param page Page cache entry to release
 */
void page_cache_put(page_cache_entry_t *page);

/**
 * @brief Mark a page as dirty
 * @param page Page cache entry to mark dirty
 */
void page_cache_mark_dirty(page_cache_entry_t *page);

/**
 * @brief Lock a page for exclusive access
 * @param page Page cache entry to lock
 * @return 0 on success, negative error code on failure
 */
int page_cache_lock(page_cache_entry_t *page);

/**
 * @brief Unlock a page
 * @param page Page cache entry to unlock
 */
void page_cache_unlock(page_cache_entry_t *page);

/**
 * @brief Read data from a file through the page cache
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param offset Offset within the file
 * @param buffer Buffer to read into
 * @param size Number of bytes to read
 * @return Number of bytes read or negative error code
 */
ssize_t page_cache_read(uint32_t device_id, uint32_t inode_number,
                        uint64_t offset, void *buffer, size_t size);

/**
 * @brief Write data to a file through the page cache
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param offset Offset within the file
 * @param buffer Buffer containing data to write
 * @param size Number of bytes to write
 * @return Number of bytes written or negative error code
 */
ssize_t page_cache_write(uint32_t device_id, uint32_t inode_number,
                         uint64_t offset, const void *buffer, size_t size);

/**
 * @brief Write back a single page to disk
 * @param page Page cache entry to write back
 * @return 0 on success, negative error code on failure
 */
int page_cache_writeback_page(page_cache_entry_t *page);

/**
 * @brief Write back all dirty pages for a file
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @return 0 on success, negative error code on failure
 */
int page_cache_sync_file(uint32_t device_id, uint32_t inode_number);

/**
 * @brief Write back all dirty pages in the cache
 * @return Number of pages written back
 */
int page_cache_sync_all(void);

/**
 * @brief Invalidate all pages for a file
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 */
void page_cache_invalidate_file(uint32_t device_id, uint32_t inode_number);

/**
 * @brief Invalidate a range of pages for a file
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param start_offset Start offset in file
 * @param end_offset End offset in file (exclusive)
 */
void page_cache_invalidate_range(uint32_t device_id, uint32_t inode_number,
                                 uint64_t start_offset, uint64_t end_offset);

/**
 * @brief Shrink the page cache by evicting clean pages
 * @param target_pages Target number of pages to free
 * @return Number of pages actually freed
 */
int page_cache_shrink(int target_pages);

/**
 * @brief Get page cache statistics
 * @param stats Structure to fill with statistics
 */
void page_cache_get_stats(page_cache_stats_t *stats);

/**
 * @brief Prefetch pages into the cache
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param start_index Starting page index
 * @param count Number of pages to prefetch
 */
void page_cache_prefetch(uint32_t device_id, uint32_t inode_number,
                         uint32_t start_index, uint32_t count);

#endif // PAGE_CACHE_H