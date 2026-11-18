/**
 * @file lockdep.c
 * @brief Lock-order and atomic-context checker (see lockdep.h).
 *
 * One CPU, and every spinlock holder runs with interrupts off, so one global stack of held locks is exact:
 * an interrupt handler only ever runs with nothing held and leaves with nothing held.
 */
#include <kernel/debug/lockdep.h>
#include <kernel/debug/ksan.h>
#include <kernel/sync/spinlock.h>

#define MAX_CLASSES 64
#define MAX_HELD    LOCKDEP_MAX_HELD

static struct { const char *site; uint64_t after; uint64_t reported; } g_class[MAX_CLASSES] = { { "unclassified", 0, 0 } };
static unsigned g_nclasses = 1;                 /* class 0 is "no class yet" */
static struct lockdep_state g_st;               /* the running task's */
#define g_held      g_st.held
#define g_nheld     g_st.nheld
#define g_irq_depth g_st.irq_depth

void lockdep_switch(struct lockdep_state *prev, const struct lockdep_state *next) {
    *prev = g_st;
    g_st = *next;
}

extern char _kernel_data_start[], _kernel_data_end[];

static uint16_t new_class(const char *site) {
    for (unsigned i = 1; i < g_nclasses; i++)           /* the same site is the same class */
        if (g_class[i].site == site)
            return (uint16_t)i;
    if (g_nclasses == MAX_CLASSES)
        return 0;
    g_class[g_nclasses].site = site;
    return (uint16_t)g_nclasses++;
}

void lockdep_class_init(void *lock, uint16_t *cls, const char *site) {
    uintptr_t flags = local_irq_save();

    *cls = new_class(site);
    local_irq_restore(flags);
    (void)lock;
}

/* A static lock nobody initialised is its own class, named by its address. */
static uint16_t class_of(void *lock, uint16_t *cls) {
    static char names[MAX_CLASSES][24];

    if (*cls || (uintptr_t)lock < (uintptr_t)_kernel_data_start || (uintptr_t)lock >= (uintptr_t)_kernel_data_end)
        return *cls;
    unsigned idx = g_nclasses;
    if (idx == MAX_CLASSES)
        return 0;
    char *n = names[idx];
    uintptr_t a = (uintptr_t)lock;
    n[0] = 's'; n[1] = 't'; n[2] = '@';
    for (int i = 0; i < 16; i++)
        n[3 + i] = "0123456789abcdef"[(a >> (60 - 4 * i)) & 15];
    n[19] = 0;
    g_class[idx].site = n;
    g_nclasses++;
    return *cls = (uint16_t)idx;
}

static bool reaches(uint16_t from, uint16_t to) {
    uint64_t seen = 1ull << from, todo = 1ull << from;

    while (todo) {
        unsigned c = (unsigned)__builtin_ctzll(todo);
        todo &= todo - 1;
        if (g_class[c].after & (1ull << to))
            return true;
        uint64_t next = g_class[c].after & ~seen;
        seen |= next;
        todo |= next;
    }
    return false;
}

static const char *name_of(uintptr_t ip, uintptr_t *off) {
    uintptr_t size;
    const char *n = ksym_lookup(ip, off, &size);

    return n ? n : "?";
}

void lockdep_acquire(void *lock, uint16_t *cls, uintptr_t ip) {
    uint16_t c = class_of(lock, cls);
    uintptr_t off;

    for (unsigned i = 0; i < g_nheld; i++)
        if (g_held[i].lock == lock) {
            const char *n = name_of(g_held[i].ip, &off);
            if (ksan_begin(KSAN_LOCK, "recursive acquire of %p (%s), already held since %s+%#lx: deadlock",
                           lock, g_class[c].site, n, (unsigned long)off))
                ksan_halt("LOCKDEP: recursive acquire");
            return;
        }
    if (c) {
        for (unsigned i = 0; i < g_nheld; i++) {
            uint16_t h = g_held[i].cls;
            if (!h || h == c)
                continue;
            if (reaches(c, h) && !(g_class[h].reported & (1ull << c))) {
                const char *n = name_of(g_held[i].ip, &off);
                g_class[h].reported |= 1ull << c;
                if (ksan_begin(KSAN_LOCK, "lock order inversion: %s taken while holding %s (held since %s+%#lx), "
                               "but %s was elsewhere taken while holding %s", g_class[c].site, g_class[h].site, n,
                               (unsigned long)off, g_class[h].site, g_class[c].site))
                    ksan_end(KSAN_LOCK);
            }
            g_class[h].after |= 1ull << c;
        }
    }
    if (g_nheld < MAX_HELD) {
        g_held[g_nheld].lock = lock;
        g_held[g_nheld].cls = c;
        g_held[g_nheld].ip = ip;
    }
    g_nheld++;
}

void lockdep_release(void *lock, uint16_t cls) {
    for (unsigned i = g_nheld > MAX_HELD ? MAX_HELD : g_nheld; i-- > 0;)
        if (g_held[i].lock == lock) {
            for (unsigned j = i; j + 1 < MAX_HELD && j + 1 < g_nheld; j++)
                g_held[j] = g_held[j + 1];
            g_nheld--;
            return;
        }
    if (g_nheld > MAX_HELD) {
        g_nheld--;
        return;
    }
    ksan_report(KSAN_LOCK, "release of %p (%s), which is not held", lock, g_class[cls].site);
}

void lockdep_irq_enter(void) { g_irq_depth++; }
void lockdep_irq_exit(void) { g_irq_depth--; }

void lockdep_might_sleep(const char *what) {
    uintptr_t off;

    if (g_irq_depth)
        ksan_report(KSAN_LOCK, "%s may sleep inside an interrupt handler", what);
    else if (g_nheld) {
        const char *n = name_of(g_held[g_nheld - 1 < MAX_HELD ? g_nheld - 1 : MAX_HELD - 1].ip, &off);
        ksan_report(KSAN_LOCK, "%s may sleep with %u spinlock(s) held (innermost taken at %s+%#lx)", what, g_nheld,
                    n, (unsigned long)off);
    }
}

void lockdep_schedule(void) {
    uintptr_t off;

    if (g_nheld) {
        const char *n = name_of(g_held[g_nheld - 1 < MAX_HELD ? g_nheld - 1 : MAX_HELD - 1].ip, &off);
        ksan_report(KSAN_LOCK, "schedule() with %u spinlock(s) held (innermost taken at %s+%#lx)", g_nheld, n,
                    (unsigned long)off);
    }
}

void lockdep_no_locks(const char *what) {
    if (g_nheld) {
        ksan_report(KSAN_LOCK, "%u spinlock(s) still held at %s", g_nheld, what);
        g_nheld = 0;                            /* resynchronise: the leaked lock would flag everything after it */
    }
}
