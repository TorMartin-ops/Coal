/**
 * @file slab.c
 * @brief Single-page slabs with sharded free lists (after mimalloc).
 *
 * Each page keeps two lists: `free`, which allocation pops, and `local_free`,
 * which frees push. Both are plain LIFO pushes and pops of a few instructions;
 * the lists meet only when `free` runs dry (Leijen, Zorn, de Moura,
 * "Mimalloc: Free List Sharding in Action", MSR-TR-2019-18). A fresh page hands
 * out objects by bumping an offset instead of threading a list through it.
 * A page that empties is returned to the buddy allocator at once, except the
 * cache's current page.
 */

#include <kernel/memory/slab.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/assert.h>

struct slab {
    struct slab *next, *prev;   /* partial list */
    slab_cache_t *cache;
    void *free;                 /* allocation list */
    void *local_free;           /* objects freed since the lists were last merged */
    uint16_t used;              /* objects handed out and not yet freed */
    uint16_t bump;              /* offset of the first never-used object */
    uint8_t full;               /* on no list: nothing left to allocate */
};

#define SLAB_HEADER_SIZE ALIGN_UP(sizeof(struct slab), 16)

static inline struct slab *slab_of(const void *obj) {
    return (struct slab *)((uintptr_t)obj & PAGING_PAGE_MASK);
}

static void partial_push(slab_cache_t *c, struct slab *s) {
    s->prev = NULL;
    s->next = c->partial;
    if (c->partial)
        c->partial->prev = s;
    c->partial = s;
}

static void partial_unlink(slab_cache_t *c, struct slab *s) {
    if (s->prev)
        s->prev->next = s->next;
    else
        c->partial = s->next;
    if (s->next)
        s->next->prev = s->prev;
}

void slab_cache_init(slab_cache_t *cache, size_t obj_size) {
    cache->obj_size = obj_size;
    cache->limit = PAGE_SIZE - obj_size;        /* last offset an object can start at */
    cache->cur = NULL;
    cache->partial = NULL;
    spinlock_init(&cache->lock);
}

static struct slab *slab_new(slab_cache_t *c) {
    uintptr_t phys = buddy_alloc_pages(0);
    if (!phys)
        return NULL;
    struct slab *s = phys_to_virt(phys);
    s->cache = c;
    s->free = s->local_free = NULL;
    s->used = 0;
    s->bump = SLAB_HEADER_SIZE;
    s->full = 0;
    return s;
}

/* Pop an object from the page's lists, merging the two when `free` is empty. */
static inline void *page_pop(struct slab *s) {
    void *obj = s->free;
    if (!obj) {
        obj = s->local_free;
        s->local_free = NULL;
    }
    if (obj) {
        s->free = *(void **)obj;
        s->used++;
    }
    return obj;
}

/* Both lists of the current page are empty: bump, or move to another page. */
static void *alloc_slow(slab_cache_t *c) {
    for (;;) {
        struct slab *s = c->cur;
        if (s) {
            void *obj = page_pop(s);
            if (obj)
                return obj;
            if (s->bump <= c->limit) {
                obj = (char *)s + s->bump;
                s->bump += c->obj_size;
                s->used++;
                return obj;
            }
            s->full = 1;
        }
        s = c->partial;
        if (s)
            partial_unlink(c, s);
        else if (!(s = slab_new(c))) {
            c->cur = NULL;
            return NULL;
        }
        c->cur = s;
    }
}

void *slab_alloc(slab_cache_t *c) {
    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);
    struct slab *s = c->cur;
    void *obj = s ? page_pop(s) : NULL;
    if (!obj)
        obj = alloc_slow(c);
    spinlock_release_irqrestore(&c->lock, flags);
    return obj;
}

void slab_free(void *obj) {
    struct slab *s = slab_of(obj);
    slab_cache_t *c = s->cache;
    if ((uintptr_t)obj - (uintptr_t)s < SLAB_HEADER_SIZE)
        KERNEL_PANIC_HALT("slab_free: not a slab object");

    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);
    *(void **)obj = s->local_free;
    s->local_free = obj;
    if (--s->used == 0 && s != c->cur) {
        if (!s->full)
            partial_unlink(c, s);
        buddy_free_pages(virt_to_phys(s));
    } else if (s->full) {
        s->full = 0;
        partial_push(c, s);
    }
    spinlock_release_irqrestore(&c->lock, flags);
}
