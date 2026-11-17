/**
 * @file slab.c
 * @brief Single-page slabs with intrusive free lists.
 */

#include <kernel/memory/slab.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/assert.h>

struct slab {
    struct slab *next, *prev;   /* partial list */
    slab_cache_t *cache;
    void *free;                 /* singly linked free objects */
    uint32_t inuse;
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
    cache->objs_per_slab = (PAGE_SIZE - SLAB_HEADER_SIZE) / obj_size;
    cache->partial = NULL;
    spinlock_init(&cache->lock);
}

static struct slab *slab_new(slab_cache_t *c) {
    uintptr_t phys = buddy_alloc_pages(0);
    if (!phys)
        return NULL;

    struct slab *s = phys_to_virt(phys);
    s->cache = c;
    s->inuse = 0;
    s->free = NULL;
    char *obj = (char *)s + SLAB_HEADER_SIZE;
    for (uint32_t i = c->objs_per_slab; i-- > 0;) {     /* ascending addresses */
        void *o = obj + (size_t)i * c->obj_size;
        *(void **)o = s->free;
        s->free = o;
    }
    return s;
}

void *slab_alloc(slab_cache_t *c) {
    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);

    struct slab *s = c->partial;
    if (!s) {
        s = slab_new(c);
        if (!s) {
            spinlock_release_irqrestore(&c->lock, flags);
            return NULL;
        }
        partial_push(c, s);
    }

    void *obj = s->free;
    s->free = *(void **)obj;
    if (++s->inuse == c->objs_per_slab)
        partial_unlink(c, s);               /* full */

    spinlock_release_irqrestore(&c->lock, flags);
    return obj;
}

void slab_free(void *obj) {
    struct slab *s = slab_of(obj);
    slab_cache_t *c = s->cache;
    size_t off = (uintptr_t)obj - (uintptr_t)s;
    if (off < SLAB_HEADER_SIZE || (off - SLAB_HEADER_SIZE) % c->obj_size)
        KERNEL_PANIC_HALT("slab_free: not a slab object");

    uintptr_t flags = spinlock_acquire_irqsave(&c->lock);

    bool was_full = (s->inuse == c->objs_per_slab);
    *(void **)obj = s->free;
    s->free = obj;
    s->inuse--;
    if (was_full)
        partial_push(c, s);

    /* Give an empty slab back unless it is the cache's only one. */
    if (s->inuse == 0 && (s->next || s->prev)) {
        partial_unlink(c, s);
        buddy_free_pages(virt_to_phys(s));
    }

    spinlock_release_irqrestore(&c->lock, flags);
}
