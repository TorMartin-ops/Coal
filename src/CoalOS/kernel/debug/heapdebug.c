/**
 * @file heapdebug.c
 * @brief Red zones, poisoning, double-free detection and the quarantine (see heapdebug.h).
 */
#include <kernel/memory/heapdebug.h>
#include <kernel/debug/ksan.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

#define QUARANTINE 256

static void *g_quar[QUARANTINE];
static unsigned g_quar_head, g_quar_count;
static spinlock_t g_quar_lock;

static void save_bt(uintptr_t *dst, const uintptr_t *bt, unsigned n) {
    for (unsigned i = 0; i < HD_BT; i++)
        dst[i] = bt && i < n ? bt[i] : 0;
}

/* Index of the first byte that differs from `pat`, or `len`. */
static size_t first_diff(const uint8_t *p, uint8_t pat, size_t len) {
    size_t i = 0;

    while (i < len && p[i] == pat)
        i++;
    return i;
}

static void report(const char *what, void *obj, const struct hd_trailer *t, const char *fmt, size_t arg) {
    if (!ksan_begin(KSAN_MEM, "%s: object %p (size %u) %s %lu", what, obj, t->req, fmt, (unsigned long)arg))
        return;
    ksan_trace("allocated by", t->alloc_bt, HD_BT);
    ksan_trace("last freed by", t->free_bt, HD_BT);
    ksan_end(KSAN_MEM);
}

void hd_on_alloc(void *obj, size_t cap, size_t req, struct hd_trailer *t, const uintptr_t *bt, unsigned n) {
    if (req > cap)
        req = cap;
    memset(obj, HD_POISON_ALLOC, cap);
    memset((uint8_t *)obj + req, HD_SLACK_BYTE, cap - req);
    t->req = (uint32_t)req;
    t->state = HD_ALLOC;
    save_bt(t->alloc_bt, bt, n);
}

bool hd_on_free(void *obj, size_t cap, struct hd_trailer *t, const uint8_t *front_rz, const uintptr_t *bt, unsigned n) {
    size_t i;

    if (t->state == HD_FREE) {
        report("heap double free", obj, t, "freed again, first free was", 0);
        return false;
    }
    if (t->state != HD_ALLOC) {
        report(t->state == HD_FRESH ? "heap invalid free (never allocated)" : "heap corruption (trailer trashed)",
               obj, t, "state", t->state);
        return false;
    }
    if ((i = first_diff(t->rz, HD_RZ_BYTE, HD_RZ_LEN)) < HD_RZ_LEN) {
        report("heap buffer overflow", obj, t, "right red zone corrupted at byte", i);
        memset(t->rz, HD_RZ_BYTE, HD_RZ_LEN);
    }
    if ((i = first_diff(front_rz, HD_RZ_BYTE, HD_RZ_LEN)) < HD_RZ_LEN) {
        report("heap buffer underflow", obj, t, "left red zone corrupted at byte", i);
        memset((uint8_t *)front_rz, HD_RZ_BYTE, HD_RZ_LEN);
    }
    if ((i = first_diff((uint8_t *)obj + t->req, HD_SLACK_BYTE, cap - t->req)) < cap - t->req) {
        report("heap buffer overflow", obj, t, "bytes past the requested size corrupted at offset", t->req + i);
    }
    memset(obj, HD_POISON_FREE, cap);
    t->state = HD_FREE;
    save_bt(t->free_bt, bt, n);
    return true;
}

void hd_check_poison(void *obj, size_t cap, struct hd_trailer *t, const char *where) {
    size_t i = first_diff(obj, HD_POISON_FREE, cap);

    if (i < cap) {
        report("heap use after free", obj, t, "written after free at offset", i);
        memset(obj, HD_POISON_FREE, cap);
    }
    (void)where;
}

void hd_bad_free(void *p, const char *why) {
    ksan_report(KSAN_MEM, "heap invalid free of %p: %s", p, why);
}

void *hd_quarantine(void *obj) {
    void *old = NULL;
    uintptr_t flags = spinlock_acquire_irqsave(&g_quar_lock);

    if (g_quar_count == QUARANTINE) {
        old = g_quar[g_quar_head];
        g_quar_count--;
        g_quar_head = (g_quar_head + 1) % QUARANTINE;
    }
    g_quar[(g_quar_head + g_quar_count++) % QUARANTINE] = obj;
    spinlock_release_irqrestore(&g_quar_lock, flags);
    return old;
}
