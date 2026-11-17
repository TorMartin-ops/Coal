/**
 * @file buffer_cache.c
 * @brief Sector cache (see buffer_cache.h).
 *
 * One lock protects the hash table, the LRU list and buffer flags. It is never
 * held across disk I/O: a buffer being read or written is marked BUSY and other
 * users of that buffer wait for it. A missing buffer is inserted as a BUSY
 * placeholder before its read starts, so two tasks never load the same sector.
 */
#define LOG_MODULE "bcache"

#include <kernel/drivers/storage/buffer_cache.h>
#include <kernel/core/log.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/process/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

#define HASH_BITS    8
#define HASH_SIZE    (1u << HASH_BITS)
#define MAX_BUFFERS  1024 /* soft limit: exceeded only when every buffer is in use */
#define MAX_DISKS    8

static spinlock_t g_lock;
static bool       g_init;
static buffer_t  *g_hash[HASH_SIZE];
static buffer_t  *g_lru_head, *g_lru_tail; /* most / least recently used */
static uint32_t   g_count;
static uint32_t   g_sync_gen;
static disk_t    *g_disks[MAX_DISKS];
static int        g_ndisks;

/* ---- Lock helpers: release/reacquire around I/O keep the saved IRQ flags up to date ---- */

#define LOCK(fl)   ((fl) = spinlock_acquire_irqsave(&g_lock))
#define UNLOCK(fl) spinlock_release_irqrestore(&g_lock, (fl))

static void wait_a_bit(void) {
    if (scheduler_is_ready()) yield();
    else asm volatile("pause");
}

/* ---- Hash and LRU (lock held) ------------------------------------------ */

static uint32_t hash_of(const disk_t *disk, uint32_t block) {
    return (((uint32_t)(uintptr_t)disk >> 4) ^ block) * 2654435761u >> (32 - HASH_BITS);
}

static buffer_t *lookup(const disk_t *disk, uint32_t block) {
    for (buffer_t *b = g_hash[hash_of(disk, block)]; b; b = b->hash_next) {
        if (b->disk == disk && b->block_number == block) return b;
    }
    return NULL;
}

static void lru_unlink(buffer_t *b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next; else g_lru_head = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev; else g_lru_tail = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}

static void lru_push_front(buffer_t *b) {
    b->lru_prev = NULL;
    b->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = b; else g_lru_tail = b;
    g_lru_head = b;
}

static void lru_touch(buffer_t *b) {
    if (b != g_lru_head) {
        lru_unlink(b);
        lru_push_front(b);
    }
}

static void cache_insert(buffer_t *b) {
    uint32_t h = hash_of(b->disk, b->block_number);
    b->hash_next = g_hash[h];
    g_hash[h] = b;
    lru_push_front(b);
    g_count++;
}

static void cache_remove(buffer_t *b) {
    buffer_t **pp = &g_hash[hash_of(b->disk, b->block_number)];
    while (*pp != b) pp = &(*pp)->hash_next;
    *pp = b->hash_next;
    lru_unlink(b);
    g_count--;
}

/* ---- I/O around the lock ------------------------------------------------ */

/** Waits for a BUSY buffer. The caller holds a reference, so it cannot go away. */
static void wait_not_busy(buffer_t *b, uintptr_t *fl) {
    while (b->flags & BUFFER_FLAG_BUSY) {
        UNLOCK(*fl);
        wait_a_bit();
        LOCK(*fl);
    }
}

/** Writes a dirty buffer to disk. The caller holds a reference. Dirty is restored on failure. */
static int write_back(buffer_t *b, uintptr_t *fl) {
    wait_not_busy(b, fl);
    if (!(b->flags & BUFFER_FLAG_DIRTY)) return 0;

    b->flags = (b->flags & ~BUFFER_FLAG_DIRTY) | BUFFER_FLAG_BUSY;
    UNLOCK(*fl);
    int r = disk_write_raw_sectors(b->disk, b->block_number, b->data, 1);
    LOCK(*fl);
    b->flags &= ~BUFFER_FLAG_BUSY;
    if (r != 0) b->flags |= BUFFER_FLAG_DIRTY;
    return r;
}

/** Frees one unreferenced buffer, writing it back first if needed. False if none can go. */
static bool evict_one(uintptr_t *fl) {
    for (buffer_t *b = g_lru_tail; b; b = b->lru_prev) {
        if (b->ref_count || (b->flags & BUFFER_FLAG_BUSY)) continue;
        if (b->flags & BUFFER_FLAG_DIRTY) {
            b->ref_count++;
            int r = write_back(b, fl);
            b->ref_count--;
            return r == 0; /* clean now; the caller rescans */
        }
        cache_remove(b);
        kfree(b);
        return true;
    }
    return false;
}

static buffer_t *alloc_buffer(disk_t *disk, uint32_t block) {
    buffer_t *b = kmalloc(sizeof(*b) + disk->blk_dev.sector_size);
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->disk = disk;
    b->block_number = block;
    b->data = (uint8_t *)(b + 1);
    return b;
}

/* ---- Public API ---------------------------------------------------------- */

int buffer_register_disk(disk_t *disk) {
    if (!disk || !disk->initialized || disk->blk_dev.sector_size > MAX_BUFFER_BLOCK_SIZE) {
        return BLOCK_ERR_PARAMS;
    }
    if (!g_init) {
        spinlock_init(&g_lock);
        g_init = true;
    }
    for (int i = 0; i < g_ndisks; i++) {
        if (g_disks[i] == disk) return 0;
    }
    if (g_ndisks == MAX_DISKS) return BLOCK_ERR_UNSUPPORTED;
    g_disks[g_ndisks++] = disk;
    return 0;
}

buffer_t *buffer_get_disk(disk_t *disk, uint32_t block) {
    if (!disk || !disk->initialized || !g_init) return NULL;

    buffer_t *spare = NULL;
    uintptr_t fl;
    LOCK(fl);
    for (;;) {
        buffer_t *b = lookup(disk, block);
        if (b) {
            b->ref_count++;
            lru_touch(b);
            wait_not_busy(b, &fl);
            if (b->flags & BUFFER_FLAG_ERROR) { /* the loader failed; it already unhashed b */
                if (--b->ref_count == 0) kfree(b);
                b = NULL;
            }
            UNLOCK(fl);
            if (spare) kfree(spare);
            return b;
        }
        if (g_count >= MAX_BUFFERS && evict_one(&fl)) continue; /* the lock may have been dropped */
        if (!spare) {
            UNLOCK(fl);
            spare = alloc_buffer(disk, block);
            if (!spare) return NULL;
            LOCK(fl);
            continue; /* rescan: someone may have loaded it meanwhile */
        }
        break;
    }

    /* Miss: publish a BUSY placeholder, then read without the lock. */
    buffer_t *b = spare;
    b->ref_count = 1;
    b->flags = BUFFER_FLAG_BUSY;
    cache_insert(b);
    UNLOCK(fl);

    int r = disk_read_raw_sectors(disk, block, b->data, 1);

    LOCK(fl);
    b->flags &= ~BUFFER_FLAG_BUSY;
    if (r == 0) {
        b->flags |= BUFFER_FLAG_VALID;
    } else {
        b->flags |= BUFFER_FLAG_ERROR;
        cache_remove(b);
        if (--b->ref_count == 0) kfree(b);
        b = NULL;
    }
    UNLOCK(fl);
    return b;
}

buffer_t *buffer_get(const char *device_name, uint32_t block) {
    for (int i = 0; device_name && i < g_ndisks; i++) {
        if (strcmp(g_disks[i]->blk_dev.device_name, device_name) == 0) {
            return buffer_get_disk(g_disks[i], block);
        }
    }
    return NULL;
}

void buffer_release(buffer_t *buf) {
    if (!buf) return;
    uintptr_t fl;
    LOCK(fl);
    if (buf->ref_count && --buf->ref_count == 0 && (buf->flags & BUFFER_FLAG_ERROR)) {
        kfree(buf); /* failed load, already out of the cache */
    }
    UNLOCK(fl);
}

void buffer_mark_dirty(buffer_t *buf) {
    if (!buf) return;
    uintptr_t fl;
    LOCK(fl);
    if (buf->flags & BUFFER_FLAG_VALID) buf->flags |= BUFFER_FLAG_DIRTY;
    UNLOCK(fl);
}

void buffer_discard(disk_t *disk, uint32_t block, uint32_t count) {
    if (!g_init) return;
    uintptr_t fl;
    LOCK(fl);
    for (uint32_t i = 0; i < count; i++) {
        buffer_t *b = lookup(disk, block + i);
        if (b && !b->ref_count && !(b->flags & BUFFER_FLAG_BUSY)) {
            cache_remove(b);
            kfree(b);
        }
    }
    UNLOCK(fl);
}

int buffer_flush(buffer_t *buf) {
    if (!buf) return BLOCK_ERR_PARAMS;
    uintptr_t fl;
    LOCK(fl);
    buf->ref_count++;
    int r = write_back(buf, &fl);
    buf->ref_count--;
    UNLOCK(fl);
    return r;
}

int buffer_cache_sync(void) {
    int result = 0;
    uintptr_t fl;
    if (!g_init) return 0;

    LOCK(fl);
    uint32_t gen = ++g_sync_gen;
    for (uint32_t h = 0; h < HASH_SIZE; h++) {
        for (;;) {
            buffer_t *b = g_hash[h];
            while (b && (!(b->flags & BUFFER_FLAG_DIRTY) || (b->flags & BUFFER_FLAG_BUSY) || b->sync_gen == gen)) {
                b = b->hash_next;
            }
            if (!b) break;
            b->sync_gen = gen;
            b->ref_count++;
            int r = write_back(b, &fl); /* may drop the lock: rescan the bucket afterwards */
            b->ref_count--;
            if (r) result = r;
        }
    }
    UNLOCK(fl);

    for (int i = 0; i < g_ndisks; i++) {
        int r = disk_flush(g_disks[i]);
        if (r) result = r;
    }
    if (result) LOG_ERROR("sync failed (err %d)", result);
    return result;
}
