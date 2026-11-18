/**
 * @file kfence.c
 * @brief Sampling heap checker: single-object pages between guard pages (see kfence.h).
 *
 * Pool layout, in pages: G D G D ... G, slot i owning data page 2i+1. Guard pages are never mapped; a freed
 * slot's data page is unmapped too. The pool's frames are allocated once at boot. Bytes of a data page that
 * the object does not cover hold a canary pattern checked on free (it catches small overflows that stay inside
 * the page).
 */
#define LOG_MODULE "kfence"

#include <kernel/memory/kfence.h>
#include <kernel/debug/ksan.h>
#include <kernel/core/cmdline.h>
#include <kernel/core/log.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/heapdebug.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

enum { SLOT_UNUSED, SLOT_ALLOC, SLOT_FREED };

struct slot {
    uintptr_t frame;
    uintptr_t obj;
    uint32_t size;
    uint8_t state, right;               /* right: object ends at the end of its page */
    uintptr_t alloc_bt[KSAN_BT_MAX], free_bt[KSAN_BT_MAX];
};

unsigned g_kfence_interval;
unsigned g_kfence_countdown;

static struct slot g_slot[KFENCE_SLOTS];
static uint16_t g_avail[KFENCE_SLOTS];  /* FIFO of slots that can take an object: never used, or freed longest ago */
static unsigned g_avail_head, g_avail_count;
static uintptr_t g_trash;               /* frame behind a guard page after a report, so the access can go on */
static spinlock_t g_lock;
static bool g_ready;
static unsigned long g_allocs;

static inline uintptr_t data_page(unsigned i) { return KFENCE_BASE + (2ul * i + 1) * PAGE_SIZE; }
static inline uint8_t canary(uintptr_t a) { return (uint8_t)(0xaa ^ (a & 7)); }

static void map(uintptr_t va, uintptr_t frame) {
    pte_t *pte = paging_pte((void *)g_kernel_page_directory_phys, va, true);

    *pte = frame ? frame | PAGE_PRESENT | PAGE_RW | PAGE_NX : 0;
    paging_invalidate_page((void *)va);
}

void kfence_init(void) {
    char v[12];
    const char *end;
    uint64_t n = 0;                     /* off unless coal.kfence=N: its objects are not in the direct map, so no DMA from them */

    if (cmdline_get("coal.kfence", 0, v, sizeof(v)) && !cmdline_number(v, &end, &n))
        n = 0;
    g_kfence_interval = g_kfence_countdown = (unsigned)n;
    if (!n)
        return;
    g_trash = frame_alloc();
    for (unsigned i = 0; i < KFENCE_SLOTS && g_trash; i++) {
        g_slot[i].frame = frame_alloc();
        if (!g_slot[i].frame)
            break;
        g_avail[g_avail_count++] = (uint16_t)i;
    }
    if (!g_avail_count) {
        g_kfence_interval = 0;
        return;
    }
    g_ready = true;
    LOG_INFO("%u slots, one in %u allocations sampled", g_avail_count, g_kfence_interval);
}

void *kfence_alloc(size_t size, const uintptr_t *bt, unsigned n) {
    struct slot *s;
    unsigned idx;
    uintptr_t flags;

    g_kfence_countdown = g_kfence_interval;
    if (!g_ready || !size || size > PAGE_SIZE)
        return NULL;
    flags = spinlock_acquire_irqsave(&g_lock);
    if (!g_avail_count) {
        spinlock_release_irqrestore(&g_lock, flags);
        return NULL;
    }
    idx = g_avail[g_avail_head];
    g_avail_head = (g_avail_head + 1) % KFENCE_SLOTS;
    g_avail_count--;
    s = &g_slot[idx];
    s->state = SLOT_ALLOC;
    g_allocs++;
    spinlock_release_irqrestore(&g_lock, flags);

    /* Objects above 1 KiB stay page aligned (the kmalloc contract), so they only guard the front. */
    s->right = size <= 1024 && (g_allocs & 1);
    s->size = (uint32_t)size;
    uintptr_t page = data_page(idx);
    s->obj = s->right ? (page + PAGE_SIZE - size) & ~(uintptr_t)7 : page;
    map(page - PAGE_SIZE, 0);
    map(page + PAGE_SIZE, 0);
    map(page, s->frame);
    for (uintptr_t a = page; a < page + PAGE_SIZE; a++)
        *(uint8_t *)a = a >= s->obj && a < s->obj + size ? HD_POISON_ALLOC : canary(a);
    for (unsigned i = 0; i < KSAN_BT_MAX; i++)
        s->alloc_bt[i] = bt && i < n ? bt[i] : 0, s->free_bt[i] = 0;
    return (void *)s->obj;
}

static void report(const char *what, struct slot *s, uintptr_t addr) {
    if (!ksan_begin(KSAN_MEM, "KFENCE: %s at %#lx (object %#lx, size %u)", what, (unsigned long)addr,
                    (unsigned long)s->obj, s->size))
        return;
    ksan_trace("allocated by", s->alloc_bt, KSAN_BT_MAX);
    ksan_trace("freed by", s->free_bt, KSAN_BT_MAX);
    ksan_end(KSAN_MEM);
}

void kfence_free(void *p, const uintptr_t *bt, unsigned n) {
    unsigned idx = (unsigned)(((uintptr_t)p - KFENCE_BASE) / PAGE_SIZE / 2);
    struct slot *s = &g_slot[idx < KFENCE_SLOTS ? idx : 0];
    uintptr_t page = data_page(idx);

    if (idx >= KFENCE_SLOTS || (uintptr_t)p != s->obj || s->state == SLOT_UNUSED) {
        ksan_report(KSAN_MEM, "KFENCE: free of %p, which is not an allocated sampled object", p);
        return;
    }
    if (s->state == SLOT_FREED) {
        report("double free", s, (uintptr_t)p);
        return;
    }
    for (uintptr_t a = page; a < page + PAGE_SIZE; a++)
        if ((a < s->obj || a >= s->obj + s->size) && *(uint8_t *)a != canary(a)) {
            report(a < s->obj ? "buffer underflow (canary before the object corrupted)"
                              : "buffer overflow (canary after the object corrupted)", s, a);
            break;
        }
    memset((void *)s->obj, HD_POISON_FREE, s->size);
    for (unsigned i = 0; i < KSAN_BT_MAX; i++)
        s->free_bt[i] = bt && i < n ? bt[i] : 0;
    map(page, 0);
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    s->state = SLOT_FREED;
    g_avail[(g_avail_head + g_avail_count++) % KFENCE_SLOTS] = (uint16_t)idx;
    spinlock_release_irqrestore(&g_lock, flags);
}

bool kfence_handle_fault(uintptr_t addr, const isr_frame_t *f) {
    unsigned page, idx;
    struct slot *s;
    bool write = f->err_code & 2;

    if (!g_ready || !kfence_owns((void *)addr))
        return false;
    page = (unsigned)((addr - KFENCE_BASE) / PAGE_SIZE);
    if (page & 1) {                                     /* a data page: the object was freed */
        idx = page / 2;
        s = &g_slot[idx];
        report(write ? "use-after-free write" : "use-after-free read", s, addr);
        map(data_page(idx), s->frame);
    } else {                                            /* a guard page: the neighbour on one side overran */
        idx = page / 2;                                 /* slot to the right (underflow) is idx, to the left idx-1 */
        s = idx < KFENCE_SLOTS && g_slot[idx].state == SLOT_ALLOC && !g_slot[idx].right ? &g_slot[idx]
          : &g_slot[idx ? idx - 1 : 0];
        report(write ? (s == &g_slot[idx] ? "out-of-bounds write before the object" : "out-of-bounds write after the object")
                     : (s == &g_slot[idx] ? "out-of-bounds read before the object" : "out-of-bounds read after the object"),
               s, addr);
        map(KFENCE_BASE + (uintptr_t)page * PAGE_SIZE, g_trash);
    }
    return true;
}
