#pragma once
#ifndef BUFFER_CACHE_H
#define BUFFER_CACHE_H

#include "types.h" 
#ifdef __cplusplus
extern "C" {
#endif

/* Buffer flag definitions */
#define BUFFER_FLAG_VALID   0x01
#define BUFFER_FLAG_DIRTY   0x02

/**
 * buffer_t
 *
 * Represents a cached disk block.
 */
typedef struct buffer {
    uint32_t block_number;      // Disk block number
    const char *device;         // Device identifier (e.g., "ata0")
    uint8_t *data;              // Pointer to data (size is assumed BUFFER_BLOCK_SIZE)
    uint32_t flags;             // Status flags (valid, dirty)
    uint32_t ref_count;         // Reference count (for concurrency management)
    
    // Hash table chaining (for quick lookup in buffer cache)
    struct buffer *hash_next;
    
    // For future use: pointers for LRU or free list eviction
    struct buffer *prev;
    struct buffer *next;
} buffer_t;

/* 
 * Initialize the buffer cache subsystem.
 */
void buffer_cache_init(void);

/*
 * Retrieve a buffer for a given device and block number.
 * If not cached, the block is read from disk.
 * Increments the buffer's reference count.
 *
 * @param device      Device identifier string.
 * @param block_number Disk block number.
 * @return Pointer to buffer_t on success, or NULL on error.
 */
buffer_t *buffer_get(const char *device, uint32_t block_number);

/*
 * Release a previously acquired buffer (decrement its reference count).
 *
 * @param buf Pointer to the buffer.
 */
void buffer_release(buffer_t *buf);

/*
 * Mark a buffer as dirty so it will be flushed to disk.
 *
 * @param buf Pointer to the buffer.
 */
void buffer_mark_dirty(buffer_t *buf);

/*
 * Flush a single buffer to disk if it is dirty.
 *
 * @param buf Pointer to the buffer.
 * @return 0 on success, non-zero on error.
 */
int buffer_flush(buffer_t *buf);

/*
 * Synchronize the entire buffer cache by flushing all dirty buffers.
 */
void buffer_cache_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* BUFFER_CACHE_H */
