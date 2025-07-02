/**
 * buffer_cache.c - High-performance disk buffer caching system
 *
 * This implementation provides a robust buffer caching system for block devices
 * with comprehensive error handling, proper synchronization primitives, and
 * optimal memory management to prevent buffer overflows and memory corruption.
 */

 #include <kernel/drivers/storage/buffer_cache.h>
 #include <kernel/memory/kmalloc.h>
 #include <kernel/drivers/display/terminal.h>
 #include <kernel/drivers/storage/disk.h>
 #include <kernel/fs/vfs/fs_errno.h>
 #include <kernel/sync/spinlock.h>
 #include <kernel/lib/string.h>
 #include <kernel/core/types.h>
 
 // Configuration
 #define BUFFER_CACHE_HASH_SIZE     256     // Power of 2 recommended for hash distribution
 #define DEFAULT_BUFFER_BLOCK_SIZE  512     // Standard sector size
 #define MAX_BUFFER_BLOCK_SIZE      8192    // Maximum allowed buffer size
 #define MIN_BUFFER_BLOCK_SIZE      128     // Minimum allowed buffer size
 #define BUFFER_PADDING             16      // Safety padding for buffers
 #define MAX_SECTORS_PER_IO         128     // Maximum sectors in a single I/O operation
 
 // Cache statistics (optional)
 static struct {
     uint32_t hits;          // Cache hits
     uint32_t misses;        // Cache misses
     uint32_t reads;         // Disk reads performed
     uint32_t writes;        // Disk writes performed
     uint32_t evictions;     // Number of buffers evicted
     uint32_t alloc_failures;// Memory allocation failures
     uint32_t io_errors;     // I/O errors encountered
 } cache_stats;
 
 // Lock for the entire buffer cache
 static spinlock_t cache_lock;
 
 // Hash table of buffer pointers
 static buffer_t *buffer_hash_table[BUFFER_CACHE_HASH_SIZE];
 
 // LRU list for buffer replacement
 static buffer_t *lru_head = NULL;  // Most recently used
 static buffer_t *lru_tail = NULL;  // Least recently used
 
 // Device registry - simple implementation
 #define MAX_REGISTERED_DISKS 8
 static struct {
     disk_t *disks[MAX_REGISTERED_DISKS];
     int count;
     spinlock_t lock;
 } disk_registry;
 
 /**
  * Compute a high-quality hash for buffer lookup
  */
 static uint32_t buffer_hash(const char *device_name, uint32_t block_number) {
     if (!device_name) return block_number % BUFFER_CACHE_HASH_SIZE;
 
     // FNV-1a hash algorithm
     uint32_t hash = 2166136261u; // FNV offset basis
 
     // Hash the device name
     for (const char *p = device_name; *p; p++) {
         hash ^= (uint8_t)(*p);
         hash *= 16777619u; // FNV prime
     }
 
     // Mix in the block number
     hash ^= (block_number & 0xFF);
     hash *= 16777619u;
     hash ^= ((block_number >> 8) & 0xFF);
     hash *= 16777619u;
     hash ^= ((block_number >> 16) & 0xFF);
     hash *= 16777619u;
     hash ^= ((block_number >> 24) & 0xFF);
 
     return hash % BUFFER_CACHE_HASH_SIZE;
 }
 
 /**
  * Register a disk with the buffer cache system
  */
 int buffer_register_disk(disk_t *disk) {
     if (!disk || !disk->initialized || !disk->blk_dev.device_name) {
         return -FS_ERR_INVALID_PARAM;
     }
 
     // Validate sector size
     if (disk->blk_dev.sector_size < MIN_BUFFER_BLOCK_SIZE ||
         disk->blk_dev.sector_size > MAX_BUFFER_BLOCK_SIZE) {
         terminal_printf("[BufferCache] Invalid sector size %u for device '%s'.\n",
                        disk->blk_dev.sector_size, disk->blk_dev.device_name);
         return -FS_ERR_INVALID_PARAM;
     }
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&disk_registry.lock);
 
     // Check if disk is already registered
     for (int i = 0; i < disk_registry.count; i++) {
         if (disk_registry.disks[i] == disk) {
             spinlock_release_irqrestore(&disk_registry.lock, irq_state);
             return 0; // Already registered
         }
     }
 
     // Check if registry is full
     if (disk_registry.count >= MAX_REGISTERED_DISKS) {
         terminal_printf("[BufferCache] Cannot register disk '%s': registry full.\n",
                         disk->blk_dev.device_name);
         spinlock_release_irqrestore(&disk_registry.lock, irq_state);
         return -FS_ERR_NO_RESOURCES;
     }
 
     // Add to registry
     disk_registry.disks[disk_registry.count++] = disk;
 
     spinlock_release_irqrestore(&disk_registry.lock, irq_state);
     terminal_printf("[BufferCache] Registered disk '%s'.\n", disk->blk_dev.device_name);
     return 0;
 }
 
 /**
  * Lookup a disk by device name
  */
 static disk_t *get_disk_by_name(const char *device_name) {
     if (!device_name) return NULL;
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&disk_registry.lock);
 
     disk_t *found_disk = NULL;
     for (int i = 0; i < disk_registry.count; i++) {
         if (strcmp(disk_registry.disks[i]->blk_dev.device_name, device_name) == 0) {
             found_disk = disk_registry.disks[i];
             break;
         }
     }
 
     spinlock_release_irqrestore(&disk_registry.lock, irq_state);
     return found_disk;
 }
 
 /**
  * Initialize the buffer cache system
  */
 void buffer_cache_init(void) {
     // Initialize locks
     spinlock_init(&cache_lock);
     spinlock_init(&disk_registry.lock);
 
     // Initialize hash table
     memset(buffer_hash_table, 0, sizeof(buffer_hash_table));
 
     // Initialize disk registry
     disk_registry.count = 0;
 
     // Clear statistics
     memset(&cache_stats, 0, sizeof(cache_stats));
 
     terminal_write("[BufferCache] Initialized buffer cache system.\n");
 }
 
 /**
  * Lookup a buffer in the cache (internal helper)
  * Assumes cache_lock is already held
  */
 static buffer_t *buffer_lookup_internal(const char *device_name, uint32_t block_number) {
     if (!device_name) return NULL;
 
     uint32_t index = buffer_hash(device_name, block_number);
     buffer_t *buf = buffer_hash_table[index];
 
     while (buf) {
         if (buf->block_number == block_number &&
             buf->disk &&
             strcmp(buf->disk->blk_dev.device_name, device_name) == 0) {
             return buf;
         }
         buf = buf->hash_next;
     }
 
     return NULL;
 }
 
 /**
  * LRU list management: move buffer to front of LRU list
  * Assumes cache_lock is already held
  */
 static void lru_make_most_recent(buffer_t *buf) {
     if (!buf || buf == lru_head) return;
 
     // Remove from current position
     if (buf->lru_prev) buf->lru_prev->lru_next = buf->lru_next;
     if (buf->lru_next) buf->lru_next->lru_prev = buf->lru_prev;
     if (buf == lru_tail) lru_tail = buf->lru_prev;
 
     // Add to head
     buf->lru_prev = NULL;
     buf->lru_next = lru_head;
     if (lru_head) lru_head->lru_prev = buf;
     lru_head = buf;
 
     // If list was empty, set tail
     if (!lru_tail) lru_tail = buf;
 }
 
 /**
  * Remove a buffer from the LRU list
  * Assumes cache_lock is already held
  */
 static void lru_remove(buffer_t *buf) {
     if (!buf) return;
 
     if (buf->lru_prev) {
         buf->lru_prev->lru_next = buf->lru_next;
     } else {
         lru_head = buf->lru_next;
     }
 
     if (buf->lru_next) {
         buf->lru_next->lru_prev = buf->lru_prev;
     } else {
         lru_tail = buf->lru_prev;
     }
 
     buf->lru_prev = buf->lru_next = NULL;
 }
 
 /**
  * Insert buffer into hash table
  * Assumes cache_lock is already held
  */
 static void buffer_insert_internal(buffer_t *buf) {
     if (!buf || !buf->disk || !buf->disk->blk_dev.device_name) return;
 
     uint32_t index = buffer_hash(buf->disk->blk_dev.device_name, buf->block_number);
     buf->hash_next = buffer_hash_table[index];
     buffer_hash_table[index] = buf;
 }
 
 /**
  * Remove buffer from hash table
  * Assumes cache_lock is already held
  */
 static void buffer_remove_internal(buffer_t *buf) {
     if (!buf || !buf->disk || !buf->disk->blk_dev.device_name) return;
 
     uint32_t index = buffer_hash(buf->disk->blk_dev.device_name, buf->block_number);
     buffer_t **pp = &buffer_hash_table[index];
 
     while (*pp) {
         if (*pp == buf) {
             *pp = buf->hash_next;
             buf->hash_next = NULL;
             return;
         }
         pp = &((*pp)->hash_next);
     }
 }
 
 /**
  * Evict the LRU buffer (if possible), flush if dirty, remove from cache, and free it.
  * Returns 0 on success (eviction performed), negative error code if no suitable victim.
  * Handles its own locking internally (acquire, release).
  */
 static int evict_lru_buffer_and_free(void) {
     uintptr_t irq_flags_cache = spinlock_acquire_irqsave(&cache_lock);
 
     buffer_t *victim = lru_tail;
 
     while (victim) {
         if (victim->ref_count == 0) {
             // Found a candidate
             bool needs_flush = (victim->flags & BUFFER_FLAG_DIRTY);
             disk_t* disk_to_write = victim->disk;
             uint32_t block_to_write = victim->block_number;
             size_t sector_size = 0;
             void* data_to_write = NULL; // Temporary buffer
 
             if (needs_flush) {
                 // Validate the disk
                 if (!disk_to_write || !disk_to_write->initialized) {
                     terminal_printf("[Evict] Error: Victim disk invalid for block %u.\n",
                                     block_to_write);
                     // Try previous victim
                     victim = victim->lru_prev;
                     continue;
                 }
 
                 sector_size = disk_to_write->blk_dev.sector_size;
                 if (sector_size == 0) {
                     sector_size = DEFAULT_BUFFER_BLOCK_SIZE;
                 }
 
                 // Allocate a temporary buffer to hold dirty data
                 data_to_write = kmalloc(sector_size);
                 if (!data_to_write) {
                     terminal_printf("[Evict] Error: kmalloc failed for flush buffer.\n");
                     // Cannot flush this one; try previous
                     victim = victim->lru_prev;
                     continue;
                 }
 
                 memcpy(data_to_write, victim->data, sector_size);
                 // Mark clean in cache metadata (under lock)
                 victim->flags &= ~BUFFER_FLAG_DIRTY;
             }
 
             // Remove from LRU and hash while holding lock
             lru_remove(victim);
             buffer_remove_internal(victim);
             cache_stats.evictions++;
 
             // Keep pointer to free outside lock
             buffer_t *victim_to_free = victim;
 
             // Release lock before doing I/O
             spinlock_release_irqrestore(&cache_lock, irq_flags_cache);
 
             // Perform Disk I/O if needed
             if (needs_flush && data_to_write && disk_to_write) {
                int write_result = disk_write_raw_sectors(disk_to_write, block_to_write,
                    data_to_write, 1);

                 if (write_result != 0) {
                     terminal_printf("[Evict] Flush FAILED (Error %d) for block %u.\n",
                                     write_result, block_to_write);
                     cache_stats.io_errors++;
                 } else {
                     cache_stats.writes++;
                 }
             }
 
             if (data_to_write) {
                 kfree(data_to_write);
             }
 
             // Free the victim memory outside the lock
             kfree(victim_to_free->data);
             kfree(victim_to_free);
 
             return 0; // Success
         }
 
         // Move to previous LRU entry
         victim = victim->lru_prev;
     }
 
     // No suitable buffer found
     spinlock_release_irqrestore(&cache_lock, irq_flags_cache);
     return -FS_ERR_NO_RESOURCES;
 }
 
 /**
  * Perform safe read of disk sectors with retries
  */
 static int safe_disk_read(disk_t *disk, uint32_t start_sector, void *buffer, size_t sector_size) {
     if (!disk || !buffer) return -FS_ERR_INVALID_PARAM;
 
     // Retry parameters
     const int max_retries = 3;
     int retries = 0;
     int result = -1;
 
     while (retries < max_retries) {
        result = disk_read_raw_sectors(disk, start_sector, buffer, 1);
         if (result == 0) {
             break; // Success
         }
 
         // Failed, retry
         retries++;
         terminal_printf("[BufferCache] Retry %d: Reading sector %u from '%s'...\n",
                         retries, start_sector, disk->blk_dev.device_name);
     }
 
     if (result != 0) {
         terminal_printf("[BufferCache] Error: Failed to read sector %u from '%s' after %d retries.\n",
                         start_sector, disk->blk_dev.device_name, max_retries);
         cache_stats.io_errors++;
     } else {
         cache_stats.reads++;
     }
 
     return result;
 }
 
 /**
  * Get a buffer (allocate new or return cached)
  */
 buffer_t *buffer_get(const char *device_name, uint32_t block_number) {
     if (!device_name) {
         terminal_write("[BufferCache] Error: NULL device name in buffer_get().\n");
         return NULL;
     }
 
     disk_t *disk = get_disk_by_name(device_name);
     if (!disk || !disk->initialized) {
         terminal_printf("[BufferCache] Error: Device '%s' not found or not initialized.\n", device_name);
         return NULL;
     }
 
     // Check sector size
     if (disk->blk_dev.sector_size < MIN_BUFFER_BLOCK_SIZE ||
         disk->blk_dev.sector_size > MAX_BUFFER_BLOCK_SIZE) {
         terminal_printf("[BufferCache] Error: Invalid sector size %u for device '%s'.\n",
                         disk->blk_dev.sector_size, device_name);
         return NULL;
     }
 
     // Acquire cache lock
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     // Check if buffer is already in cache
     buffer_t *buf = buffer_lookup_internal(device_name, block_number);
     if (buf) {
         // Found in cache
         buf->ref_count++;
         lru_make_most_recent(buf);
         spinlock_release_irqrestore(&cache_lock, irq_state);
         cache_stats.hits++;
         return buf;
     }
 
     // Not found in cache
     cache_stats.misses++;
 
     // Try to allocate the buffer_t
     buf = (buffer_t *)kmalloc(sizeof(buffer_t));
     if (!buf) {
         terminal_write("[BufferCache] kmalloc failed for buffer_t, attempting eviction...\n");
         // Release lock before eviction
         spinlock_release_irqrestore(&cache_lock, irq_state);
 
         // Attempt eviction
         if (evict_lru_buffer_and_free() == 0) {
             // Eviction succeeded, re-acquire lock & retry allocation
             irq_state = spinlock_acquire_irqsave(&cache_lock);
             buf = (buffer_t *)kmalloc(sizeof(buffer_t));
         } else {
             // Eviction failed or no buffers to evict
             irq_state = spinlock_acquire_irqsave(&cache_lock);
         }
 
         if (!buf) {
             terminal_write("[BufferCache] kmalloc failed for buffer_t even after eviction.\n");
             spinlock_release_irqrestore(&cache_lock, irq_state);
             return NULL;
         }
     }
 
     memset(buf, 0, sizeof(buffer_t));
     buf->disk = disk;
     buf->block_number = block_number;
     buf->ref_count = 1;
     buf->flags = 0;
 
     // Allocate buffer->data
     size_t sector_size = disk->blk_dev.sector_size;
     buf->data = kmalloc(sector_size + BUFFER_PADDING);
     if (!buf->data) {
         terminal_write("[BufferCache] kmalloc failed for buffer data, attempting eviction...\n");
         // Free the buf struct itself before we try eviction
         kfree(buf);
         spinlock_release_irqrestore(&cache_lock, irq_state);
 
         if (evict_lru_buffer_and_free() == 0) {
             // Retake lock and retry
             irq_state = spinlock_acquire_irqsave(&cache_lock);
             buf = (buffer_t *)kmalloc(sizeof(buffer_t));
             if (buf) {
                 memset(buf, 0, sizeof(buffer_t));
                 buf->disk = disk;
                 buf->block_number = block_number;
                 buf->ref_count = 1;
                 buf->flags = 0;
                 buf->data = kmalloc(sector_size + BUFFER_PADDING);
             }
         } else {
             // Could not evict
             irq_state = spinlock_acquire_irqsave(&cache_lock);
         }
 
         if (!buf || !buf->data) {
             terminal_write("[BufferCache] kmalloc failed for buffer data even after eviction.\n");
             if (buf) {
                 // if buf->data is NULL, free buf
                 kfree(buf);
             }
             spinlock_release_irqrestore(&cache_lock, irq_state);
             return NULL;
         }
     }
 
     // Zero data area (including padding)
     memset(buf->data, 0, sector_size + BUFFER_PADDING);
 
     // Release lock for disk I/O
     spinlock_release_irqrestore(&cache_lock, irq_state);
 
     // Read data from disk
     int read_result = safe_disk_read(disk, block_number, buf->data, disk->blk_dev.sector_size);
 
     // Re-acquire lock
     irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     if (read_result != 0) {
         // Read failed, free buffer
         kfree(buf->data);
         kfree(buf);
         spinlock_release_irqrestore(&cache_lock, irq_state);
         terminal_printf("[BufferCache] Error: Failed to read block %u from device '%s'.\n",
                         block_number, device_name);
         return NULL;
     }
 
     // Mark buffer as valid
     buf->flags |= BUFFER_FLAG_VALID;
 
     // Insert into hash table and LRU list
     buffer_insert_internal(buf);
     lru_make_most_recent(buf);
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
     return buf;
 }
 
 /**
  * Release a buffer
  */
 void buffer_release(buffer_t *buf) {
     if (!buf) return;
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     if (buf->ref_count > 0) {
         buf->ref_count--;
     } else {
         terminal_printf("[BufferCache] Warning: Releasing buffer with ref_count=0 (block %u on '%s').\n",
                         buf->block_number,
                         buf->disk ? buf->disk->blk_dev.device_name : "unknown");
     }
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 }
 
 /**
  * Mark a buffer as dirty
  */
 void buffer_mark_dirty(buffer_t *buf) {
     if (!buf) return;
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     // Only mark valid buffers as dirty
     if (buf->flags & BUFFER_FLAG_VALID) {
         buf->flags |= BUFFER_FLAG_DIRTY;
     } else {
         terminal_printf("[BufferCache] Warning: Attempted to mark invalid buffer as dirty (%u on '%s').\n",
                         buf->block_number,
                         buf->disk ? buf->disk->blk_dev.device_name : "unknown");
     }
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 }
 
 /**
  * Flush a single buffer to disk
  */
 int buffer_flush(buffer_t *buf) {
     if (!buf || !buf->disk) {
         return -FS_ERR_INVALID_PARAM;
     }
 
     // Check if buffer needs flushing
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     if (!(buf->flags & BUFFER_FLAG_DIRTY) || !(buf->flags & BUFFER_FLAG_VALID)) {
         // Nothing to flush
         spinlock_release_irqrestore(&cache_lock, irq_state);
         return 0;
     }
 
     disk_t *disk = buf->disk;
     uint32_t block = buf->block_number;
     size_t buffer_size = disk->blk_dev.sector_size;
 
     // Copy data to temporary buffer for writing outside the lock
     void *temp_data = kmalloc(buffer_size);
     if (!temp_data) {
         spinlock_release_irqrestore(&cache_lock, irq_state);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     memcpy(temp_data, buf->data, buffer_size);
     buf->flags &= ~BUFFER_FLAG_DIRTY; // Mark clean now under lock
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 
     // Write to disk without holding the lock
     int write_result = disk_write_raw_sectors(disk, block, temp_data, 1);
     kfree(temp_data);
 
     if (write_result != 0) {
         cache_stats.io_errors++;
         terminal_printf("[BufferCache] Error: Failed to write block %u to disk '%s'.\n",
                         block, disk->blk_dev.device_name);
         return -FS_ERR_IO;
     }
 
     cache_stats.writes++;
 
     return 0;
 }
 
 /**
  * Sync all dirty buffers
  */
 void buffer_cache_sync(void) {
     terminal_write("[BufferCache] Starting full cache sync...\n");
 
     int total_flushed = 0;
     int errors = 0;
 
     // Get all dirty buffers
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     // Create a copy of all dirty buffers to avoid long lock hold
     buffer_t **dirty_buffers = kmalloc(sizeof(buffer_t*) * BUFFER_CACHE_HASH_SIZE);
     if (!dirty_buffers) {
         spinlock_release_irqrestore(&cache_lock, irq_state);
         terminal_write("[BufferCache] Error: Failed to allocate memory for sync.\n");
         return;
     }
 
     int dirty_count = 0;
 
     // Scan all hash buckets
     for (uint32_t i = 0; i < BUFFER_CACHE_HASH_SIZE; i++) {
         buffer_t *buf = buffer_hash_table[i];
         while (buf) {
             if ((buf->flags & BUFFER_FLAG_DIRTY) && (buf->flags & BUFFER_FLAG_VALID)) {
                 // Increment ref count to prevent eviction during sync
                 buf->ref_count++;
                 dirty_buffers[dirty_count++] = buf;
 
                 // Resize array if needed (unlikely, but just in case)
                 if (dirty_count >= (int)BUFFER_CACHE_HASH_SIZE && i < BUFFER_CACHE_HASH_SIZE - 1) {
                     buffer_t **new_array = kmalloc(sizeof(buffer_t*) * BUFFER_CACHE_HASH_SIZE * 2);
                     if (!new_array) {
                         // Decrement ref counts for buffers we've already found
                         for (int j = 0; j < dirty_count; j++) {
                             dirty_buffers[j]->ref_count--;
                         }
                         spinlock_release_irqrestore(&cache_lock, irq_state);
                         kfree(dirty_buffers);
                         terminal_write("[BufferCache] Error: Failed to resize dirty buffer array.\n");
                         return;
                     }
                     // Copy to new array
                     memcpy(new_array, dirty_buffers, sizeof(buffer_t*) * dirty_count);
                     kfree(dirty_buffers);
                     dirty_buffers = new_array;
                 }
             }
             buf = buf->hash_next;
         }
     }
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 
     // Flush each dirty buffer
     for (int i = 0; i < dirty_count; i++) {
         buffer_t *buf = dirty_buffers[i];
 
         int flush_result = buffer_flush(buf);
         if (flush_result != 0) {
             errors++;
         } else {
             total_flushed++;
         }
 
         // Release the extra reference we added
         irq_state = spinlock_acquire_irqsave(&cache_lock);
         if (buf->ref_count > 0) {
             buf->ref_count--;
         }
         spinlock_release_irqrestore(&cache_lock, irq_state);
     }
 
     kfree(dirty_buffers);
 
     terminal_printf("[BufferCache] Sync complete: %d flushed, %d errors.\n", total_flushed, errors);
 }
 
 /**
  * Get buffer cache statistics
  */
 void buffer_cache_get_stats(buffer_cache_stats_t *stats) {
     if (!stats) return;
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     stats->hits = cache_stats.hits;
     stats->misses = cache_stats.misses;
     stats->reads = cache_stats.reads;
     stats->writes = cache_stats.writes;
     stats->evictions = cache_stats.evictions;
     stats->alloc_failures = cache_stats.alloc_failures;
     stats->io_errors = cache_stats.io_errors;
 
     // Count current buffers
     stats->cached_buffers = 0;
     stats->dirty_buffers = 0;
 
     buffer_t *buf = lru_head;
     while (buf) {
         stats->cached_buffers++;
         if (buf->flags & BUFFER_FLAG_DIRTY) {
             stats->dirty_buffers++;
         }
         buf = buf->lru_next;
     }
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 }
 
 /**
  * Invalidate all buffers for a specific device
  */
 void buffer_invalidate_device(const char *device_name) {
     if (!device_name) return;
 
     uintptr_t irq_state = spinlock_acquire_irqsave(&cache_lock);
 
     int invalidated = 0;
 
     // Check all hash buckets
     for (uint32_t i = 0; i < BUFFER_CACHE_HASH_SIZE; i++) {
         buffer_t **pp = &buffer_hash_table[i];
         while (*pp) {
             buffer_t *buf = *pp;
 
             if (buf->disk && strcmp(buf->disk->blk_dev.device_name, device_name) == 0) {
                 // Check if buffer can be invalidated
                 if (buf->ref_count > 0) {
                     // Skip buffers still in use
                     pp = &buf->hash_next;
                 } else {
                     // Remove from hash table
                     *pp = buf->hash_next;
 
                     // Remove from LRU list
                     lru_remove(buf);
 
                     // Free the buffer
                     kfree(buf->data);
                     kfree(buf);
 
                     invalidated++;
                 }
             } else {
                 pp = &buf->hash_next;
             }
         }
     }
 
     spinlock_release_irqrestore(&cache_lock, irq_state);
 
     terminal_printf("[BufferCache] Invalidated %d buffers for device '%s'.\n",
                     invalidated, device_name);
 }
 