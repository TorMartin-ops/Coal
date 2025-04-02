#include "buffer_cache.h"
#include "kmalloc.h"
#include "terminal.h"
#include "disk.h"           // Include disk.h for disk operations
#include "fs_errno.h"       // For error codes
#include <string.h>
#include "types.h"


/* Assume a global disk_t is initialized elsewhere for the primary disk.
 * In a multi-disk system, you'd need a proper lookup mechanism.
 * Example: extern disk_t g_main_disk; */
// --- This is a placeholder, replace with your actual disk management ---
static disk_t g_main_disk; // TEMPORARY PLACEHOLDER - Initialize this properly!
static bool g_main_disk_initialized = false;
// --- End Placeholder ---


// Define the block size used by the underlying device (commonly 512 bytes).
// It might be better to get this from the disk_t structure.
#define BUFFER_BLOCK_SIZE 512

// Define the size of the hash table used to index cached buffers.
#define BUFFER_CACHE_HASH_SIZE 64

// Global hash table of buffer pointers.
static buffer_t *buffer_hash_table[BUFFER_CACHE_HASH_SIZE];


/*
 * Helper to get the disk_t for a device name (simplified).
 * Replace this with your actual device lookup.
 */
static disk_t* get_disk_by_name(const char *device_name) {
    // --- Temporary Placeholder Implementation ---
    if (!g_main_disk_initialized) {
        if (disk_init(&g_main_disk, device_name) == 0) {
            g_main_disk_initialized = true;
        } else {
            terminal_write("[BufferCache] Failed to init placeholder disk.\n");
            return NULL;
        }
    }
    // Simple check if the requested name matches the global disk
    if (strcmp(g_main_disk.device_name, device_name) == 0) {
        return &g_main_disk;
    }
    terminal_write("[BufferCache] Device name lookup failed (using placeholder).\n");
    return NULL; // Indicate device not found/supported by this placeholder
    // --- End Placeholder Implementation ---
}


/*
 * Compute a simple hash based on disk pointer and block number.
 * Using disk pointer address for hashing might be simple but check distribution.
 */
static uint32_t buffer_hash(disk_t *disk, uint32_t block_number) {
    // Simple hash combining block number and potentially part of the disk pointer address
    uint32_t hash = block_number ^ (uintptr_t)disk;
    // Add device name characters for better distribution if needed
    if (disk && disk->device_name) {
        for (const char *p = disk->device_name; *p; p++) {
             hash = hash * 31 + (uint8_t)(*p);
        }
    }
    return hash % BUFFER_CACHE_HASH_SIZE;
}

/*
 * Look up a buffer in the cache using disk_t*.
 */
static buffer_t *buffer_lookup(disk_t *disk, uint32_t block_number) {
    if (!disk) return NULL;
    uint32_t index = buffer_hash(disk, block_number);
    buffer_t *buf = buffer_hash_table[index];
    while (buf) {
        // Compare block number AND the disk pointer
        if ((buf->block_number == block_number) && (buf->disk == disk)) {
            return buf;
        }
        buf = buf->hash_next;
    }
    return NULL;
}

/*
 * Insert a buffer into the hash table.
 */
static void buffer_insert(buffer_t *buf) {
    if (!buf || !buf->disk) return;
    uint32_t index = buffer_hash(buf->disk, buf->block_number);
    buf->hash_next = buffer_hash_table[index];
    buffer_hash_table[index] = buf;
}

/*
 * Remove a buffer from the hash table.
 */
static void buffer_remove(buffer_t *buf) {
     if (!buf || !buf->disk) return;
    uint32_t index = buffer_hash(buf->disk, buf->block_number);
    buffer_t **prev = &buffer_hash_table[index];
    while (*prev) {
        if (*prev == buf) {
            *prev = buf->hash_next;
            break;
        }
        prev = &((*prev)->hash_next);
    }
}

/*
 * Initialize the buffer cache.
 */
void buffer_cache_init(void) {
    memset(buffer_hash_table, 0, sizeof(buffer_hash_table));
    // Consider initializing the placeholder disk here or ensuring it's done before first use
    // if (!g_main_disk_initialized) { ... disk_init(&g_main_disk, "hd0"); ... }
    terminal_write("[BufferCache] Initialized.\n");
}

/*
 * Retrieve (or load) a buffer for the given device name and block number.
 */
buffer_t *buffer_get(const char *device_name, uint32_t block_number) {
    if (!device_name) {
        terminal_write("[BufferCache] buffer_get: Invalid device name parameter.\n");
        return NULL;
    }

    // Get the disk_t structure for the device name
    disk_t *disk = get_disk_by_name(device_name);
    if (!disk) {
         terminal_write("[BufferCache] buffer_get: Could not find disk for device: ");
         terminal_write(device_name);
         terminal_write("\n");
         return NULL;
    }

    // Lookup using the disk_t pointer
    buffer_t *buf = buffer_lookup(disk, block_number);
    if (buf) {
        buf->ref_count++;
        return buf;
    }

    // Not in cache, allocate a new buffer structure.
    buf = (buffer_t *)kmalloc(sizeof(buffer_t));
    if (!buf) {
        terminal_write("[BufferCache] buffer_get: Out of memory for buffer_t.\n");
        return NULL;
    }
    memset(buf, 0, sizeof(buffer_t)); // Clear the buffer structure

    // Store disk_t pointer instead of name
    buf->disk = disk;
    buf->block_number = block_number;
    buf->flags = 0;
    buf->ref_count = 1;
    buf->prev = NULL;
    buf->next = NULL;
    buf->hash_next = NULL;

    // Allocate data block. Use actual sector size from disk if possible.
    size_t data_block_size = disk->sector_size > 0 ? disk->sector_size : BUFFER_BLOCK_SIZE;
    buf->data = (uint8_t *)kmalloc(data_block_size);
    if (!buf->data) {
        terminal_write("[BufferCache] buffer_get: Out of memory for data buffer.\n");
        kfree(buf, sizeof(buffer_t));
        return NULL;
    }

    // Read block from disk using disk_read_sectors
    if (disk_read_sectors(disk, block_number, buf->data, 1) != 0) {
        terminal_write("[BufferCache] buffer_get: Failed to read block from disk.\n");
        kfree(buf->data, data_block_size);
        kfree(buf, sizeof(buffer_t));
        return NULL;
    }

    buf->flags |= BUFFER_FLAG_VALID;
    buffer_insert(buf); // Insert into hash table
    return buf;
}

/*
 * Release a buffer (decrement its reference count).
 */
void buffer_release(buffer_t *buf) {
    if (!buf)
        return;

    // Add lock here if using multi-threading/SMP
    if (buf->ref_count > 0)
        buf->ref_count--;
    // Unlock here

    // In a production system, buffers with zero references may be
    // subject to an LRU eviction algorithm here. If ref_count is 0,
    // potentially add buf to an LRU list.
}

/*
 * Mark a buffer as dirty.
 */
void buffer_mark_dirty(buffer_t *buf) {
    if (buf) {
        // Add lock here if using multi-threading/SMP
        buf->flags |= BUFFER_FLAG_DIRTY;
        // Unlock here
    }
}

/*
 * Flush a single buffer to disk if it is dirty.
 */
int buffer_flush(buffer_t *buf) {
    if (!buf || !buf->disk)
        return -FS_ERR_INVALID_PARAM;

    // Add lock here if multi-threading/SMP accessing flags
    if (!(buf->flags & BUFFER_FLAG_DIRTY)) {
        // Unlock here
        return 0; // Not dirty, nothing to do
    }
    if (!(buf->flags & BUFFER_FLAG_VALID)) {
         terminal_write("[BufferCache] buffer_flush: Attempted to flush invalid buffer.\n");
         // Unlock here
         return -FS_ERR_IO; // Cannot flush invalid buffer
    }
    // Unlock flags lock if separate

    size_t data_block_size = buf->disk->sector_size > 0 ? buf->disk->sector_size : BUFFER_BLOCK_SIZE;

    // Use disk_write_sectors
    if (disk_write_sectors(buf->disk, buf->block_number, buf->data, 1) != 0) {
        terminal_write("[BufferCache] buffer_flush: Failed to write block to disk.\n");
        return -FS_ERR_IO;
    }

    // Add lock here if multi-threading/SMP accessing flags
    buf->flags &= ~BUFFER_FLAG_DIRTY; // Clear dirty flag *after* successful write
    // Unlock here

    return 0; // Success
}

/*
 * Flush all dirty buffers with zero reference count.
 */
void buffer_cache_sync(void) {
    terminal_write("[BufferCache] Starting sync...\n");
    for (uint32_t i = 0; i < BUFFER_CACHE_HASH_SIZE; i++) {
        // Lock hash bucket here if multi-threading/SMP
        buffer_t *buf = buffer_hash_table[i];
        while (buf) {
            // Lock buffer here if needed (e.g., for ref_count/flags check)
            if ((buf->flags & BUFFER_FLAG_DIRTY) && (buf->ref_count == 0)) {
                // Attempt to flush. buffer_flush handles its own locking/flag clearing.
                 int flush_result = buffer_flush(buf);
                 if (flush_result != 0) {
                    // Log error, but continue syncing others
                    terminal_write("[BufferCache] buffer_cache_sync: Failed to flush a buffer. Error: ");
                    // Optionally print error code based on flush_result
                    terminal_write("\n");
                }
            }
            // Unlock buffer here if needed
            buf = buf->hash_next;
        }
        // Unlock hash bucket here
    }
    terminal_write("[BufferCache] Sync complete.\n");
}


// --- Add Buffer Eviction Logic (LRU Example - Conceptual) ---
/*
static buffer_t *lru_head = NULL;
static buffer_t *lru_tail = NULL;

// Function to move a buffer to the front of the LRU list (when accessed)
static void lru_make_recent(buffer_t *buf) {
    if (!buf) return;
    // Remove from current position (if any)
    if (buf->prev) buf->prev->next = buf->next;
    if (buf->next) buf->next->prev = buf->prev;
    if (lru_tail == buf) lru_tail = buf->prev;
    if (lru_head == buf) lru_head = buf->next;

    // Add to front
    buf->next = lru_head;
    buf->prev = NULL;
    if (lru_head) lru_head->prev = buf;
    lru_head = buf;
    if (!lru_tail) lru_tail = buf;
}

// Function to evict the least recently used buffer
static buffer_t* evict_lru_buffer() {
    buffer_t *victim = lru_tail;
    while(victim) {
        if (victim->ref_count == 0) {
            // Found a candidate
            if (victim->flags & BUFFER_FLAG_DIRTY) {
                if (buffer_flush(victim) != 0) {
                     terminal_write("[BufferCache] Failed to flush LRU victim during eviction.\n");
                     // Skip this one and try the next? Or fail eviction?
                     victim = victim->prev;
                     continue; // Try previous buffer
                }
            }
            // Remove from LRU list
             if (victim->prev) victim->prev->next = NULL;
             lru_tail = victim->prev; // Update tail
             if (lru_head == victim) lru_head = NULL; // List becomes empty

             // Remove from hash table
             buffer_remove(victim);
             return victim; // Return buffer to be freed
        }
        victim = victim->prev; // Check previous buffer
    }
    return NULL; // No suitable buffer to evict
}

// Modify buffer_get: If allocation fails, call evict_lru_buffer() and retry kmalloc.
// Modify buffer_release: If ref_count becomes 0, add to tail of LRU list? (Or only add on first get?)
// Modify buffer_lookup hit: call lru_make_recent(buf)
*/