/**
 * @file timer.c
 * @brief Timing wheel (Varghese & Lauck, SOSP 1987) on a one-shot clock event device.
 *
 * Five levels of 64 slots; level n holds timers that expire less than 64^(n+1) ms ahead, in slots of
 * 64^n ms. When the time reaches the start of a level n slot its timers move down to finer levels
 * (cascade), so every timer fires on its exact millisecond. Per level a bitmap tells which slots are
 * used; that lets the next expiry be found without scanning, and lets time jump over idle stretches.
 */
#include <kernel/drivers/timer/timer.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/timer/lapic.h>
#include <kernel/lib/div64.h>
#include <kernel/process/scheduler.h>
#include <kernel/sync/spinlock.h>

#define LVL_BITS   6
#define LVL_SLOTS  (1u << LVL_BITS)
#define LEVELS     5
#define MAX_DELAY  ((1u << (LVL_BITS * LEVELS)) - 1)
#define HORIZON    0x40000000u          /* "no timer pending" */
#define MIN_DELTA_NS 2000u
#define NS_PER_MS  1000000u

static struct ktimer *wheel[LEVELS * LVL_SLOTS];
static uint32_t used[LEVELS][2];        /* bit i of level l: slot i is non-empty */
static uint32_t base;                   /* wheel time (ms) not yet processed */
static uint32_t next_event;             /* nothing needs attention before this wheel time: expiry or cascade */
static uint32_t next_due;               /* no timer expires before this wheel time; the interrupt is set for it */

static const struct clockevent *dev;
static uint64_t armed_ns;               /* absolute time the one-shot is armed for, 0: not armed */

/* ---- wheel --------------------------------------------------------------------------- */

static void slot_mark(unsigned slot, bool on) {
    uint32_t *w = &used[slot / LVL_SLOTS][(slot % LVL_SLOTS) / 32], bit = 1u << (slot % 32);

    if (on) *w |= bit; else *w &= ~bit;
}

static void enqueue(struct ktimer *t) {
    uint32_t delta = t->expires - base;
    unsigned lvl = 0;

    if ((int32_t)delta < 0) { t->expires = base; delta = 0; }
    while (lvl < LEVELS - 1 && delta >> (LVL_BITS * (lvl + 1))) lvl++;

    unsigned shift = LVL_BITS * lvl, slot = lvl * LVL_SLOTS + ((t->expires >> shift) & (LVL_SLOTS - 1));
    uint32_t seen_at = t->expires >> shift << shift;        /* a coarse slot is looked at when its block starts */

    t->next = wheel[slot];
    if (t->next) t->next->pprev = &t->next;
    wheel[slot] = t;
    t->pprev = &wheel[slot];
    t->slot = (uint16_t)slot;
    slot_mark(slot, true);
    if ((int32_t)(seen_at - next_event) < 0) next_event = seen_at;
    if ((int32_t)(t->expires - next_due) < 0) next_due = t->expires;
}

static void detach(struct ktimer *t) {
    *t->pprev = t->next;
    if (t->next) t->next->pprev = t->pprev;
    if (!wheel[t->slot]) slot_mark(t->slot, false);
    t->pprev = NULL;
}

/* Distance from slot `from` to the next used slot of a level (cyclic), -1 if none. */
static int next_used(const uint32_t bits[2], unsigned from) {
    uint64_t b = (uint64_t)bits[1] << 32 | bits[0];

    if (!b) return -1;
    if (from) b = b >> from | b << (64 - from);
    return (uint32_t)b ? __builtin_ctz((uint32_t)b) : 32 + __builtin_ctz((uint32_t)(b >> 32));
}

/* Soonest expiry in a slot's list. */
static uint32_t earliest(const struct ktimer *t) {
    uint32_t e = t->expires;

    for (t = t->next; t; t = t->next)
        if ((int32_t)(t->expires - e) < 0) e = t->expires;
    return e;
}

/* Recomputes next_event and next_due from the used slots. Coarse slots are cascaded when their block
 * starts (next_event), but their timers only expire later; the interrupt waits for the real expiry,
 * and the cascades are caught up then. */
static void find_next(void) {
    uint32_t event = base + HORIZON, due = event;

    for (unsigned lvl = 0; lvl < LEVELS; lvl++) {
        unsigned shift = LVL_BITS * lvl;
        unsigned lead = lvl && (base & ((1u << shift) - 1)) ? 1 : 0;    /* this block's slot was cascaded already */
        unsigned from = ((base >> shift) + lead) & (LVL_SLOTS - 1);
        int d = next_used(used[lvl], from);
        uint32_t at, exp;

        if (d < 0) continue;
        at = lvl ? ((base >> shift) + lead + (uint32_t)d) << shift : base + (uint32_t)d;
        exp = lvl ? earliest(wheel[lvl * LVL_SLOTS + ((from + (unsigned)d) & (LVL_SLOTS - 1))]) : at;
        if ((int32_t)(at - event) < 0) event = at;
        if ((int32_t)(exp - due) < 0) due = exp;
    }
    next_event = event;
    next_due = due;
}

/* Detaches a slot's list; the first node's pprev is pointed at *list so that timer_del() on a
 * member still works while the callbacks run. */
static struct ktimer *take(unsigned slot, struct ktimer **list) {
    *list = wheel[slot];
    wheel[slot] = NULL;
    slot_mark(slot, false);
    if (*list) (*list)->pprev = list;
    return *list;
}

static void cascade(unsigned lvl, unsigned idx) {
    struct ktimer *list, *t;

    take(lvl * LVL_SLOTS + idx, &list);
    while ((t = list)) {
        detach(t);
        enqueue(t);
    }
}

/* Processes wheel time `base`: cascades if a block starts, then runs the expired timers. */
static void step(void) {
    uint32_t now = base;
    unsigned idx = now & (LVL_SLOTS - 1);
    struct ktimer *list, *t;

    for (unsigned lvl = 1; !idx && lvl < LEVELS; lvl++) {
        idx = (now >> (LVL_BITS * lvl)) & (LVL_SLOTS - 1);
        cascade(lvl, idx);
    }
    base = now + 1;                     /* timers re-armed by callbacks land after this millisecond */
    take(now & (LVL_SLOTS - 1), &list);
    while ((t = list)) {
        detach(t);
        t->fn(t);
    }
}

static void wheel_run(uint32_t now) {
    while ((int32_t)(now - base) >= 0) {
        if ((int32_t)(next_event - base) > 0) {         /* idle stretch: skip it */
            base = (int32_t)(next_event - now) > 0 ? now + 1 : next_event;
            continue;
        }
        step();
        find_next();
    }
}

/* Asks the device for an interrupt when wheel time `at` begins; now_ms is the current time, 64 bit. */
static void request_wheel(uint64_t now_ms, uint32_t at) {
    int32_t ahead = (int32_t)(at - (uint32_t)now_ms);

    if ((int32_t)(at - base) >= (int32_t)HORIZON) return;  /* wheel is empty */
    clockevent_request((now_ms + (ahead > 0 ? (uint32_t)ahead : 0)) * NS_PER_MS);
}

static void start(struct ktimer *t, uint32_t ms, bool only_earlier) {
    uintptr_t flags = local_irq_save();
    uint64_t now_ns = clock_ns(), now_ms = div64_32(now_ns, NS_PER_MS);
    uint32_t now = (uint32_t)now_ms;
    uint32_t expires = (uint32_t)div64_32(now_ns + NS_PER_MS - 1, NS_PER_MS) + (ms > MAX_DELAY ? MAX_DELAY : ms);

    if (t->pprev) {
        if (only_earlier && (int32_t)(t->expires - expires) <= 0) goto out;
        detach(t);
    }
    if ((int32_t)(now - base) > 0 && (int32_t)(next_event - now) >= 0) base = now;     /* nothing was due: catch up */
    t->expires = expires;
    enqueue(t);
    request_wheel(now_ms, next_due);
out:
    local_irq_restore(flags);
}

void timer_add(struct ktimer *t, uint32_t ms) {
    start(t, ms, false);
}

void timer_add_earlier(struct ktimer *t, uint32_t ms) {
    start(t, ms, true);
}

bool timer_del(struct ktimer *t) {
    uintptr_t flags = local_irq_save();
    bool was_pending = t->pprev != NULL;

    if (was_pending) detach(t);
    local_irq_restore(flags);
    return was_pending;
}

/* ---- clock event device -------------------------------------------------------------- */

void clockevent_request(uint64_t abs_ns) {
    uintptr_t flags = local_irq_save();

    if (dev && dev->arm && (!armed_ns || abs_ns < armed_ns)) {
        uint64_t now = clock_ns(), delta = abs_ns > now ? abs_ns - now : 0;

        if (delta > dev->max_delta_ns) delta = dev->max_delta_ns;
        if (delta < MIN_DELTA_NS) delta = MIN_DELTA_NS;
        armed_ns = now + delta;
        dev->arm((uint32_t)delta);
    }
    local_irq_restore(flags);
}

void timer_interrupt(void) {
    uint64_t now_ns = clock_ns(), now_ms = div64_32(now_ns, NS_PER_MS);

    armed_ns = 0;
    wheel_run((uint32_t)now_ms);
    request_wheel(now_ms, next_due);
    scheduler_timer_event(now_ns);      /* may switch tasks and return much later */
}

void timer_init(void) {
    dev = lapic_clockevent_init();
    if (!dev) dev = pit_clockevent_init();
    base = (uint32_t)div64_32(clock_ns(), NS_PER_MS);
    next_event = next_due = base + HORIZON;
}

const char *timer_device_name(void) {
    return dev ? dev->name : "none";
}
