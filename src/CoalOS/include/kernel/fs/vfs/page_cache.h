/**
 * @file page_cache.h
 * @brief Cache of 4 KiB file pages, keyed by (owner, key, page index).
 *
 * The owner is the file system instance and the key names a file inside it
 * (FAT: the first cluster). The cache only stores pages; the file system does
 * the I/O, under its own lock: it fills new pages, writes back dirty ones
 * (pc_first_dirty) and drops the pages of files that disappear. Clean, unpinned
 * pages are evicted in LRU order when the cache is full and when the OOM path
 * needs frames (pc_shrink); dirty pages never are.
 */
#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <kernel/core/types.h>

#define PC_PAGE_SIZE 4096u

typedef struct pc_page {
    uint8_t  *data;                 /* PC_PAGE_SIZE bytes */
    const void *owner;
    uint32_t key, idx;
    uint32_t pins;
    bool     dirty;
    struct pc_page *hnext;          /* hash chain */
    struct pc_page *lprev, *lnext;  /* LRU, most recent first */
    struct pc_page *dprev, *dnext;  /* dirty list, oldest first */
} pc_page_t;

/** The page, pinned and made most recent, or NULL. */
pc_page_t *pc_find(const void *owner, uint32_t key, uint32_t idx);

/** A new pinned page with undefined contents; the caller has checked that it is not cached. NULL if out of memory. */
pc_page_t *pc_add(const void *owner, uint32_t key, uint32_t idx);

void pc_put(pc_page_t *p);
void pc_mark_dirty(pc_page_t *p);
void pc_mark_clean(pc_page_t *p);

/** Removes unpinned pages idx in [from, to) of a file without writing them back. */
void pc_drop(const void *owner, uint32_t key, uint32_t from, uint32_t to);

/** Removes every unpinned page of the owner (unmount). */
void pc_drop_owner(const void *owner);

/** Oldest dirty page of the owner, pinned; NULL when none. */
pc_page_t *pc_first_dirty(const void *owner);

/** True when so many pages are dirty that the writer should flush before adding more. */
bool pc_too_dirty(void);

/** Frees up to n clean, unpinned pages (least recently used first); returns how many. Safe from the OOM path. */
size_t pc_shrink(size_t n);

#endif
