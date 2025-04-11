#include "buffer_cache.h"
#include "kmalloc.h"
#include "terminal.h"
#include "disk.h"           // Include disk.h for disk operations
#include "fs_errno.h"       // For error codes
#include <string.h>         // For strcmp, memset
#include "types.h"

/* Assume a global disk_t is initialized elsewhere for the primary disk.
 * In a multi-disk system, you'd need a proper lookup mechanism.
 */
// --- This is a placeholder, replace with your actual disk management ---
static disk_t g_main_disk; // TEMPORARY PLACEHOLDER - Initialize this properly!
static bool g_main_disk_initialized = false;
// --- End Placeholder ---


// Define the block size used by the underlying device.
// Use the actual size from the block device within the disk_t structure.
#define DEFAULT_BUFFER_BLOCK_SIZE 512

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
        // Use the provided device_name for initialization
        if (disk_init(&g_main_disk, device_name) == 0) {
            g_main_disk_initialized = true;
        } else {
            terminal_printf("[BufferCache] Failed to init placeholder disk for %s.\n", device_name);
            return NULL;
        }
    }
    // Access device_name via the nested blk_dev structure
    if (strcmp(g_main_disk.blk_dev.device_name, device_name) == 0) {
        return &g_main_disk;
    }
    terminal_printf("[BufferCache] Device name lookup failed for %s (using placeholder %s).\n",
                    device_name, g_main_disk.blk_dev.device_name);
    return NULL; // Indicate device not found/supported by this placeholder
    // --- End Placeholder Implementation ---
}


/*
 * Compute a simple hash based on disk pointer and block number.
 */
static uint32_t buffer_hash(disk_t *disk, uint32_t block_number) {
    // Simple hash combining block number and potentially part of the disk pointer address
    uint32_t hash = block_number ^ (uintptr_t)disk;
    // Access device_name via the nested blk_dev structure
    if (disk && disk->blk_dev.device_name) {
        // Access device_name via the nested blk_dev structure
        for (const char *p = disk->blk_dev.device_name; *p; p++) {
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
 * (Marked as unused by the compiler, keep for potential LRU implementation)
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
    if (!device_name) { return NULL; }
    disk_t *disk = get_disk_by_name(device_name);
    if (!disk || !disk->initialized) { return NULL; }

    buffer_t *buf = buffer_lookup(disk, block_number);
    if (buf) {
        buf->ref_count++;
        return buf;
    }

    buf = (buffer_t *)kmalloc(sizeof(buffer_t));
    if (!buf) { return NULL; }
    memset(buf, 0, sizeof(buffer_t));

    buf->disk = disk;
    buf->block_number = block_number;
    buf->ref_count = 1;

    size_t data_block_size = disk->blk_dev.sector_size > 0 ? disk->blk_dev.sector_size : DEFAULT_BUFFER_BLOCK_SIZE;
    buf->data = (uint8_t *)kmalloc(data_block_size);
    if (!buf->data) { kfree(buf); return NULL; } // Use correct kfree

    if (disk_read_sectors(disk, block_number, buf->data, 1) != 0) {
        // *** Fixed kfree call: Only pass the pointer ***
        kfree(buf->data);
        kfree(buf);
        return NULL;
    }

    buf->flags |= BUFFER_FLAG_VALID;
    buffer_insert(buf);
    return buf;
}

/*
 * Release a buffer (decrement its reference count).
 */
void buffer_release(buffer_t *buf) {
    if (!buf)
        return;

    // Add lock here if using multi-threading/SMP
    if (buf->ref_count > 0) {
        buf->ref_count--;
    } else {
         terminal_printf("[BufferCache] Warning: Releasing buffer with ref_count=0 (block %u)\n", buf->block_number);
    }

    // If ref_count is 0 and using LRU, this buffer is now eligible for eviction.
    // The LRU list management would handle this.
    // Unlock here
}

/*
 * Mark a buffer as dirty.
 */
void buffer_mark_dirty(buffer_t *buf) {
    if (buf) {
        // Add lock here if using multi-threading/SMP accessing flags
        buf->flags |= BUFFER_FLAG_DIRTY;
        // Unlock here
    }
}

/*
 * Flush a single buffer to disk if it is dirty.
 */
int buffer_flush(buffer_t *buf) {
    if (!buf || !buf->disk || !buf->disk->initialized) {
        return -FS_ERR_INVALID_PARAM;
    }

    // Add lock here if multi-threading/SMP accessing flags
    if (!(buf->flags & BUFFER_FLAG_DIRTY)) {
        // Unlock here
        return 0; // Not dirty, nothing to do
    }
    if (!(buf->flags & BUFFER_FLAG_VALID)) {
         terminal_printf("[BufferCache] buffer_flush: Attempted to flush invalid buffer (block %u).\n", buf->block_number);
         // Unlock here
         return -FS_ERR_IO; // Cannot flush invalid buffer
    }
    // Unlock flags lock if separate

    // Access sector_size via the nested blk_dev structure
    size_t data_block_size = buf->disk->blk_dev.sector_size > 0 ? buf->disk->blk_dev.sector_size : DEFAULT_BUFFER_BLOCK_SIZE;

    // Use disk_write_sectors
    if (disk_write_sectors(buf->disk, buf->block_number, buf->data, 1) != 0) {
        terminal_printf("[BufferCache] buffer_flush: Failed to write block %u to disk %s.\n",
                        buf->block_number, buf->disk->blk_dev.device_name);
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
    int errors = 0;
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
                    errors++;
                    terminal_printf("[BufferCache] buffer_cache_sync: Failed to flush buffer block %u. Error: %d\n",
                                    buf->block_number, flush_result);
                }
            }
            // Unlock buffer here if needed
            buf = buf->hash_next;
        }
        // Unlock hash bucket here
    }
    if (errors == 0) {
        terminal_write("[BufferCache] Sync complete.\n");
    } else {
         terminal_printf("[BufferCache] Sync complete with %d errors.\n", errors);
    }
}


// --- Add Buffer Eviction Logic (LRU Example - Conceptual Stubs) ---
/*
static buffer_t *lru_head = NULL;
static buffer_t *lru_tail = NULL;

static void lru_remove_node(buffer_t *buf) {
     if (!buf) return;
     if (buf->prev) buf->prev->next = buf->next; else lru_head = buf->next;
     if (buf->next) buf->next->prev = buf->prev; else lru_tail = buf->prev;
     buf->prev = buf->next = NULL;
}

static void lru_add_to_front(buffer_t *buf) {
    if (!buf) return;
    buf->next = lru_head;
    buf->prev = NULL;
    if (lru_head) lru_head->prev = buf;
    lru_head = buf;
    if (!lru_tail) lru_tail = buf;
}

static void lru_make_recent(buffer_t *buf) {
    if (!buf || buf == lru_head) return; // Already most recent
    lru_remove_node(buf);
    lru_add_to_front(buf);
}

static buffer_t* evict_lru_buffer() {
    buffer_t *victim = lru_tail;
    while(victim) {
        if (victim->ref_count == 0) {
            // Found a candidate
            if (victim->flags & BUFFER_FLAG_DIRTY) {
                if (buffer_flush(victim) != 0) {
                     terminal_printf("[BufferCache] Failed to flush LRU victim block %u during eviction.\n", victim->block_number);
                     // Skip this one and try the next? Or fail eviction?
                     victim = victim->prev;
                     continue; // Try previous buffer
                }
            }
            // Remove from LRU list
            lru_remove_node(victim);
            // Remove from hash table
            buffer_remove(victim);
            return victim; // Return buffer to be freed (data and struct)
        }
        victim = victim->prev; // Check previous buffer
    }
    return NULL; // No suitable buffer to evict
}

// Modify buffer_get: If kmalloc fails, call evict_lru_buffer(), free the victim's data and struct, then retry kmalloc.
// Modify buffer_get/lookup hit: call lru_make_recent(buf)
// Modify buffer_release: If ref_count becomes 0, potentially add to tail of LRU list? (Usually added on first get/load)
*/