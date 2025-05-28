/**
 * buffer_cache.h - Buffer cache interface for disk sector caching
 */

 #ifndef BUFFER_CACHE_H
 #define BUFFER_CACHE_H
 
 #include <kernel/core/types.h>
 #include <kernel/drivers/storage/disk.h>
 
 // Buffer flags
 #define BUFFER_FLAG_VALID   0x01  // Buffer contains valid data
 #define BUFFER_FLAG_DIRTY   0x02  // Buffer has been modified, needs writing
 #define BUFFER_FLAG_LOCKED  0x04  // Buffer is locked for I/O
 #define BUFFER_FLAG_ERROR   0x08  // Buffer has an I/O error
 #define MAX_BUFFER_BLOCK_SIZE      8192 
 
 // Statistics structure
 typedef struct {
     uint32_t hits;           // Cache hits
     uint32_t misses;         // Cache misses
     uint32_t reads;          // Disk reads performed
     uint32_t writes;         // Disk writes performed
     uint32_t evictions;      // Number of buffers evicted
     uint32_t alloc_failures; // Memory allocation failures
     uint32_t io_errors;      // I/O errors encountered
     uint32_t cached_buffers; // Current number of buffers in cache
     uint32_t dirty_buffers;  // Current number of dirty buffers
 } buffer_cache_stats_t;
 
 // Buffer structure
 typedef struct buffer buffer_t;
 
 struct buffer {
     disk_t *disk;            // Disk this buffer belongs to
     uint32_t block_number;   // Block number on disk
     uint8_t *data;           // Pointer to the data
     uint32_t flags;          // Buffer flags
     uint32_t ref_count;      // Reference count
     
     // Hash table chain
     buffer_t *hash_next;
     
     // LRU list pointers
     buffer_t *lru_prev;
     buffer_t *lru_next;
 };
 
 // Initialize the buffer cache system
 void buffer_cache_init(void);
 
 // Register a disk with the buffer cache
 int buffer_register_disk(disk_t *disk);
 
 // Get a buffer from the cache or disk
 buffer_t *buffer_get(const char *device_name, uint32_t block_number);
 
 // Release a buffer
 void buffer_release(buffer_t *buf);
 
 // Mark a buffer as dirty
 void buffer_mark_dirty(buffer_t *buf);
 
 // Flush a single buffer to disk
 int buffer_flush(buffer_t *buf);
 
 // Sync all dirty buffers to disk
 void buffer_cache_sync(void);
 
 // Get buffer cache statistics
 void buffer_cache_get_stats(buffer_cache_stats_t *stats);
 
 // Invalidate all buffers for a specific device
 void buffer_invalidate_device(const char *device_name);
 
 #endif /* BUFFER_CACHE_H */