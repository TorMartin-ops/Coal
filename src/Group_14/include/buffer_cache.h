#pragma once
#ifndef BUFFER_CACHE_H
#define BUFFER_CACHE_H

#include "types.h"
#include "disk.h" // Include disk.h for disk_t type

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer flag definitions */
#define BUFFER_FLAG_VALID   0x01 // Buffer contains valid data read from disk
#define BUFFER_FLAG_DIRTY   0x02 // Buffer has been modified and needs writing back

/**
 * buffer_t
 *
 * Represents a cached disk block.
 */
typedef struct buffer {
    uint32_t block_number;      // Disk block number (LBA)
    disk_t *disk;               // Pointer to the disk device structure this buffer belongs to
    uint8_t *data;              // Pointer to the cached data (size typically disk->sector_size)
    uint32_t flags;             // Status flags (BUFFER_FLAG_VALID, BUFFER_FLAG_DIRTY)
    uint32_t ref_count;         // Reference count (how many users currently hold this buffer)

    // Hash table chaining (for quick lookup in the buffer cache)
    struct buffer *hash_next;

    // Doubly linked list pointers for LRU (Least Recently Used) eviction strategy
    struct buffer *prev; // Previous buffer in LRU list
    struct buffer *next; // Next buffer in LRU list

    // Add mutex/lock here if supporting concurrency
} buffer_t;

/*
 * Initialize the buffer cache subsystem.
 * Must be called once during kernel initialization.
 */
void buffer_cache_init(void);

/*
 * Retrieve a buffer for a given device name and block number.
 * If the block is not in the cache, it reads it from the disk.
 * This function increments the buffer's reference count. The caller
 * MUST call buffer_release() when done with the buffer.
 *
 * @param device_name Device identifier string (e.g., "hd0").
 * @param block_number Disk block number (LBA).
 * @return Pointer to buffer_t on success, or NULL on error (e.g., out of memory, I/O error).
 */
buffer_t *buffer_get(const char *device_name, uint32_t block_number);

/*
 * Release a previously acquired buffer (obtained via buffer_get).
 * Decrements the buffer's reference count. If the count reaches zero,
 * the buffer becomes a candidate for eviction (e.g., via LRU).
 *
 * @param buf Pointer to the buffer_t to release.
 */
void buffer_release(buffer_t *buf);

/*
 * Mark a buffer as dirty (modified).
 * Dirty buffers will be written back to disk during sync or eviction.
 *
 * @param buf Pointer to the buffer_t to mark as dirty.
 */
void buffer_mark_dirty(buffer_t *buf);

/*
 * Flush a single buffer to disk if it is dirty and valid.
 * Clears the dirty flag upon successful write.
 *
 * @param buf Pointer to the buffer_t to flush.
 * @return 0 on success or if the buffer wasn't dirty/valid, negative error code on write failure.
 */
int buffer_flush(buffer_t *buf);

/*
 * Synchronize the entire buffer cache.
 * Writes all dirty buffers with a reference count of zero back to their respective disks.
 */
void buffer_cache_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* BUFFER_CACHE_H */