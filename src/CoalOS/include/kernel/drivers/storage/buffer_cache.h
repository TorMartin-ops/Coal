/**
 * @file buffer_cache.h
 * @brief Write-back sector cache. One buffer per (disk, sector), LRU eviction.
 *
 * buffer_get() returns a referenced buffer holding the sector's data; call
 * buffer_mark_dirty() after modifying it and buffer_release() when done.
 * Dirty data reaches the disk on eviction, buffer_flush() or buffer_cache_sync().
 */
#ifndef BUFFER_CACHE_H
#define BUFFER_CACHE_H

#include <kernel/core/types.h>
#include <kernel/drivers/storage/disk.h>

#define BUFFER_FLAG_VALID 0x01 /* data matches (or is newer than) the disk */
#define BUFFER_FLAG_DIRTY 0x02 /* needs writing back */
#define BUFFER_FLAG_BUSY  0x04 /* disk I/O in progress */
#define BUFFER_FLAG_ERROR 0x08 /* the initial read failed */

#define MAX_BUFFER_BLOCK_SIZE 8192

typedef struct buffer buffer_t;

struct buffer {
    disk_t   *disk;
    uint32_t  block_number;
    uint8_t  *data;
    uint32_t  flags;
    uint32_t  ref_count;
    uint32_t  sync_gen;       /* last buffer_cache_sync() pass that visited this buffer */
    buffer_t *hash_next;
    buffer_t *lru_prev;       /* towards the most recently used */
    buffer_t *lru_next;
};

/** Makes a disk reachable by name through buffer_get(). */
int buffer_register_disk(disk_t *disk);

buffer_t *buffer_get(const char *device_name, uint32_t block_number);
buffer_t *buffer_get_disk(disk_t *disk, uint32_t block_number);
void buffer_release(buffer_t *buf);
void buffer_mark_dirty(buffer_t *buf);

/** Forgets the cached sectors [block, block+count) of a disk, dirty or not (their contents are dead). */
void buffer_discard(disk_t *disk, uint32_t block, uint32_t count);

/** Writes one buffer back if it is dirty. */
int buffer_flush(buffer_t *buf);

/** Writes back every dirty buffer, then flushes the drives' write caches. 0 or a negative error. */
int buffer_cache_sync(void);

#endif /* BUFFER_CACHE_H */
