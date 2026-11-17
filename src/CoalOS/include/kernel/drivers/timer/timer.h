/**
 * @file timer.h
 * @brief Kernel timers and the clock event device behind them.
 *
 * Timers live in a hierarchical timing wheel (Varghese & Lauck, SOSP 1987): start, stop and expiry
 * are O(1) and the cost does not depend on the number of timers. The wheel runs on a one-shot clock
 * event device (TSC-deadline or one-shot local APIC timer, the PIT as fallback) that is programmed
 * for the earliest wheel expiry or time slice end only, so an idle CPU takes no timer interrupts.
 */
#ifndef KERNEL_DRIVERS_TIMER_TIMER_H
#define KERNEL_DRIVERS_TIMER_TIMER_H

#include <kernel/core/types.h>

struct ktimer {
    struct ktimer *next, **pprev;       /* wheel slot list; pprev == NULL: not pending */
    uint32_t expires;                   /* wheel time (ms) */
    uint16_t slot;
    void (*fn)(struct ktimer *);        /* runs in interrupt context: short, never sleeps */
};

/** Arms (or re-arms) t to run fn once, at least ms milliseconds from now. Safe from any context. */
void timer_add(struct ktimer *t, uint32_t ms);

/** Like timer_add() but keeps an earlier pending expiry. */
void timer_add_earlier(struct ktimer *t, uint32_t ms);

/** Cancels t. Returns true if it was pending. */
bool timer_del(struct ktimer *t);

static inline bool timer_pending(const struct ktimer *t) { return t->pprev != NULL; }

/* ---- clock event device ------------------------------------------------------------- */

struct clockevent {
    const char *name;
    uint32_t max_delta_ns;              /* longest one-shot the device can count */
    void (*arm)(uint32_t delta_ns);     /* interrupt once in delta_ns; NULL: free-running 1 kHz device */
};

/** Picks the clock event device and starts the wheel. After the memory manager and idt_init(), with interrupts off. */
void timer_init(void);

/** Name of the clock event device in use. */
const char *timer_device_name(void);

/** Makes sure an interrupt arrives at clock_ns() >= abs_ns (an earlier armed one is kept). */
void clockevent_request(uint64_t abs_ns);

/** The device's interrupt handlers call this after acknowledging the interrupt. */
void timer_interrupt(void);

#endif /* KERNEL_DRIVERS_TIMER_TIMER_H */
