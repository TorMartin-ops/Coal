/**
 * @file page_cache.c
 * @brief File page cache (see page_cache.h).
 *
 * One spinlock guards the hash, the LRU and dirty lists and the page flags. It
 * is held for a few pointer updates only, never across I/O or a copy: users
 * pin a page for that. Page contents belong to the file system's own lock.
 */
#include <kernel/fs/vfs/page_cache.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/sync/spinlock.h>

#define HASH_BITS 12
#define PC_PAGE_SHRINK 32
#define LOW_FREE_DIV   16   /* stop growing when less than 1/16 of lowmem is free */
#define MAX_DIV        2    /* at most half of lowmem */
#define MAX_DIRTY_DIV  8    /* at most 1/8 of lowmem dirty */

static spinlock_t g_lock;           /* zero-initialised: unlocked */
static pc_page_t *g_hash[1u << HASH_BITS];
static pc_page_t *g_lru_head, *g_lru_tail, *g_dirty_head, *g_dirty_tail;
static size_t g_count, g_ndirty;

#define LOCK()   spinlock_acquire_irqsave(&g_lock)
#define UNLOCK(fl) spinlock_release_irqrestore(&g_lock, (fl))

static uint32_t hash_of(const void *owner, uint32_t key, uint32_t idx) {
    return (((uint32_t)(uintptr_t)owner >> 4) + key * 2654435761u + idx * 40503u) * 2246822519u >> (32 - HASH_BITS);
}

static void lru_unlink(pc_page_t *p) {
    if (p->lprev) p->lprev->lnext = p->lnext; else g_lru_head = p->lnext;
    if (p->lnext) p->lnext->lprev = p->lprev; else g_lru_tail = p->lprev;
}

static void lru_push(pc_page_t *p) {
    p->lprev = NULL;
    p->lnext = g_lru_head;
    if (g_lru_head) g_lru_head->lprev = p; else g_lru_tail = p;
    g_lru_head = p;
}

static void clean_locked(pc_page_t *p) {
    if (!p->dirty) return;
    if (p->dprev) p->dprev->dnext = p->dnext; else g_dirty_head = p->dnext;
    if (p->dnext) p->dnext->dprev = p->dprev; else g_dirty_tail = p->dprev;
    p->dirty = false;
    g_ndirty--;
}

/* Unhash and free an unpinned page. */
static void destroy_locked(pc_page_t *p) {
    pc_page_t **pp = &g_hash[hash_of(p->owner, p->key, p->idx)];
    while (*pp != p) pp = &(*pp)->hnext;
    *pp = p->hnext;
    clean_locked(p);
    lru_unlink(p);
    g_count--;
    kfree(p->data);
    kfree(p);
}

static pc_page_t *find_locked(const void *owner, uint32_t key, uint32_t idx) {
    for (pc_page_t *p = g_hash[hash_of(owner, key, idx)]; p; p = p->hnext)
        if (p->idx == idx && p->key == key && p->owner == owner) return p;
    return NULL;
}

static size_t shrink_locked(size_t n) {
    size_t freed = 0;
    for (pc_page_t *p = g_lru_tail, *prev; p && freed < n; p = prev) {
        prev = p->lprev;
        if (p->pins || p->dirty) continue;
        destroy_locked(p);
        freed++;
    }
    return freed;
}

pc_page_t *pc_find(const void *owner, uint32_t key, uint32_t idx) {
    uintptr_t fl = LOCK();
    pc_page_t *p = find_locked(owner, key, idx);
    if (p) {
        p->pins++;
        if (p != g_lru_head) { lru_unlink(p); lru_push(p); }
    }
    UNLOCK(fl);
    return p;
}

pc_page_t *pc_add(const void *owner, uint32_t key, uint32_t idx) {
    pc_page_t *p = kmalloc(sizeof(*p));
    uint8_t *data = p ? kmalloc(PC_PAGE_SIZE) : NULL;
    if (p && !data) {                       /* lowmem is gone: give clean pages back and try once more */
        pc_shrink(PC_PAGE_SHRINK);
        data = kmalloc(PC_PAGE_SIZE);
    }
    if (!data) { kfree(p); return NULL; }

    size_t low = buddy_total_pages(ZONE_NORMAL);
    uintptr_t fl = LOCK();
    if (g_count >= low / MAX_DIV || buddy_free_pages_count(ZONE_NORMAL) < low / LOW_FREE_DIV) shrink_locked(1);
    p->data = data;
    p->owner = owner;
    p->key = key;
    p->idx = idx;
    p->pins = 1;
    p->dirty = false;
    uint32_t h = hash_of(owner, key, idx);
    p->hnext = g_hash[h];
    g_hash[h] = p;
    lru_push(p);
    g_count++;
    UNLOCK(fl);
    return p;
}

void pc_put(pc_page_t *p) {
    uintptr_t fl = LOCK();
    p->pins--;
    UNLOCK(fl);
}

void pc_mark_dirty(pc_page_t *p) {
    uintptr_t fl = LOCK();
    if (!p->dirty) {
        p->dirty = true;
        p->dnext = NULL;
        p->dprev = g_dirty_tail;
        if (g_dirty_tail) g_dirty_tail->dnext = p; else g_dirty_head = p;
        g_dirty_tail = p;
        g_ndirty++;
    }
    UNLOCK(fl);
}

void pc_mark_clean(pc_page_t *p) {
    uintptr_t fl = LOCK();
    clean_locked(p);
    UNLOCK(fl);
}

void pc_drop(const void *owner, uint32_t key, uint32_t from, uint32_t to) {
    uintptr_t fl = LOCK();
    for (uint32_t i = from; i < to; i++) {
        pc_page_t *p = find_locked(owner, key, i);
        if (p && !p->pins) destroy_locked(p);
    }
    UNLOCK(fl);
}

void pc_drop_owner(const void *owner) {
    uintptr_t fl = LOCK();
    for (pc_page_t *p = g_lru_tail, *prev; p; p = prev) {
        prev = p->lprev;
        if (p->owner == owner && !p->pins) destroy_locked(p);
    }
    UNLOCK(fl);
}

pc_page_t *pc_first_dirty(const void *owner) {
    uintptr_t fl = LOCK();
    pc_page_t *p = g_dirty_head;
    while (p && p->owner != owner) p = p->dnext;
    if (p) p->pins++;
    UNLOCK(fl);
    return p;
}

bool pc_too_dirty(void) { return g_ndirty > buddy_total_pages(ZONE_NORMAL) / MAX_DIRTY_DIV; }

size_t pc_shrink(size_t n) {
    uintptr_t fl = LOCK();
    size_t freed = shrink_locked(n);
    UNLOCK(fl);
    return freed;
}
