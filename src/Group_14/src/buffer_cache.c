#include "buffer_cache.h"
#include "kmalloc.h"
#include "terminal.h"
#include <string.h>
#include "types.h"   

// Assume these functions are provided by your lower-level block device module.
extern int block_read(const char *device, uint32_t lba, void *buffer, size_t count);
extern int block_write(const char *device, uint32_t lba, const void *buffer, size_t count);

// Define the block size used by the underlying device (commonly 512 bytes).
#define BUFFER_BLOCK_SIZE 512

// Define the size of the hash table used to index cached buffers.
#define BUFFER_CACHE_HASH_SIZE 64

// Global hash table of buffer pointers.
static buffer_t *buffer_hash_table[BUFFER_CACHE_HASH_SIZE];

/* 
 * Compute a simple hash based on device string and block number.
 */
static uint32_t buffer_hash(const char *device, uint32_t block_number) {
    uint32_t hash = block_number;
    for (const char *p = device; *p; p++) {
        hash = hash * 31 + (uint8_t)(*p);
    }
    return hash % BUFFER_CACHE_HASH_SIZE;
}

/*
 * Look up a buffer in the cache.
 */
static buffer_t *buffer_lookup(const char *device, uint32_t block_number) {
    uint32_t index = buffer_hash(device, block_number);
    buffer_t *buf = buffer_hash_table[index];
    while (buf) {
        if ((buf->block_number == block_number) && (strcmp(buf->device, device) == 0)) {
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
    uint32_t index = buffer_hash(buf->device, buf->block_number);
    buf->hash_next = buffer_hash_table[index];
    buffer_hash_table[index] = buf;
}

/*
 * Remove a buffer from the hash table.
 */
static void buffer_remove(buffer_t *buf) {
    uint32_t index = buffer_hash(buf->device, buf->block_number);
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
    terminal_write("[BufferCache] Initialized.\n");
}

/*
 * Retrieve (or load) a buffer for the given device and block number.
 */
buffer_t *buffer_get(const char *device, uint32_t block_number) {
    if (!device) {
        terminal_write("[BufferCache] buffer_get: Invalid device parameter.\n");
        return NULL;
    }
    buffer_t *buf = buffer_lookup(device, block_number);
    if (buf) {
        buf->ref_count++;
        return buf;
    }
    // Allocate a new buffer structure.
    buf = (buffer_t *)kmalloc(sizeof(buffer_t));
    if (!buf) {
        terminal_write("[BufferCache] buffer_get: Out of memory for buffer_t.\n");
        return NULL;
    }
    buf->device = device;  // In production, consider duplicating the string.
    buf->block_number = block_number;
    buf->flags = 0;
    buf->ref_count = 1;
    buf->prev = NULL;
    buf->next = NULL;
    buf->hash_next = NULL;
    
    // Allocate data block.
    buf->data = (uint8_t *)kmalloc(BUFFER_BLOCK_SIZE);
    if (!buf->data) {
        terminal_write("[BufferCache] buffer_get: Out of memory for data buffer.\n");
        kfree(buf, sizeof(buffer_t));
        return NULL;
    }
    // Read block from disk.
    if (block_read(device, block_number, buf->data, 1) != 0) {
        terminal_write("[BufferCache] buffer_get: Failed to read block from disk.\n");
        kfree(buf->data, BUFFER_BLOCK_SIZE);
        kfree(buf, sizeof(buffer_t));
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
    if (buf->ref_count > 0)
        buf->ref_count--;
    // In a production system, buffers with zero references may be
    // subject to an LRU eviction algorithm.
}

/*
 * Mark a buffer as dirty.
 */
void buffer_mark_dirty(buffer_t *buf) {
    if (buf)
        buf->flags |= BUFFER_FLAG_DIRTY;
}

/*
 * Flush a single buffer to disk if it is dirty.
 */
int buffer_flush(buffer_t *buf) {
    if (!buf)
        return -1;
    if (!(buf->flags & BUFFER_FLAG_DIRTY))
        return 0;
    if (block_write(buf->device, buf->block_number, buf->data, 1) != 0) {
        terminal_write("[BufferCache] buffer_flush: Failed to write block to disk.\n");
        return -1;
    }
    buf->flags &= ~BUFFER_FLAG_DIRTY;
    return 0;
}

/*
 * Flush all dirty buffers with zero reference count.
 */
void buffer_cache_sync(void) {
    for (uint32_t i = 0; i < BUFFER_CACHE_HASH_SIZE; i++) {
        buffer_t *buf = buffer_hash_table[i];
        while (buf) {
            if ((buf->flags & BUFFER_FLAG_DIRTY) && (buf->ref_count == 0)) {
                if (buffer_flush(buf) != 0) {
                    terminal_write("[BufferCache] buffer_cache_sync: Failed to flush a buffer.\n");
                }
            }
            buf = buf->hash_next;
        }
    }
    terminal_write("[BufferCache] Sync complete.\n");
}
