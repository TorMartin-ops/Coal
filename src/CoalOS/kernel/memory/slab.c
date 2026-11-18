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
#ifdef COAL_DEBUG_KERNEL
#include <kernel/memory/heapdebug.h>
#include <kernel/debug/ksan.h>
#include <kernel/lib/string.h>
#endif

struct slab {
    struct slab *next, *prev;   /* partial list */
    slab_cache_t *cache;
    void *free;                 /* allocation list */
    void *local_free;           /* objects freed since the lists were last merged */
    uint16_t used;              /* objects handed out and not yet freed */
    uint16_t bump;              /* offset of the first never-used object */
    uint8_t full;               /* on no list: nothing left to allocate */
};

#ifdef COAL_DEBUG_KERNEL
/* Debug layout: [header][16 B red zone]([object][trailer])..., the trailer starting with the red zone that
 * guards the end of the object and the one in front of the next. The free-list link lives in the trailer so a
 * freed object keeps its poison whole. */
#define SLAB_FIRST      (ALIGN_UP(sizeof(struct slab), 16) + HD_RZ_LEN)
#define STRIDE(c)       ((c)->obj_size + sizeof(struct hd_trailer))
#define TRAILER(c, o)   ((struct hd_trailer *)((char *)(o) + (c)->obj_size))
#define LINK(c, o)      (&TRAILER(c, o)->next)
#else
#define SLAB_FIRST      ALIGN_UP(sizeof(struct slab), 16)
#define STRIDE(c)       ((c)->obj_size)
#define LINK(c, o)      ((void **)(o))
#endif
#define SLAB_HEADER_SIZE SLAB_FIRST

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
    cache->limit = PAGE_SIZE - STRIDE(cache);   /* last offset an object can start at */
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
#ifdef COAL_DEBUG_KERNEL
    memset((char *)s + SLAB_FIRST - HD_RZ_LEN, HD_RZ_BYTE, HD_RZ_LEN);
    for (size_t off = SLAB_FIRST; off <= c->limit; off += STRIDE(c)) {
        struct hd_trailer *t = TRAILER(c, (char *)s + off);
        memset(t->rz, HD_RZ_BYTE, HD_RZ_LEN);
        memset(t->rz_next, HD_RZ_BYTE, HD_RZ_LEN);
        t->state = HD_FRESH;
    }
#endif
    return s;
}

/* Pop an object from the page's lists, merging the two when `free` is empty. */
static inline void *page_pop(slab_cache_t *c, struct slab *s) {
    void *obj = s->free;
    if (!obj) {
        obj = s->local_free;
        s->local_free = NULL;
    }
    if (obj) {
        s->free = *LINK(c, obj);
        s->used++;
    }
    return obj;
}

/* Both lists of the current page are empty: bump, or move to another page. */
static void *alloc_slow(slab_cache_t *c) {
    for (;;) {
        struct slab *s = c->cur;
        if (s) {
            void *obj = page_pop(c, s);
            if (obj)
                return obj;
            if (s->bump <= c->limit) {
                obj = (char *)s + s->bump;
                s->bump += STRIDE(c);
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

static void *alloc_obj(slab_cache_t *c) {
    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);
    struct slab *s = c->cur;
    void *obj = s ? page_pop(c, s) : NULL;
    if (!obj)
        obj = alloc_slow(c);
    spinlock_release_irqrestore(&c->lock, flags);
    return obj;
}

static void free_obj(slab_cache_t *c, struct slab *s, void *obj) {
    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);
    *LINK(c, obj) = s->local_free;
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

#ifndef COAL_DEBUG_KERNEL
void *slab_alloc(slab_cache_t *c) {
    return alloc_obj(c);
}

void slab_free(void *obj) {
    struct slab *s = slab_of(obj);
    if ((uintptr_t)obj - (uintptr_t)s < SLAB_HEADER_SIZE)
        KERNEL_PANIC_HALT("slab_free: not a slab object");
    free_obj(s->cache, s, obj);
}
#else
void *slab_alloc(slab_cache_t *c) {
    return slab_alloc_req(c, c->obj_size, NULL, 0);
}

void slab_free(void *obj) {
    slab_free_bt(obj, NULL, 0);
}

void *slab_alloc_req(slab_cache_t *c, size_t req, const uintptr_t *bt, unsigned n) {
    void *obj = alloc_obj(c);
    if (!obj)
        return NULL;
    struct hd_trailer *t = TRAILER(c, obj);
    if (t->state == HD_FREE)
        hd_check_poison(obj, c->obj_size, t, "slab");        /* a freed object that was written to */
    hd_on_alloc(obj, c->obj_size, req, t, bt, n);
    return obj;
}

/* The object really goes back to its slab: nobody can hold it any more. */
static void release(void *obj) {
    struct slab *s = slab_of(obj);
    slab_cache_t *c = s->cache;
    struct hd_trailer *t = TRAILER(c, obj);

    hd_check_poison(obj, c->obj_size, t, "quarantine");
    free_obj(c, s, obj);
}

void slab_free_bt(void *obj, const uintptr_t *bt, unsigned n) {
    struct slab *s = slab_of(obj);
    slab_cache_t *c = s->cache;
    uintptr_t off = (uintptr_t)obj - (uintptr_t)s;

    if (off < SLAB_FIRST || (off - SLAB_FIRST) % STRIDE(c) || off > c->limit) {
        hd_bad_free(obj, "pointer is not the start of a slab object");
        return;
    }
    struct hd_trailer *t = TRAILER(c, obj);
    struct hd_trailer *before = off == SLAB_FIRST ? NULL : TRAILER(c, (char *)obj - STRIDE(c));
    const uint8_t *front = off == SLAB_FIRST ? (const uint8_t *)obj - HD_RZ_LEN : before->rz_next;
    if (!hd_on_free(obj, c->obj_size, t, front, bt, n))
        return;
    void *evicted = hd_quarantine(obj);
    if (evicted)
        release(evicted);
}
#endif
