/**
 * @file pit.c
 * @brief PIT driver: 1000 Hz tick that also drives the scheduler.
 */
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/lib/port_io.h>
#include <kernel/process/scheduler.h>

#define PIT_BASE_HZ  1193182u
#define PIT_CH0      0x40
#define PIT_CMD      0x43
#define PIT_CMD_CH0_RATE_GEN 0x34 /* channel 0, lobyte/hibyte, mode 2 */

/* The counter is split so that readers on i386 can take a consistent
 * snapshot without disabling interrupts. */
static volatile uint32_t s_ticks_lo;
static volatile uint32_t s_ticks_hi;
static void (*s_hook)(void);

static void pit_irq_handler(isr_frame_t *frame) {
    (void)frame;
    if (++s_ticks_lo == 0) {
        s_ticks_hi++;
    }
    if (s_hook) {
        s_hook();
    }
    /* EOI first: scheduler_tick() may switch tasks and not return here. */
    pic_send_eoi(0);
    scheduler_tick();
}

void timer_set_hook(void (*fn)(void)) {
    s_hook = fn;
}

uint64_t timer_ticks(void) {
    uint32_t hi, lo;
    do {
        hi = s_ticks_hi;
        lo = s_ticks_lo;
    } while (hi != s_ticks_hi);
    return ((uint64_t)hi << 32) | lo;
}

_Static_assert(TIMER_HZ == 1000, "timer_uptime_ms() assumes a 1 ms tick");

uint64_t timer_uptime_ms(void) {
    return timer_ticks(); /* no 64-bit division in the kernel (no libgcc) */
}

void init_pit(void) {
    const uint16_t divisor = (uint16_t)(PIT_BASE_HZ / TIMER_HZ);
    register_int_handler(IRQ0_VECTOR, pit_irq_handler, NULL);
    outb(PIT_CMD, PIT_CMD_CH0_RATE_GEN);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));
}
