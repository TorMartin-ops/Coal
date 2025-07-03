/**
 * @file page_cache.c
 * @brief Page Cache Implementation for File System
 * @author Coal OS Development Team
 * @version 1.0
 * 
 * @details Implements a page-level cache for file data to improve file system
 * performance. Works with the VFS layer to cache file contents at PAGE_SIZE
 * granularity.
 */

#include <kernel/fs/vfs/page_cache.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/process/scheduler.h>  // For yield()
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Page Cache Configuration and Types
//============================================================================

// Hash table for page lookup
static page_cache_entry_t *page_hash_table[PAGE_CACHE_HASH_SIZE];

// LRU list for page replacement
static page_cache_entry_t *lru_head = NULL;  // Most recently used
static page_cache_entry_t *lru_tail = NULL;  // Least recently used

// Global cache lock
static spinlock_t cache_lock;

// Statistics
static page_cache_stats_t cache_stats;

// Current number of pages in cache
static uint32_t current_pages = 0;

// Logging macros
#define PAGE_CACHE_ERROR(fmt, ...) serial_printf("[PageCache ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define PAGE_CACHE_WARN(fmt, ...)  serial_printf("[PageCache WARN ] " fmt "\n", ##__VA_ARGS__)
#define PAGE_CACHE_DEBUG(fmt, ...) serial_printf("[PageCache DEBUG] " fmt "\n", ##__VA_ARGS__)
#define PAGE_CACHE_INFO(fmt, ...)  serial_printf("[PageCache INFO ] " fmt "\n", ##__VA_ARGS__)

//============================================================================
// Hash Functions
//============================================================================

/**
 * @brief Compute hash index for page lookup
 */
static uint32_t page_hash(uint32_t device_id, uint32_t inode_number, uint32_t page_index) {
    // Simple hash combining device, inode, and page index
    uint32_t hash = device_id;
    hash = ((hash << 5) + hash) + inode_number;
    hash = ((hash << 5) + hash) + page_index;
    return hash % PAGE_CACHE_HASH_SIZE;
}

//============================================================================
// LRU List Management
//============================================================================

/**
 * @brief Remove page from LRU list
 * @note Assumes cache_lock is held
 */
static void lru_remove(page_cache_entry_t *page) {
    if (!page) return;
    
    if (page->lru_prev) {
        page->lru_prev->lru_next = page->lru_next;
    } else {
        lru_head = page->lru_next;
    }
    
    if (page->lru_next) {
        page->lru_next->lru_prev = page->lru_prev;
    } else {
        lru_tail = page->lru_prev;
    }
    
    page->lru_prev = page->lru_next = NULL;
}

/**
 * @brief Add page to front of LRU list (most recently used)
 * @note Assumes cache_lock is held
 */
static void lru_add_front(page_cache_entry_t *page) {
    if (!page) return;
    
    page->lru_prev = NULL;
    page->lru_next = lru_head;
    
    if (lru_head) {
        lru_head->lru_prev = page;
    }
    lru_head = page;
    
    if (!lru_tail) {
        lru_tail = page;
    }
}

/**
 * @brief Move page to front of LRU list
 * @note Assumes cache_lock is held
 */
static void lru_touch(page_cache_entry_t *page) {
    if (!page || page == lru_head) return;
    
    lru_remove(page);
    lru_add_front(page);
}

//============================================================================
// Hash Table Management
//============================================================================

/**
 * @brief Insert page into hash table
 * @note Assumes cache_lock is held
 */
static void hash_insert(page_cache_entry_t *page) {
    if (!page) return;
    
    uint32_t index = page_hash(page->device_id, page->inode_number, page->page_index);
    
    page->hash_prev = NULL;
    page->hash_next = page_hash_table[index];
    
    if (page_hash_table[index]) {
        page_hash_table[index]->hash_prev = page;
    }
    
    page_hash_table[index] = page;
}

/**
 * @brief Remove page from hash table
 * @note Assumes cache_lock is held
 */
static void hash_remove(page_cache_entry_t *page) {
    if (!page) return;
    
    uint32_t index = page_hash(page->device_id, page->inode_number, page->page_index);
    
    if (page->hash_prev) {
        page->hash_prev->hash_next = page->hash_next;
    } else {
        page_hash_table[index] = page->hash_next;
    }
    
    if (page->hash_next) {
        page->hash_next->hash_prev = page->hash_prev;
    }
    
    page->hash_prev = page->hash_next = NULL;
}

/**
 * @brief Find page in hash table
 * @note Assumes cache_lock is held
 */
static page_cache_entry_t* hash_find(uint32_t device_id, uint32_t inode_number, 
                                    uint32_t page_index) {
    uint32_t index = page_hash(device_id, inode_number, page_index);
    page_cache_entry_t *page = page_hash_table[index];
    
    while (page) {
        if (page->device_id == device_id && 
            page->inode_number == inode_number &&
            page->page_index == page_index) {
            return page;
        }
        page = page->hash_next;
    }
    
    return NULL;
}

//============================================================================
// Page I/O Functions
//============================================================================

/**
 * @brief Read page data from disk
 * @note Page must be locked
 */
static int page_read_from_disk(page_cache_entry_t *page) {
    if (!page || !page->data) return -FS_ERR_INVALID_PARAM;
    
    // Calculate file offset
    uint64_t offset = (uint64_t)page->page_index * PAGE_SIZE;
    
    // Read from VFS
    ssize_t result = vfs_read_at(page->device_id, page->inode_number, 
                                 offset, page->data, PAGE_SIZE);
    
    if (result < 0) {
        page->flags |= PAGE_FLAG_ERROR;
        return result;
    }
    
    // Zero fill if we read less than a page
    if (result < PAGE_SIZE) {
        memset((uint8_t*)page->data + result, 0, PAGE_SIZE - result);
    }
    
    page->flags |= (PAGE_FLAG_VALID | PAGE_FLAG_UPTODATE);
    page->flags &= ~PAGE_FLAG_ERROR;
    
    return 0;
}

/**
 * @brief Write page data to disk
 * @note Page must be locked
 */
static int page_write_to_disk(page_cache_entry_t *page) {
    if (!page || !page->data) return -FS_ERR_INVALID_PARAM;
    
    if (!(page->flags & PAGE_FLAG_DIRTY)) {
        return 0; // Nothing to write
    }
    
    // Calculate file offset
    uint64_t offset = (uint64_t)page->page_index * PAGE_SIZE;
    
    // Write to VFS
    ssize_t result = vfs_write_at(page->device_id, page->inode_number,
                                  offset, page->data, PAGE_SIZE);
    
    if (result < 0) {
        page->flags |= PAGE_FLAG_ERROR;
        return result;
    }
    
    page->flags &= ~PAGE_FLAG_DIRTY;
    cache_stats.write_backs++;
    
    return 0;
}

//============================================================================
// Page Allocation and Eviction
//============================================================================

/**
 * @brief Allocate a new page cache entry
 */
static page_cache_entry_t* page_alloc(void) {
    page_cache_entry_t *page = kmalloc(sizeof(page_cache_entry_t));
    if (!page) return NULL;
    
    memset(page, 0, sizeof(page_cache_entry_t));
    
    page->data = kmalloc(PAGE_SIZE);
    if (!page->data) {
        kfree(page);
        return NULL;
    }
    
    spinlock_init(&page->lock);
    return page;
}

/**
 * @brief Free a page cache entry
 */
static void page_free(page_cache_entry_t *page) {
    if (!page) return;
    
    if (page->data) {
        kfree(page->data);
    }
    kfree(page);
}

/**
 * @brief Try to evict a page from the cache
 * @note Assumes cache_lock is held by caller
 * @param irq_flags Current interrupt flags from cache lock
 * @return 0 on success, negative error code on failure
 */
static int try_evict_page(uintptr_t *irq_flags) {
    page_cache_entry_t *victim = lru_tail;
    
    while (victim) {
        // Skip pages that are in use or locked
        if (victim->ref_count == 0 && !(victim->flags & PAGE_FLAG_LOCKED)) {
            // Try to lock the page
            uintptr_t page_flags = spinlock_acquire_irqsave(&victim->lock);
            
            // Double-check conditions
            if (victim->ref_count == 0 && !(victim->flags & PAGE_FLAG_LOCKED)) {
                // Write back if dirty
                if (victim->flags & PAGE_FLAG_DIRTY) {
                    victim->flags |= PAGE_FLAG_LOCKED;
                    spinlock_release_irqrestore(&victim->lock, page_flags);
                    
                    // Release cache lock for I/O
                    spinlock_release_irqrestore(&cache_lock, *irq_flags);
                    int result = page_write_to_disk(victim);
                    *irq_flags = spinlock_acquire_irqsave(&cache_lock);
                    
                    // Re-acquire page lock
                    page_flags = spinlock_acquire_irqsave(&victim->lock);
                    victim->flags &= ~PAGE_FLAG_LOCKED;
                    
                    if (result < 0) {
                        spinlock_release_irqrestore(&victim->lock, page_flags);
                        victim = victim->lru_prev;
                        continue;
                    }
                }
                
                spinlock_release_irqrestore(&victim->lock, page_flags);
                
                // Remove from cache
                hash_remove(victim);
                lru_remove(victim);
                current_pages--;
                cache_stats.evictions++;
                
                // Free the page
                page_free(victim);
                
                return 0;
            }
            
            spinlock_release_irqrestore(&victim->lock, page_flags);
        }
        
        victim = victim->lru_prev;
    }
    
    return -FS_ERR_NO_RESOURCES;
}

//============================================================================
// Public API Implementation
//============================================================================

void page_cache_init(void) {
    // Initialize lock
    spinlock_init(&cache_lock);
    
    // Clear hash table
    memset(page_hash_table, 0, sizeof(page_hash_table));
    
    // Clear statistics
    memset(&cache_stats, 0, sizeof(cache_stats));
    
    // Initialize LRU list
    lru_head = lru_tail = NULL;
    current_pages = 0;
    
    PAGE_CACHE_INFO("Page cache initialized with %u hash buckets", PAGE_CACHE_HASH_SIZE);
}

page_cache_entry_t* page_cache_get(uint32_t device_id, uint32_t inode_number, 
                                   uint32_t page_index) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    // Look for existing page
    page_cache_entry_t *page = hash_find(device_id, inode_number, page_index);
    
    if (page) {
        // Cache hit
        page->ref_count++;
        lru_touch(page);
        cache_stats.cache_hits++;
        spinlock_release_irqrestore(&cache_lock, irq_flags);
        return page;
    }
    
    // Cache miss
    cache_stats.cache_misses++;
    
    // Check if we need to evict
    if (current_pages >= PAGE_CACHE_MAX_PAGES) {
        if (try_evict_page(&irq_flags) < 0) {
            spinlock_release_irqrestore(&cache_lock, irq_flags);
            PAGE_CACHE_ERROR("Failed to evict page, cache full");
            return NULL;
        }
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    // Allocate new page
    page = page_alloc();
    if (!page) {
        PAGE_CACHE_ERROR("Failed to allocate page");
        return NULL;
    }
    
    // Initialize page
    page->device_id = device_id;
    page->inode_number = inode_number;
    page->page_index = page_index;
    page->ref_count = 1;
    page->flags = 0;
    
    // Re-acquire lock and insert
    irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    // Check again if page was added while we were allocating
    page_cache_entry_t *existing = hash_find(device_id, inode_number, page_index);
    if (existing) {
        // Someone else added it, use theirs
        existing->ref_count++;
        lru_touch(existing);
        spinlock_release_irqrestore(&cache_lock, irq_flags);
        page_free(page);
        return existing;
    }
    
    // Insert into cache
    hash_insert(page);
    lru_add_front(page);
    current_pages++;
    cache_stats.total_pages = current_pages;
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    return page;
}

page_cache_entry_t* page_cache_find(uint32_t device_id, uint32_t inode_number,
                                    uint32_t page_index) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    page_cache_entry_t *page = hash_find(device_id, inode_number, page_index);
    if (page) {
        page->ref_count++;
        lru_touch(page);
        cache_stats.cache_hits++;
    } else {
        cache_stats.cache_misses++;
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    return page;
}

void page_cache_put(page_cache_entry_t *page) {
    if (!page) return;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    if (page->ref_count > 0) {
        page->ref_count--;
    } else {
        PAGE_CACHE_WARN("Releasing page with ref_count=0");
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
}

void page_cache_mark_dirty(page_cache_entry_t *page) {
    if (!page) return;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&page->lock);
    
    if (page->flags & PAGE_FLAG_VALID) {
        page->flags |= PAGE_FLAG_DIRTY;
        
        // Update dirty page count
        uintptr_t cache_flags = spinlock_acquire_irqsave(&cache_lock);
        cache_stats.dirty_pages++;
        spinlock_release_irqrestore(&cache_lock, cache_flags);
    }
    
    spinlock_release_irqrestore(&page->lock, irq_flags);
}

int page_cache_lock(page_cache_entry_t *page) {
    if (!page) return -FS_ERR_INVALID_PARAM;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&page->lock);
    
    while (page->flags & PAGE_FLAG_LOCKED) {
        spinlock_release_irqrestore(&page->lock, irq_flags);
        // TODO: Implement proper wait queue
        yield(); // Simple yield for now
        irq_flags = spinlock_acquire_irqsave(&page->lock);
    }
    
    page->flags |= PAGE_FLAG_LOCKED;
    
    // Update locked page count
    uintptr_t cache_flags = spinlock_acquire_irqsave(&cache_lock);
    cache_stats.locked_pages++;
    spinlock_release_irqrestore(&cache_lock, cache_flags);
    
    spinlock_release_irqrestore(&page->lock, irq_flags);
    
    return 0;
}

void page_cache_unlock(page_cache_entry_t *page) {
    if (!page) return;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&page->lock);
    
    if (page->flags & PAGE_FLAG_LOCKED) {
        page->flags &= ~PAGE_FLAG_LOCKED;
        
        // Update locked page count
        uintptr_t cache_flags = spinlock_acquire_irqsave(&cache_lock);
        if (cache_stats.locked_pages > 0) {
            cache_stats.locked_pages--;
        }
        spinlock_release_irqrestore(&cache_lock, cache_flags);
    }
    
    spinlock_release_irqrestore(&page->lock, irq_flags);
}

ssize_t page_cache_read(uint32_t device_id, uint32_t inode_number,
                        uint64_t offset, void *buffer, size_t size) {
    if (!buffer || size == 0) return -FS_ERR_INVALID_PARAM;
    
    ssize_t total_read = 0;
    
    while (size > 0) {
        // Calculate page index and offset within page
        uint32_t page_index = offset / PAGE_SIZE;
        uint32_t page_offset = offset % PAGE_SIZE;
        size_t to_read = PAGE_SIZE - page_offset;
        if (to_read > size) to_read = size;
        
        // Get the page
        page_cache_entry_t *page = page_cache_get(device_id, inode_number, page_index);
        if (!page) {
            if (total_read > 0) return total_read;
            return -FS_ERR_NO_RESOURCES;
        }
        
        // Lock the page
        int result = page_cache_lock(page);
        if (result < 0) {
            page_cache_put(page);
            if (total_read > 0) return total_read;
            return result;
        }
        
        // Read from disk if necessary
        if (!(page->flags & PAGE_FLAG_UPTODATE)) {
            result = page_read_from_disk(page);
            if (result < 0) {
                page_cache_unlock(page);
                page_cache_put(page);
                if (total_read > 0) return total_read;
                return result;
            }
        }
        
        // Copy data to user buffer
        memcpy((uint8_t*)buffer + total_read, (uint8_t*)page->data + page_offset, to_read);
        
        // Unlock and release page
        page_cache_unlock(page);
        page_cache_put(page);
        
        // Update counters
        total_read += to_read;
        offset += to_read;
        size -= to_read;
    }
    
    return total_read;
}

ssize_t page_cache_write(uint32_t device_id, uint32_t inode_number,
                         uint64_t offset, const void *buffer, size_t size) {
    if (!buffer || size == 0) return -FS_ERR_INVALID_PARAM;
    
    ssize_t total_written = 0;
    
    while (size > 0) {
        // Calculate page index and offset within page
        uint32_t page_index = offset / PAGE_SIZE;
        uint32_t page_offset = offset % PAGE_SIZE;
        size_t to_write = PAGE_SIZE - page_offset;
        if (to_write > size) to_write = size;
        
        // Get the page
        page_cache_entry_t *page = page_cache_get(device_id, inode_number, page_index);
        if (!page) {
            if (total_written > 0) return total_written;
            return -FS_ERR_NO_RESOURCES;
        }
        
        // Lock the page
        int result = page_cache_lock(page);
        if (result < 0) {
            page_cache_put(page);
            if (total_written > 0) return total_written;
            return result;
        }
        
        // Read from disk if partial page write
        if (page_offset != 0 || to_write != PAGE_SIZE) {
            if (!(page->flags & PAGE_FLAG_UPTODATE)) {
                result = page_read_from_disk(page);
                if (result < 0) {
                    page_cache_unlock(page);
                    page_cache_put(page);
                    if (total_written > 0) return total_written;
                    return result;
                }
            }
        }
        
        // Copy data from user buffer
        memcpy((uint8_t*)page->data + page_offset, (uint8_t*)buffer + total_written, to_write);
        
        // Mark page as dirty and up to date
        page->flags |= (PAGE_FLAG_DIRTY | PAGE_FLAG_UPTODATE | PAGE_FLAG_VALID);
        
        // Unlock and release page
        page_cache_unlock(page);
        page_cache_put(page);
        
        // Update counters
        total_written += to_write;
        offset += to_write;
        size -= to_write;
    }
    
    return total_written;
}

int page_cache_writeback_page(page_cache_entry_t *page) {
    if (!page) return -FS_ERR_INVALID_PARAM;
    
    int result = page_cache_lock(page);
    if (result < 0) return result;
    
    result = page_write_to_disk(page);
    
    page_cache_unlock(page);
    return result;
}

int page_cache_sync_file(uint32_t device_id, uint32_t inode_number) {
    int pages_written = 0;
    int errors = 0;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    // Collect all dirty pages for this file
    for (uint32_t i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t *page = page_hash_table[i];
        while (page) {
            if (page->device_id == device_id && 
                page->inode_number == inode_number &&
                (page->flags & PAGE_FLAG_DIRTY)) {
                
                // Increase ref count to prevent eviction
                page->ref_count++;
                spinlock_release_irqrestore(&cache_lock, irq_flags);
                
                // Write back the page
                int result = page_cache_writeback_page(page);
                if (result < 0) {
                    errors++;
                } else {
                    pages_written++;
                }
                
                // Release the page
                page_cache_put(page);
                
                // Re-acquire lock
                irq_flags = spinlock_acquire_irqsave(&cache_lock);
            }
            page = page->hash_next;
        }
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    if (errors > 0) {
        PAGE_CACHE_ERROR("Failed to sync %d pages for file", errors);
        return -FS_ERR_IO;
    }
    
    return pages_written;
}

int page_cache_sync_all(void) {
    int pages_written = 0;
    int errors = 0;
    
    PAGE_CACHE_INFO("Starting full page cache sync...");
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    // Collect all dirty pages
    for (uint32_t i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t *page = page_hash_table[i];
        while (page) {
            if (page->flags & PAGE_FLAG_DIRTY) {
                // Increase ref count to prevent eviction
                page->ref_count++;
                spinlock_release_irqrestore(&cache_lock, irq_flags);
                
                // Write back the page
                int result = page_cache_writeback_page(page);
                if (result < 0) {
                    errors++;
                } else {
                    pages_written++;
                }
                
                // Release the page
                page_cache_put(page);
                
                // Re-acquire lock
                irq_flags = spinlock_acquire_irqsave(&cache_lock);
            }
            page = page->hash_next;
        }
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    PAGE_CACHE_INFO("Sync complete: %d pages written, %d errors", pages_written, errors);
    
    return pages_written;
}

void page_cache_invalidate_file(uint32_t device_id, uint32_t inode_number) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    int invalidated = 0;
    
    for (uint32_t i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t *page = page_hash_table[i];
        page_cache_entry_t *next;
        
        while (page) {
            next = page->hash_next;
            
            if (page->device_id == device_id && page->inode_number == inode_number) {
                if (page->ref_count == 0) {
                    // Remove from cache
                    hash_remove(page);
                    lru_remove(page);
                    current_pages--;
                    
                    // Free the page
                    page_free(page);
                    invalidated++;
                }
            }
            
            page = next;
        }
    }
    
    cache_stats.total_pages = current_pages;
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    if (invalidated > 0) {
        PAGE_CACHE_DEBUG("Invalidated %d pages for file %u:%u", invalidated, device_id, inode_number);
    }
}

void page_cache_invalidate_range(uint32_t device_id, uint32_t inode_number,
                                 uint64_t start_offset, uint64_t end_offset) {
    uint32_t start_page = start_offset / PAGE_SIZE;
    uint32_t end_page = (end_offset + PAGE_SIZE - 1) / PAGE_SIZE;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    int invalidated = 0;
    
    for (uint32_t i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t *page = page_hash_table[i];
        page_cache_entry_t *next;
        
        while (page) {
            next = page->hash_next;
            
            if (page->device_id == device_id && 
                page->inode_number == inode_number &&
                page->page_index >= start_page &&
                page->page_index < end_page) {
                
                if (page->ref_count == 0) {
                    // Remove from cache
                    hash_remove(page);
                    lru_remove(page);
                    current_pages--;
                    
                    // Free the page
                    page_free(page);
                    invalidated++;
                }
            }
            
            page = next;
        }
    }
    
    cache_stats.total_pages = current_pages;
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    if (invalidated > 0) {
        PAGE_CACHE_DEBUG("Invalidated %d pages in range %llu-%llu for file %u:%u", 
                         invalidated, start_offset, end_offset, device_id, inode_number);
    }
}

int page_cache_shrink(int target_pages) {
    int freed = 0;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    while (freed < target_pages && lru_tail) {
        if (try_evict_page(&irq_flags) == 0) {
            freed++;
        } else {
            break;
        }
    }
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
    
    if (freed > 0) {
        PAGE_CACHE_DEBUG("Shrunk cache by %d pages", freed);
    }
    
    return freed;
}

void page_cache_get_stats(page_cache_stats_t *stats) {
    if (!stats) return;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&cache_lock);
    
    memcpy(stats, &cache_stats, sizeof(page_cache_stats_t));
    stats->total_pages = current_pages;
    
    spinlock_release_irqrestore(&cache_lock, irq_flags);
}

void page_cache_prefetch(uint32_t device_id, uint32_t inode_number,
                         uint32_t start_index, uint32_t count) {
    // Simple prefetch implementation
    for (uint32_t i = 0; i < count; i++) {
        page_cache_entry_t *page = page_cache_get(device_id, inode_number, start_index + i);
        if (page) {
            // Lock and read if not already loaded
            if (page_cache_lock(page) == 0) {
                if (!(page->flags & PAGE_FLAG_UPTODATE)) {
                    page_read_from_disk(page);
                }
                page_cache_unlock(page);
            }
            page_cache_put(page);
        }
    }
}